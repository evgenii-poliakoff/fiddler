// ProjectViewerDock — the right-side artifact viewer / inspector.
//
// Layout:
//
//   ┌─────────────────────────────────┐
//   │ Project                         │ ← QDockWidget title
//   ├─────────────────────────────────┤
//   │ ▾ Markers                       │
//   │   Mark 1     (1000 ms)          │ ← QTreeWidget; one entry per marker
//   │   Mark 2     (1500 ms)          │
//   │ ▾ Loops                         │
//   │   ▶ Loop 1   (1000–3000 ms)     │ ← ▶ glyph marks the armed loop
//   │   Loop 2     (5000–7000 ms)     │
//   ├─────────────────────────────────┤
//   │ Properties                      │
//   │   Name:     [Mark 1____]        │ ← QStackedWidget; per-type property page
//   │   Position: [1000   ↕]          │   (here, the Marker page)
//   └─────────────────────────────────┘
//
// MEMO: the dock is the home for "all transcription-project
// artifacts". Markers and loops are the first two categories;
// future steps may add notes, sections, etc. The QTreeWidget +
// QStackedWidget pattern is what lets each new category drop in
// alongside without restructuring.
//
// Selection is by stable artifact ID. Across categories the
// selection is mutually exclusive — the score widgets enforce the
// same rule (one of {barline, marker, loop} is selected at a time),
// and the dock mirrors that here so a single property page is
// always showing the right artifact's fields.
//
// Loop arming: this dock raises an `Armed` checkbox on the loop
// property page AND emits loopActivated on double-click. Both
// channels feed the same MainWindow handler so the user can use
// whichever idiom fits the moment (jump-and-play with a
// double-click; explicit toggle from the page).

#pragma once

#include <QDockWidget>

#include <cstdint>
#include <memory>
#include <optional>

class QCheckBox;
class QLineEdit;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace fiddler::score {
class LoopModel;
class MarkerModel;
}

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

    // Attach (or detach with nullptr) the loop model. Same lifecycle
    // contract as the marker model — writes happen via the model's
    // public API; the dock holds a non-const handle so it can call
    // setRange / rename / setPauseMs.
    void setLoopModel(std::shared_ptr<score::LoopModel> model);
    [[nodiscard]] std::shared_ptr<score::LoopModel>
        loopModel() const noexcept { return loopModel_; }

    [[nodiscard]] std::optional<std::int64_t>
        selectedMarkerId() const noexcept { return selectedMarkerId_; }
    [[nodiscard]] std::optional<std::int64_t>
        selectedLoopId() const noexcept { return selectedLoopId_; }
    [[nodiscard]] std::optional<std::int64_t>
        armedLoopId() const noexcept { return armedLoopId_; }

public slots:
    // Programmatic selection setter — used by MainWindow to keep
    // the dock in sync with the score widgets. Mutually exclusive
    // with setSelectedLoopId (and vice versa).
    void setSelectedMarkerId(std::optional<std::int64_t> id);
    void setSelectedLoopId  (std::optional<std::int64_t> id);

    // Pushed by MainWindow when the armed-loop state changes (the
    // user pressed Stop while looping, or armed a different loop).
    // The dock updates the tree row glyph and the Arm checkbox so
    // the UI matches transport state. nullopt = nothing armed.
    void setArmedLoopId(std::optional<std::int64_t> id);

signals:
    // Selection changed in the dock — by tree click, by programmatic
    // set, or as a side effect of mutual exclusion. MainWindow
    // forwards to the score widgets.
    void markerSelectionChanged(std::optional<std::int64_t> id);
    void loopSelectionChanged  (std::optional<std::int64_t> id);

    // The user double-clicked a marker entry — "jump and play".
    // MainWindow seeks the player to the marker and starts
    // playback. Different from markerSelectionChanged: that's a
    // passive selection update, this is a request to act.
    void markerActivated(std::int64_t id);

    // Loop double-click — same idiom, but the action is "arm this
    // loop and start playing it from startMs". MainWindow makes
    // that policy decision; we just relay the gesture.
    void loopActivated(std::int64_t id);

    // The user toggled the Arm checkbox in the loop property page.
    // `armed` is the *requested* new state (the property page already
    // reflects it; MainWindow updates the actual transport). Distinct
    // from loopActivated so MainWindow can disambiguate the gestures
    // in the event log.
    void loopArmToggleRequested(std::int64_t id, bool armed);

    // The user pressed Del on a focused tree entry. MainWindow turns
    // these into model->remove calls.
    void markerDeleteRequested(std::int64_t id);
    void loopDeleteRequested  (std::int64_t id);

    // Loop-creation gesture from the dock — Ctrl+left-click on a
    // marker row asks MainWindow to capture the *current* primary
    // anchor's ms as the secondary. The dock doesn't know what's
    // primary on the score widgets (it could be a barline), so the
    // signal is intentionally parameterless: MainWindow reads the
    // primary's ms from the waveform widget. Mirrors the Ctrl+click
    // gesture already on the score widgets.
    void loopAnchorAddRequested();
    // Plain left-click anywhere else in the tree clears any active
    // secondary anchor — same as a plain (non-Ctrl) click on the
    // score widgets.
    void loopAnchorClearRequested();

