// ProjectViewerDock — the right-side artifact viewer / inspector.
//
// Layout:
//
//   ┌───────────────────────────┐
//   │ Project                   │ ← QDockWidget title
//   ├───────────────────────────┤
//   │ ▾ Markers                 │
//   │   Mark 1   (1000 ms)      │ ← QTreeWidget; one entry per marker
//   │   Mark 2   (1500 ms)      │
//   │   …                       │
//   ├───────────────────────────┤
//   │ Properties                │
//   │   Name:     [Mark 1____]  │ ← QStackedWidget; per-type property page
//   │   Position: [1000   ↕]    │   (here, the Marker page)
//   └───────────────────────────┘
//
// MEMO: the dock is the home for "all transcription-project
// artifacts". Markers are the first category; loops (PR B) will
// add a second; future steps may add notes, sections, etc. The
// QTreeWidget + QStackedWidget pattern is what lets each new
// category drop in alongside without restructuring.
//
// Selection is by stable marker ID (mirrors WaveformWidget /
// StaffWidget). MainWindow keeps tree, score widgets, and dock in
// sync via the same `mirroringSelection_` guard pattern that
// already prevents double-firing among the score widgets.

#pragma once

#include <QDockWidget>

#include <cstdint>
#include <memory>
#include <optional>

class QLineEdit;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace fiddler::score { class MarkerModel; }

namespace fiddler::ui {

class ProjectViewerDock : public QDockWidget {
    Q_OBJECT
public:
    explicit ProjectViewerDock(QWidget* parent = nullptr);
    ~ProjectViewerDock() override;

    // Attach (or detach with nullptr) the marker model the dock
    // browses + edits. Writes go through the model's API
    // (rename / setPosition); the dock holds a non-const handle
    // because of those writes.
    void setMarkerModel(std::shared_ptr<score::MarkerModel> model);
    [[nodiscard]] std::shared_ptr<score::MarkerModel>
        markerModel() const noexcept { return markerModel_; }

    [[nodiscard]] std::optional<std::int64_t>
        selectedMarkerId() const noexcept { return selectedMarkerId_; }

public slots:
    // Programmatic selection setter — used by MainWindow to keep
    // the dock in sync with the score widgets.
    void setSelectedMarkerId(std::optional<std::int64_t> id);

signals:
    // The user clicked a marker entry in the tree (or cleared the
    // selection). MainWindow forwards to the score widgets and
    // seeks the player to the marker's source-time.
    void markerSelectionChanged(std::optional<std::int64_t> id);

    // The user double-clicked a marker entry — "jump and play".
    // MainWindow seeks the player to the marker and starts
    // playback. Different from markerSelectionChanged: that's a
    // passive selection update, this is a request to act.
    void markerActivated(std::int64_t id);

    // The user pressed Del while a marker entry was focused in the
    // tree. MainWindow turns this into a markerModel->remove() call.
    void markerDeleteRequested(std::int64_t id);

protected:
    // Forward Del key on the tree to the markerDeleteRequested
    // signal; let everything else fall through.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // Connected to MarkerModel::changed — rebuild the tree, refresh
    // the property page if it's still showing a valid marker.
    void onMarkerModelChanged();

    // Connected to QTreeWidget::currentItemChanged — translate the
    // newly-current item (or null) into a marker ID and emit
    // markerSelectionChanged.
    void onTreeCurrentItemChanged(QTreeWidgetItem* current,
                                  QTreeWidgetItem* previous);

    // Connected to QTreeWidget::itemDoubleClicked — emit
    // markerActivated if the item is a marker row.
    void onTreeItemDoubleClicked(QTreeWidgetItem* item, int column);

    // Property page edits round-trip into the model. The
    // updatingPropertyPage_ guard suppresses re-emit when *we* set
    // the field values from the model (not when the user types).
    void onNameEdited();
    // MEMO: takes no `newMs` because we read the spinbox's value()
    // directly. We bind to editingFinished (not valueChanged) so
    // the user can type leading zeros while editing without each
    // keystroke round-tripping through the model and stripping
    // them. See the slot impl for the full reasoning.
    void onPositionEdited();

private:
    void buildUi();
    void rebuildTree();
    void refreshPropertyPage();

    // Find the QTreeWidgetItem (under the Markers category) whose
    // user-data ID matches `id`. Returns nullptr if not found.
    [[nodiscard]] QTreeWidgetItem*
        findMarkerItem(std::int64_t id) const;

    std::shared_ptr<score::MarkerModel> markerModel_;
    std::optional<std::int64_t>         selectedMarkerId_;

    // Top-level items in the tree, kept around so we don't have to
    // search by text on each rebuild. Markers is the only category
    // for now; loops will add a second member here in PR B.
    QTreeWidget*     tree_              = nullptr;
    QTreeWidgetItem* markersCategory_   = nullptr;

    // Property-page stack. Index 0 = "no selection"; index 1 =
    // marker properties.
    QStackedWidget*  propertyStack_     = nullptr;
    QLineEdit*       markerNameEdit_    = nullptr;
    QSpinBox*        markerPositionBox_ = nullptr;

    // MEMO: invariant — set true while we're populating the
    // property-page widgets from the model, so their valueChanged
    // / editingFinished slots don't loop back into model writes.
    // Reset before returning from refreshPropertyPage().
    bool updatingPropertyPage_ = false;
};

} // namespace fiddler::ui
