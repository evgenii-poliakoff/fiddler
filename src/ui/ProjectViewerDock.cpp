#include "ui/ProjectViewerDock.h"

#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "ui/LoopCountdownWidget.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <climits>

namespace fiddler::ui {

namespace {

// MEMO: user-data roles for the artifact ID stored on each tree
// item. Separate roles per kind let us use the *role itself* as a
// kind tag when interpreting a tree row — checking which role
// returns a valid value tells us whether the row is a marker or a
// loop. Marker rows only have kMarkerIdRole; loop rows only have
// kLoopIdRole.
constexpr int kMarkerIdRole = Qt::UserRole + 1;
constexpr int kLoopIdRole   = Qt::UserRole + 2;

// Property-stack page indices.
constexpr int kPageNoSelection = 0;
constexpr int kPageMarker      = 1;
constexpr int kPageLoop        = 2;

// Glyph prefix for the armed loop's tree row. Plain ASCII (▶ would
// also work but the play-symbol arrow looks busier next to other
// rows).
constexpr const char* kArmedGlyph = "▶ ";

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
    // MEMO: install ourselves as event filters on BOTH the tree and
    // its viewport. Keyboard events (Del) go to the tree; mouse
    // events (the new Ctrl+click gesture) go to the viewport. With
    // only one of them filtered we'd silently miss half of the
    // input — Qt's QAbstractItemView delegates click handling to
    // its internal viewport widget.
    tree_->installEventFilter(this);
    tree_->viewport()->installEventFilter(this);

    markersCategory_ = new QTreeWidgetItem(tree_);
    markersCategory_->setText(0, tr("Markers"));
    markersCategory_->setFlags(Qt::ItemIsEnabled);   // not selectable
    markersCategory_->setExpanded(true);

    loopsCategory_ = new QTreeWidgetItem(tree_);
    loopsCategory_->setText(0, tr("Loops"));
    loopsCategory_->setFlags(Qt::ItemIsEnabled);     // not selectable
    loopsCategory_->setExpanded(true);

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
            tr("Select a marker or loop above to view its properties."),
            page);
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
                this,            &ProjectViewerDock::onMarkerNameEdited);
        form->addRow(tr("Name:"), markerNameEdit_);

        markerPositionBox_ = new QSpinBox(page);
        markerPositionBox_->setObjectName("markerPositionBox");
        markerPositionBox_->setRange(0, INT_MAX);
        markerPositionBox_->setSuffix(tr(" ms"));
        // MEMO: bind to editingFinished (Enter or focus-loss) — NOT
        // valueChanged. valueChanged fires after every keystroke,
        // and that round-trips through the model
        // (onMarkerPositionEdited → setPosition → emit changed →
        // refreshPropertyPage → setValue), which forces the spinbox
        // to re-format its text on every edit. That re-formatting
        // strips leading zeros: editing "30004" by deleting the '3'
        // produces "0004" which gets re-parsed to 4 and re-rendered
        // as "4" — instead of the "0004" the user expects to keep
        // editing toward "40004". editingFinished commits only on
        // user intent (Enter / Tab / focus-loss), so the in-progress
        // text is left alone.
        connect(markerPositionBox_, &QSpinBox::editingFinished,
                this, &ProjectViewerDock::onMarkerPositionEdited);
        form->addRow(tr("Position:"), markerPositionBox_);

        propertyStack_->insertWidget(kPageMarker, page);
    }

    // Page 2: loop properties — name, start, end, and an Arm
    // checkbox that mirrors transport state.
    {
        auto* page = new QWidget(propertyStack_);
        auto* form = new QFormLayout(page);
        form->setContentsMargins(4, 4, 4, 4);

        loopNameEdit_ = new QLineEdit(page);
        loopNameEdit_->setObjectName("loopNameEdit");
        connect(loopNameEdit_, &QLineEdit::editingFinished,
                this,          &ProjectViewerDock::onLoopNameEdited);
        form->addRow(tr("Name:"), loopNameEdit_);

        loopStartBox_ = new QSpinBox(page);
        loopStartBox_->setObjectName("loopStartBox");
        loopStartBox_->setRange(0, INT_MAX);
        loopStartBox_->setSuffix(tr(" ms"));
        connect(loopStartBox_, &QSpinBox::editingFinished,
                this, &ProjectViewerDock::onLoopStartEdited);
        form->addRow(tr("Start:"), loopStartBox_);

        loopEndBox_ = new QSpinBox(page);
        loopEndBox_->setObjectName("loopEndBox");
        loopEndBox_->setRange(1, INT_MAX);    // end > start; 1 is the floor
        loopEndBox_->setSuffix(tr(" ms"));
        connect(loopEndBox_, &QSpinBox::editingFinished,
                this, &ProjectViewerDock::onLoopEndEdited);
        form->addRow(tr("End:"), loopEndBox_);

        // MEMO: per-loop "Pause" spinbox was removed in issue #16.
        // Pause-between-repeats is now a global setting in the
        // transport row, applied uniformly to every loop. The
        // per-loop value used to live here.

        loopArmedCheck_ = new QCheckBox(tr("Armed"), page);
        loopArmedCheck_->setObjectName("loopArmedCheck");
        // MEMO: connect to toggled(bool) (not stateChanged) because
        // we only ever use two states and toggled gives us the
        // boolean directly. The `updatingPropertyPage_` guard
        // suppresses re-emit when the dock pushes the armed state
        // back from MainWindow via setArmedLoopId().
        connect(loopArmedCheck_, &QCheckBox::toggled,
                this, &ProjectViewerDock::onLoopArmedToggled);
        form->addRow(QString(), loopArmedCheck_);

        propertyStack_->insertWidget(kPageLoop, page);
    }

    propertyStack_->setCurrentIndex(kPageNoSelection);
    layout->addWidget(propertyStack_);

    // ---- Global pre-roll countdown -------------------------------------
    // MEMO: the countdown widget used to live inside the loop
    // property page when pre-roll was a per-loop setting. After
    // issue #16 promoted it to a global setting, the widget belongs
    // to the dock as a whole — it's visible whenever the user has
    // pre-roll enabled, regardless of which (if any) artifact is
    // selected. Sits at the bottom so the property page above keeps
    // its natural sizing and the user always knows where to look
    // for the "ready, set, go" cue.
    //
    // setArmed(true) once at construction: with global pre-roll the
    // widget is only ever shown in the "ready / counting" tiers —
    // the "not armed" dim tier is unreachable now. See
    // memory/feedback_progressive_visual_weight.md.
    loopCountdown_ = new LoopCountdownWidget(root);
    loopCountdown_->setObjectName("loopCountdown");
    loopCountdown_->setArmed(true);
    loopCountdown_->setVisible(prerollEnabled_);
    layout->addWidget(loopCountdown_);

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
    // Drop any stale marker selection before rebuilding.
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    rebuildMarkerSection();
    refreshPropertyPage();
}

