#include "ui/ProjectViewerDock.h"

#include "score/MarkerModel.h"

#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <climits>

namespace fiddler::ui {

namespace {

// MEMO: user-data role for the marker ID stored on each tree item.
// Picked Qt::UserRole + 1 to leave the base UserRole free for any
// future "kind" tag we'd want when categories multiply.
constexpr int kMarkerIdRole = Qt::UserRole + 1;

// Property-stack page indices.
constexpr int kPageNoSelection = 0;
constexpr int kPageMarker      = 1;

} // namespace

// ---- construction -------------------------------------------------------

ProjectViewerDock::ProjectViewerDock(QWidget* parent)
    : QDockWidget(tr("Project"), parent)
{
    // MEMO: object name lets MainWindow + integration tests reach
    // the dock via findChild without coupling to the widget tree.
    setObjectName("projectViewerDock");
    buildUi();
}

ProjectViewerDock::~ProjectViewerDock() = default;

void ProjectViewerDock::buildUi() {
    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(4, 4, 4, 4);

    // ---- Tree of artifacts ----------------------------------------------
    tree_ = new QTreeWidget(root);
    tree_->setObjectName("projectViewerTree");
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    // MEMO: install ourselves as the tree's event filter so we can
    // intercept Del on focused items without subclassing QTreeWidget.
    tree_->installEventFilter(this);

    markersCategory_ = new QTreeWidgetItem(tree_);
    markersCategory_->setText(0, tr("Markers"));
    markersCategory_->setFlags(Qt::ItemIsEnabled);   // not selectable
    markersCategory_->setExpanded(true);

    connect(tree_, &QTreeWidget::currentItemChanged,
            this,  &ProjectViewerDock::onTreeCurrentItemChanged);
    connect(tree_, &QTreeWidget::itemDoubleClicked,
            this,  &ProjectViewerDock::onTreeItemDoubleClicked);

    layout->addWidget(tree_, /*stretch=*/1);

    // ---- Property pages -------------------------------------------------
    propertyStack_ = new QStackedWidget(root);
    propertyStack_->setObjectName("projectViewerPropertyStack");

    // Page 0: no selection — placeholder text so the area isn't
    // confusingly blank when nothing is selected.
    {
        auto* page = new QWidget(propertyStack_);
        auto* l    = new QVBoxLayout(page);
        l->setContentsMargins(4, 4, 4, 4);
        auto* hint = new QLabel(
            tr("Select a marker above to view its properties."), page);
        hint->setEnabled(false);
        l->addWidget(hint);
        l->addStretch();
        propertyStack_->insertWidget(kPageNoSelection, page);
    }

    // Page 1: marker properties — name + position-in-ms.
    {
        auto* page = new QWidget(propertyStack_);
        auto* form = new QFormLayout(page);
        form->setContentsMargins(4, 4, 4, 4);

        markerNameEdit_ = new QLineEdit(page);
        markerNameEdit_->setObjectName("markerNameEdit");
        connect(markerNameEdit_, &QLineEdit::editingFinished,
                this,            &ProjectViewerDock::onNameEdited);
        form->addRow(tr("Name:"), markerNameEdit_);

        markerPositionBox_ = new QSpinBox(page);
        markerPositionBox_->setObjectName("markerPositionBox");
        markerPositionBox_->setRange(0, INT_MAX);
        markerPositionBox_->setSuffix(tr(" ms"));
        // MEMO: bind to editingFinished (Enter or focus-loss) — NOT
        // valueChanged. valueChanged fires after every keystroke,
        // and that round-trips through the model
        // (onPositionEdited → setPosition → emit changed →
        // refreshPropertyPage → setValue), which forces the spinbox
        // to re-format its text on every edit. That re-formatting
        // strips leading zeros: editing "30004" by deleting the '3'
        // produces "0004" which gets re-parsed to 4 and re-rendered
        // as "4" — instead of the "0004" the user expects to keep
        // editing toward "40004". editingFinished commits only on
        // user intent (Enter / Tab / focus-loss), so the in-progress
        // text is left alone.
        connect(markerPositionBox_, &QSpinBox::editingFinished,
                this, &ProjectViewerDock::onPositionEdited);
        form->addRow(tr("Position:"), markerPositionBox_);

        propertyStack_->insertWidget(kPageMarker, page);
    }

    propertyStack_->setCurrentIndex(kPageNoSelection);
    layout->addWidget(propertyStack_);

    setWidget(root);
}

// ---- model attachment ---------------------------------------------------

void ProjectViewerDock::setMarkerModel(
    std::shared_ptr<score::MarkerModel> model)
{
    if (markerModel_) {
        disconnect(markerModel_.get(), nullptr, this, nullptr);
    }
    markerModel_ = std::move(model);
    if (markerModel_) {
        connect(markerModel_.get(), &score::MarkerModel::changed,
                this, &ProjectViewerDock::onMarkerModelChanged);
    }
    // Drop any stale selection before rebuilding — the tree may not
    // have an entry matching the previous ID anymore.
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    rebuildTree();
    refreshPropertyPage();
}

void ProjectViewerDock::setSelectedMarkerId(
    std::optional<std::int64_t> id)
{
    // Coerce a stale ID to nullopt so we never display a dangling
    // selection. Same defensive pattern as the score widgets.
    if (id.has_value() && markerModel_
        && !markerModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    if (selectedMarkerId_ == id) return;
    selectedMarkerId_ = id;

    // MEMO: temporarily block tree signals while we set the current
    // item — without this the currentItemChanged slot would fire
    // and *re-*emit markerSelectionChanged, creating a loop with
    // MainWindow's mirror plumbing.
    const QSignalBlocker treeBlock(tree_);
    if (id.has_value()) {
        if (auto* item = findMarkerItem(*id)) {
            tree_->setCurrentItem(item);
        } else {
            tree_->setCurrentItem(nullptr);
        }
    } else {
        tree_->setCurrentItem(nullptr);
    }

    refreshPropertyPage();
    emit markerSelectionChanged(selectedMarkerId_);
}

// ---- model → ui ---------------------------------------------------------

void ProjectViewerDock::onMarkerModelChanged() {
    // Validate the current selection against the new model state.
    // If the selected marker has gone (was removed), drop selection.
    // If it merely moved (setPosition), the ID is still valid — keep
    // selection as-is.
    if (selectedMarkerId_.has_value() && markerModel_
        && !markerModel_->indexOf(*selectedMarkerId_).has_value())
    {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    rebuildTree();
    refreshPropertyPage();
}

void ProjectViewerDock::rebuildTree() {
    // MEMO: block tree signals during the rebuild. Without this,
    // deleting the currently-selected QTreeWidgetItem fires
    // currentItemChanged(nullptr) which would wipe selectedMarkerId_
    // — turning a setPosition (which should preserve selection by ID)
    // into a selection-clearing event.
    const QSignalBlocker rebuildBlock(tree_);

    // Drop existing marker children. We don't preserve QTreeWidgetItem
    // pointers across rebuilds — they're cheap to recreate, and the
    // user-data role round-trips the marker ID either way.
    while (markersCategory_->childCount() > 0) {
        delete markersCategory_->takeChild(0);
    }
    if (!markerModel_) return;

    for (const auto& m : markerModel_->markers()) {
        auto* item = new QTreeWidgetItem(markersCategory_);
        // MEMO: user-visible text combines the user-given name with
        // the timestamp so the user can scan the list and locate
        // entries by either. Format mirrors what a transcription
        // editor reader would expect ("Mark 1   (1000 ms)").
        item->setText(0, QString("%1   (%2 ms)")
                            .arg(m.name)
                            .arg(m.sourceMs));
        item->setData(0, kMarkerIdRole,
                      QVariant::fromValue<std::int64_t>(m.id));
    }
    markersCategory_->setExpanded(true);

    // Re-apply the current selection in the rebuilt tree. The
    // outer rebuildBlock above keeps this from re-firing the
    // currentItemChanged slot.
    if (selectedMarkerId_.has_value()) {
        if (auto* item = findMarkerItem(*selectedMarkerId_)) {
            tree_->setCurrentItem(item);
        }
    }
}

void ProjectViewerDock::refreshPropertyPage() {
    if (!selectedMarkerId_.has_value() || !markerModel_) {
        propertyStack_->setCurrentIndex(kPageNoSelection);
        return;
    }
    const auto idx = markerModel_->indexOf(*selectedMarkerId_);
    if (!idx) {
        propertyStack_->setCurrentIndex(kPageNoSelection);
        return;
    }
    const auto& m = markerModel_->markers()[*idx];

    updatingPropertyPage_ = true;
    markerNameEdit_->setText(m.name);
    markerPositionBox_->setValue(static_cast<int>(m.sourceMs));
    updatingPropertyPage_ = false;

    propertyStack_->setCurrentIndex(kPageMarker);
}

QTreeWidgetItem*
ProjectViewerDock::findMarkerItem(std::int64_t id) const {
    for (int i = 0; i < markersCategory_->childCount(); ++i) {
        auto* child = markersCategory_->child(i);
        if (child->data(0, kMarkerIdRole).toLongLong() == id) {
            return child;
        }
    }
    return nullptr;
}

// ---- ui → model ---------------------------------------------------------

void ProjectViewerDock::onTreeCurrentItemChanged(
    QTreeWidgetItem* current, QTreeWidgetItem* /*previous*/)
{
    std::optional<std::int64_t> newId;
    if (current && current->parent() == markersCategory_) {
        newId = current->data(0, kMarkerIdRole).toLongLong();
    }
    if (selectedMarkerId_ == newId) return;
    selectedMarkerId_ = newId;
    refreshPropertyPage();
    emit markerSelectionChanged(selectedMarkerId_);
}

void ProjectViewerDock::onTreeItemDoubleClicked(
    QTreeWidgetItem* item, int /*column*/)
{
    // Only marker rows fire markerActivated — double-clicking the
    // category header (and any future non-marker row) is a no-op.
    if (!item || item->parent() != markersCategory_) return;
    const auto id = item->data(0, kMarkerIdRole).toLongLong();
    emit markerActivated(id);
}

void ProjectViewerDock::onNameEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedMarkerId_ || !markerModel_) return;
    markerModel_->rename(*selectedMarkerId_, markerNameEdit_->text());
    // Tree text refreshes via onMarkerModelChanged → rebuildTree.
}

void ProjectViewerDock::onPositionEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedMarkerId_ || !markerModel_) return;
    // editingFinished doesn't pass the new value, so we read it
    // directly from the spinbox. By the time this fires the user
    // has finished editing (Enter / focus-loss), so the value is
    // settled.
    markerModel_->setPosition(
        *selectedMarkerId_,
        static_cast<std::int64_t>(markerPositionBox_->value()));
}

// ---- input forwarding ---------------------------------------------------

bool ProjectViewerDock::eventFilter(QObject* watched, QEvent* event) {
    // MEMO: forward Del on a focused marker entry to the
    // markerDeleteRequested signal. Lets the same key work in the
    // dock as on the score widgets without a separate window-level
    // shortcut. Other keys fall through to the tree's defaults
    // (arrow nav, Tab, etc.).
    if (watched == tree_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete
            && selectedMarkerId_.has_value())
        {
            emit markerDeleteRequested(*selectedMarkerId_);
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

} // namespace fiddler::ui