protected:
    // Forward Del key on the tree to the markerDeleteRequested
    // signal; let everything else fall through.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // Connected to MarkerModel::changed / LoopModel::changed —
    // rebuild the tree section for the changed kind, refresh the
    // property page if it's still showing a valid artifact.
    void onMarkerModelChanged();
    void onLoopModelChanged();

    // Connected to QTreeWidget::currentItemChanged — translate the
    // newly-current item (or null) into an artifact selection.
    void onTreeCurrentItemChanged(QTreeWidgetItem* current,
                                  QTreeWidgetItem* previous);

    // Connected to QTreeWidget::itemDoubleClicked — emit
    // markerActivated for marker rows, loopActivated for loop rows.
    void onTreeItemDoubleClicked(QTreeWidgetItem* item, int column);

    // Property page edits round-trip into the model. The
    // updatingPropertyPage_ guard suppresses re-emit when *we* set
    // the field values from the model (not when the user types).
    void onMarkerNameEdited();
    // MEMO: takes no `newMs` because we read the spinbox's value()
    // directly. We bind to editingFinished (not valueChanged) so
    // the user can type leading zeros while editing without each
    // keystroke round-tripping through the model and stripping
    // them. See the slot impl for the full reasoning.
    void onMarkerPositionEdited();

    // Loop property page slots. Range edits enforce the
    // start < end invariant by widening the partner box's range
    // bound; if a user types start > current end, end auto-bumps
    // to start + 1. Same editingFinished discipline as markers.
    void onLoopNameEdited();
    void onLoopStartEdited();
    void onLoopEndEdited();
    void onLoopPauseEdited();
    void onLoopArmedToggled(bool checked);

private:
    void buildUi();
    void rebuildMarkerSection();
    void rebuildLoopSection();
    void refreshPropertyPage();
    // Decide which loop-anchor signal to fire for a tree mouse-press.
    // Pulled out as a helper because eventFilter() should stay
    // narrowly focused on dispatch.
    void handleTreeMousePress(class QMouseEvent* me);

    // Find the QTreeWidgetItem (under the appropriate category) whose
    // user-data ID matches `id`. Returns nullptr if not found.
    [[nodiscard]] QTreeWidgetItem*
        findMarkerItem(std::int64_t id) const;
    [[nodiscard]] QTreeWidgetItem*
        findLoopItem(std::int64_t id) const;

    std::shared_ptr<score::MarkerModel> markerModel_;
    std::shared_ptr<score::LoopModel>   loopModel_;

    // Mutually exclusive — at most one of these has a value.
    std::optional<std::int64_t>         selectedMarkerId_;
    std::optional<std::int64_t>         selectedLoopId_;

    // Independent of selection — a loop can be armed without being
    // selected (the user could click another artifact while the loop
    // is still wrapping). nullopt means "nothing armed".
    std::optional<std::int64_t>         armedLoopId_;

    // Top-level items in the tree.
    QTreeWidget*     tree_              = nullptr;
    QTreeWidgetItem* markersCategory_   = nullptr;
    QTreeWidgetItem* loopsCategory_     = nullptr;

    // Property-page stack. Page indices live in the .cpp.
    QStackedWidget*  propertyStack_     = nullptr;

    // Marker page widgets.
    QLineEdit*       markerNameEdit_    = nullptr;
    QSpinBox*        markerPositionBox_ = nullptr;

    // Loop page widgets.
    QLineEdit*       loopNameEdit_      = nullptr;
    QSpinBox*        loopStartBox_      = nullptr;
    QSpinBox*        loopEndBox_        = nullptr;
    QSpinBox*        loopPauseBox_      = nullptr;
    QCheckBox*       loopArmedCheck_    = nullptr;

    // MEMO: invariant — set true while we're populating the
    // property-page widgets from the model, so their valueChanged
    // / editingFinished / toggled slots don't loop back into model
    // writes or re-emit user-intent signals. Reset before returning
    // from refreshPropertyPage() / setArmedLoopId().
    bool updatingPropertyPage_ = false;
};

} // namespace fiddler::ui