void ProjectViewerDock::setLoopModel(
    std::shared_ptr<score::LoopModel> model)
{
    if (loopModel_) {
        disconnect(loopModel_.get(), nullptr, this, nullptr);
    }
    loopModel_ = std::move(model);
    if (loopModel_) {
        connect(loopModel_.get(), &score::LoopModel::changed,
                this, &ProjectViewerDock::onLoopModelChanged);
    }
    if (selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (armedLoopId_.has_value()) {
        // The armed loop belongs to the model that's now gone; drop
        // it. MainWindow gets the cleared signal indirectly when the
        // user (re-)arms via the new model.
        armedLoopId_.reset();
    }
    rebuildLoopSection();
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
    // MEMO: cross-kind mutual exclusion in the dock — selecting a
    // marker clears any active loop selection. MainWindow's mirror
    // plumbing already enforces this for the score widgets; the
    // dock has to enforce it locally too because the tree is the
    // origin of selection events when the user clicks a row.
    if (id.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (selectedMarkerId_ == id) {
        // Even on a no-op marker selection we may still need to
        // refresh the property page if the loop slot was the
        // previous occupant (cleared above).
        refreshPropertyPage();
        return;
    }
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

void ProjectViewerDock::setSelectedLoopId(
    std::optional<std::int64_t> id)
{
    if (id.has_value() && loopModel_
        && !loopModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    // Mirror of setSelectedMarkerId: cross-kind mutual exclusion.
    if (id.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (selectedLoopId_ == id) {
        refreshPropertyPage();
        return;
    }
    selectedLoopId_ = id;

    const QSignalBlocker treeBlock(tree_);
    if (id.has_value()) {
        if (auto* item = findLoopItem(*id)) {
            tree_->setCurrentItem(item);
        } else {
            tree_->setCurrentItem(nullptr);
        }
    } else {
        tree_->setCurrentItem(nullptr);
    }

    refreshPropertyPage();
    emit loopSelectionChanged(selectedLoopId_);
}

void ProjectViewerDock::startCountdown(int totalMs) {
    if (loopCountdown_) loopCountdown_->start(totalMs);
}

void ProjectViewerDock::cancelCountdown() {
    if (loopCountdown_) loopCountdown_->cancel();
}

void ProjectViewerDock::setPrerollEnabled(bool enabled) {
    if (prerollEnabled_ == enabled) return;
    prerollEnabled_ = enabled;
    if (loopCountdown_) {
        // When pre-roll is turned off mid-countdown, also stop the
        // animation — leaving a frozen ring visible would be
        // confusing.
        if (!enabled) loopCountdown_->cancel();
        loopCountdown_->setVisible(enabled);
    }
}

void ProjectViewerDock::setArmedLoopId(
    std::optional<std::int64_t> id)
{
    // Coerce a dangling ID to nullopt — defensive against
    // callers that have a stale view of the model.
    if (id.has_value() && loopModel_
        && !loopModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    if (armedLoopId_ == id) return;
    armedLoopId_ = id;
    // MEMO: rebuild the loop section so the armed glyph (▶) moves
    // to the new row. The selection-by-ID is preserved by
    // rebuildLoopSection's signal-blocked re-application.
    rebuildLoopSection();
    // Sync the Arm checkbox if the property page is currently
    // showing the armed (or just-disarmed) loop. The
    // updatingPropertyPage_ guard suppresses the re-emit.
    refreshPropertyPage();
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
    rebuildMarkerSection();
    refreshPropertyPage();
}

void ProjectViewerDock::onLoopModelChanged() {
    // Mirror of onMarkerModelChanged. Range edits (setRange) keep
    // the same ID, so a selected loop survives a re-sort; only an
    // outright remove drops the selection.
    if (selectedLoopId_.has_value() && loopModel_
        && !loopModel_->indexOf(*selectedLoopId_).has_value())
    {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (armedLoopId_.has_value() && loopModel_
        && !loopModel_->indexOf(*armedLoopId_).has_value())
    {
        // Armed loop was removed — quietly clear; MainWindow detects
        // the dropped armedLoopId via its own listener on the model.
        armedLoopId_.reset();
    }
    rebuildLoopSection();
    refreshPropertyPage();
}

void ProjectViewerDock::rebuildMarkerSection() {
    // MEMO: block tree signals during the rebuild. Without this,
    // deleting the currently-selected QTreeWidgetItem fires
    // currentItemChanged(nullptr) which would wipe selection IDs —
    // turning a setPosition / setRange (which should preserve
    // selection by ID) into a selection-clearing event.
    const QSignalBlocker rebuildBlock(tree_);

    while (markersCategory_->childCount() > 0) {
        delete markersCategory_->takeChild(0);
    }
    if (markerModel_) {
        for (const auto& m : markerModel_->markers()) {
            auto* item = new QTreeWidgetItem(markersCategory_);
            // MEMO: user-visible text combines the user-given name
            // with the timestamp so the user can scan the list and
            // locate entries by either. Format mirrors what a
            // transcription editor reader would expect.
            item->setText(0, QString("%1   (%2 ms)")
                                .arg(m.name)
                                .arg(m.sourceMs));
            item->setData(0, kMarkerIdRole,
                          QVariant::fromValue<std::int64_t>(m.id));
        }
        markersCategory_->setExpanded(true);
    }

    // Re-apply the current selection (whatever kind it is). The
    // outer rebuildBlock keeps this from re-firing the
    // currentItemChanged slot.
    if (selectedMarkerId_.has_value()) {
        if (auto* item = findMarkerItem(*selectedMarkerId_)) {
            tree_->setCurrentItem(item);
        }
    } else if (selectedLoopId_.has_value()) {
        if (auto* item = findLoopItem(*selectedLoopId_)) {
            tree_->setCurrentItem(item);
        }
    }
}

void ProjectViewerDock::rebuildLoopSection() {
    const QSignalBlocker rebuildBlock(tree_);

    while (loopsCategory_->childCount() > 0) {
        delete loopsCategory_->takeChild(0);
    }
    if (loopModel_) {
        for (const auto& l : loopModel_->loops()) {
            auto* item = new QTreeWidgetItem(loopsCategory_);
            // MEMO: armed loop gets a "▶ " prefix in the row text so
            // the user can see at a glance which loop is currently
            // wrapping the transport.
            const bool armed = (armedLoopId_ == l.id);
            const QString text =
                QString("%1%2   (%3–%4 ms)")
                    .arg(armed ? QString::fromUtf8(kArmedGlyph)
                               : QString())
                    .arg(l.name)
                    .arg(l.startMs)
                    .arg(l.endMs);
            item->setText(0, text);
            item->setData(0, kLoopIdRole,
                          QVariant::fromValue<std::int64_t>(l.id));
        }
        loopsCategory_->setExpanded(true);
    }

    // Same selection re-apply as the marker section.
    if (selectedLoopId_.has_value()) {
        if (auto* item = findLoopItem(*selectedLoopId_)) {
            tree_->setCurrentItem(item);
        }
    } else if (selectedMarkerId_.has_value()) {
        if (auto* item = findMarkerItem(*selectedMarkerId_)) {
            tree_->setCurrentItem(item);
        }
    }
}

void ProjectViewerDock::refreshPropertyPage() {
    if (selectedMarkerId_.has_value() && markerModel_) {
        const auto idx = markerModel_->indexOf(*selectedMarkerId_);
        if (idx) {
            const auto& m = markerModel_->markers()[*idx];
            updatingPropertyPage_ = true;
            markerNameEdit_->setText(m.name);
            markerPositionBox_->setValue(static_cast<int>(m.sourceMs));
            updatingPropertyPage_ = false;
            propertyStack_->setCurrentIndex(kPageMarker);
            return;
        }
    }
    if (selectedLoopId_.has_value() && loopModel_) {
        const auto idx = loopModel_->indexOf(*selectedLoopId_);
        if (idx) {
            const auto& l = loopModel_->loops()[*idx];
            updatingPropertyPage_ = true;
            loopNameEdit_->setText(l.name);
            loopStartBox_->setValue(static_cast<int>(l.startMs));
            // MEMO: keep the End spinbox's lower bound one above the
            // current Start so the invariant end > start can never
            // be entered through the UI. Same trick the other way
            // for the Start spinbox's upper bound below.
            loopEndBox_->setMinimum(static_cast<int>(l.startMs) + 1);
            loopEndBox_->setValue(static_cast<int>(l.endMs));
            loopStartBox_->setMaximum(static_cast<int>(l.endMs) - 1);
            const bool isLoopArmed = (armedLoopId_ == l.id);
            loopArmedCheck_->setChecked(isLoopArmed);
            updatingPropertyPage_ = false;
            propertyStack_->setCurrentIndex(kPageLoop);
            return;
        }
    }
    propertyStack_->setCurrentIndex(kPageNoSelection);
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

QTreeWidgetItem*
ProjectViewerDock::findLoopItem(std::int64_t id) const {
    for (int i = 0; i < loopsCategory_->childCount(); ++i) {
        auto* child = loopsCategory_->child(i);
        if (child->data(0, kLoopIdRole).toLongLong() == id) {
            return child;
        }
    }
    return nullptr;
}

// ---- ui → model ---------------------------------------------------------

void ProjectViewerDock::onTreeCurrentItemChanged(
    QTreeWidgetItem* current, QTreeWidgetItem* /*previous*/)
{
    // MEMO: the row's category determines which kind of selection
    // we're emitting. Reading the parent pointer is more robust
    // than role-presence checks because category headers and
    // non-data items both fail any role check.
    if (current && current->parent() == markersCategory_) {
        std::optional<std::int64_t> newId =
            current->data(0, kMarkerIdRole).toLongLong();
        // Cross-kind mutual exclusion: if a loop was selected,
        // clear it before promoting the marker.
        if (selectedLoopId_.has_value()) {
            selectedLoopId_.reset();
            emit loopSelectionChanged(selectedLoopId_);
        }
        if (selectedMarkerId_ != newId) {
            selectedMarkerId_ = newId;
            refreshPropertyPage();
            emit markerSelectionChanged(selectedMarkerId_);
        } else {
            refreshPropertyPage();
        }
        return;
    }
    if (current && current->parent() == loopsCategory_) {
        std::optional<std::int64_t> newId =
            current->data(0, kLoopIdRole).toLongLong();
        if (selectedMarkerId_.has_value()) {
            selectedMarkerId_.reset();
            emit markerSelectionChanged(selectedMarkerId_);
        }
        if (selectedLoopId_ != newId) {
            selectedLoopId_ = newId;
            refreshPropertyPage();
            emit loopSelectionChanged(selectedLoopId_);
        } else {
            refreshPropertyPage();
        }
        return;
    }

    // Current is null or a category header — clear both kinds.
    bool emittedAny = false;
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
        emittedAny = true;
    }
    if (selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
        emittedAny = true;
    }
    if (emittedAny) refreshPropertyPage();
}

void ProjectViewerDock::onTreeItemDoubleClicked(
    QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;
    if (item->parent() == markersCategory_) {
        const auto id = item->data(0, kMarkerIdRole).toLongLong();
        emit markerActivated(id);
        return;
    }
    if (item->parent() == loopsCategory_) {
        const auto id = item->data(0, kLoopIdRole).toLongLong();
        emit loopActivated(id);
        return;
    }
    // Category header double-click is a no-op (Qt's default
    // expand/collapse behaviour is what we want here).
}

void ProjectViewerDock::onMarkerNameEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedMarkerId_ || !markerModel_) return;
    markerModel_->rename(*selectedMarkerId_, markerNameEdit_->text());
    // Tree text refreshes via onMarkerModelChanged → rebuildMarkerSection.
}

void ProjectViewerDock::onMarkerPositionEdited() {
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

void ProjectViewerDock::onLoopNameEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedLoopId_ || !loopModel_) return;
    loopModel_->rename(*selectedLoopId_, loopNameEdit_->text());
}

void ProjectViewerDock::onLoopStartEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedLoopId_ || !loopModel_) return;
    const auto idx = loopModel_->indexOf(*selectedLoopId_);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    const std::int64_t newStart = loopStartBox_->value();
    // The spinbox bounds (clamped at refreshPropertyPage time)
    // already prevent newStart >= currentEnd; defensively re-check
    // anyway in case the bounds ever drift.
    if (newStart >= l.endMs) return;
    loopModel_->setRange(*selectedLoopId_, newStart, l.endMs);
}

void ProjectViewerDock::onLoopEndEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedLoopId_ || !loopModel_) return;
    const auto idx = loopModel_->indexOf(*selectedLoopId_);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    const std::int64_t newEnd = loopEndBox_->value();
    if (newEnd <= l.startMs) return;
    loopModel_->setRange(*selectedLoopId_, l.startMs, newEnd);
}

void ProjectViewerDock::onLoopArmedToggled(bool checked) {
    if (updatingPropertyPage_) return;
    if (!selectedLoopId_) return;
    // MainWindow holds the actual transport state. We just relay
    // the request — MainWindow will flip our armedLoopId_ via
    // setArmedLoopId() once the change has actually taken effect.
    emit loopArmToggleRequested(*selectedLoopId_, checked);
}

// ---- input forwarding ---------------------------------------------------

bool ProjectViewerDock::eventFilter(QObject* watched, QEvent* event) {
    const bool isTreeViewport =
        (tree_ && watched == tree_->viewport());
    if (watched == tree_ || isTreeViewport) {
        // MEMO: dispatch in two branches — a left-click might extend
        // a loop-creation anchor pair (Ctrl+click on a marker row),
        // and Del fires the per-kind delete signal. Other events
        // fall through to the tree's defaults (arrow nav, Tab,
        // double-click handled by the standard signal, etc.).
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                handleTreeMousePress(mouseEvent);
                // Don't consume — Qt still needs to update the
                // selection in response to the click.
            }
        }
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Delete) {
                if (selectedMarkerId_.has_value()) {
                    emit markerDeleteRequested(*selectedMarkerId_);
                    return true;
                }
                if (selectedLoopId_.has_value()) {
                    emit loopDeleteRequested(*selectedLoopId_);
                    return true;
                }
            }
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void ProjectViewerDock::handleTreeMousePress(QMouseEvent* me) {
    // MEMO: emit the loop-anchor request BEFORE Qt processes the
    // click. The handler in MainWindow reads the *current* primary
    // anchor's ms (still the previous selection at this point),
    // captures it as the secondary, and only then does the click
    // proceed and change selection to the new marker. That ordering
    // is what makes Ctrl+click in the dock match Ctrl+click on the
    // score widgets — both capture the prior primary's ms.
    auto* item = tree_->itemAt(me->pos());
    const bool ctrlHeld =
        (me->modifiers() & Qt::ControlModifier) != 0;

    if (ctrlHeld && item && item->parent() == markersCategory_) {
        emit loopAnchorAddRequested();
        return;
    }
    // Plain click anywhere else (including loop rows or empty
    // space) clears the secondary anchor — the user is no longer
    // building a loop.
    emit loopAnchorClearRequested();
}

} // namespace fiddler::ui
