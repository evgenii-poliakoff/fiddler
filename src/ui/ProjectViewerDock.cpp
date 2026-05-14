#include "ui/ProjectViewerDock.h"

#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"
#include "score/Pitch.h"
#include "ui/LoopCountdownWidget.h"
#include "util/Log.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFocusEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
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
constexpr int kPageNote        = 3;

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

    // MEMO[#step6.2]: no Notes category in the dock tree. Notes
    // are selected and navigated via the staff (piano-roll bars),
    // not via a list — they're anonymous (pitch + interval, no
    // name) so a tabular row carries no information the staff
    // doesn't already show. Markers and loops are NAMED, hence
    // the tree rows. The note property page still exists and is
    // driven by the staff's selectedNoteId.

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
            tr("Select a marker, loop, or note above "
               "to view its properties."),
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
        markerNameEdit_->installEventFilter(this);
        connect(markerNameEdit_, &QLineEdit::editingFinished,
                this,            &ProjectViewerDock::onMarkerNameEdited);
        form->addRow(tr("Name:"), markerNameEdit_);

        markerPositionBox_ = new QSpinBox(page);
        markerPositionBox_->setObjectName("markerPositionBox");
        markerPositionBox_->setRange(0, INT_MAX);
        markerPositionBox_->setSuffix(tr(" ms"));
        // MEMO: filter the spinbox AND its inner QLineEdit. QSpinBox
        // wraps a QLineEdit for text entry; the inner widget is the
        // focus widget while the user is typing and receives the
        // Ctrl+Z ShortcutOverride directly. Filtering only the
        // spinbox would miss those.
        markerPositionBox_->installEventFilter(this);
        if (auto* inner = markerPositionBox_->findChild<QLineEdit*>()) {
            inner->installEventFilter(this);
        }
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
        loopNameEdit_->installEventFilter(this);
        connect(loopNameEdit_, &QLineEdit::editingFinished,
                this,          &ProjectViewerDock::onLoopNameEdited);
        form->addRow(tr("Name:"), loopNameEdit_);

        loopStartBox_ = new QSpinBox(page);
        loopStartBox_->setObjectName("loopStartBox");
        loopStartBox_->setRange(0, INT_MAX);
        loopStartBox_->setSuffix(tr(" ms"));
        loopStartBox_->installEventFilter(this);
        if (auto* inner = loopStartBox_->findChild<QLineEdit*>()) {
            inner->installEventFilter(this);
        }
        connect(loopStartBox_, &QSpinBox::editingFinished,
                this, &ProjectViewerDock::onLoopStartEdited);
        form->addRow(tr("Start:"), loopStartBox_);

        loopEndBox_ = new QSpinBox(page);
        loopEndBox_->setObjectName("loopEndBox");
        loopEndBox_->setRange(1, INT_MAX);    // end > start; 1 is the floor
        loopEndBox_->setSuffix(tr(" ms"));
        loopEndBox_->installEventFilter(this);
        if (auto* inner = loopEndBox_->findChild<QLineEdit*>()) {
            inner->installEventFilter(this);
        }
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

    // Page 3: note properties — pitch (SPN + MIDI label), start,
    // end, duration. There is intentionally NO Name field: top-level
    // notation apps (MuseScore / Sibelius / MusicXML / ABC) don't
    // name individual notes; the NoteModel::name field is kept for
    // forward compatibility (annotations / fingerings) but isn't
    // exposed in the property page.
    {
        auto* page = new QWidget(propertyStack_);
        auto* form = new QFormLayout(page);
        form->setContentsMargins(4, 4, 4, 4);

        // MEMO: pitch is entered as Scientific Pitch Notation ("A4",
        // "F#5") because a fiddler thinks in note names, not MIDI
        // numbers. The MIDI number is shown as a read-only secondary
        // label so the user has the conversion at hand if they want
        // it. Same dual presentation Logic / MuseScore use.
        auto* pitchRow = new QWidget(page);
        auto* pitchRowLayout = new QHBoxLayout(pitchRow);
        pitchRowLayout->setContentsMargins(0, 0, 0, 0);
        notePitchEdit_ = new QLineEdit(pitchRow);
        notePitchEdit_->setObjectName("notePitchEdit");
        notePitchEdit_->setPlaceholderText(tr("e.g. A4"));
        notePitchEdit_->installEventFilter(this);
        connect(notePitchEdit_, &QLineEdit::editingFinished,
                this,           &ProjectViewerDock::onNotePitchEdited);
        notePitchMidiLabel_ = new QLabel(QString(), pitchRow);
        notePitchMidiLabel_->setObjectName("notePitchMidiLabel");
        notePitchMidiLabel_->setEnabled(false);
        pitchRowLayout->addWidget(notePitchEdit_, /*stretch=*/1);
        pitchRowLayout->addWidget(notePitchMidiLabel_);
        form->addRow(tr("Pitch:"), pitchRow);

        noteStartBox_ = new QSpinBox(page);
        noteStartBox_->setObjectName("noteStartBox");
        noteStartBox_->setRange(0, INT_MAX);
        noteStartBox_->setSuffix(tr(" ms"));
        noteStartBox_->installEventFilter(this);
        if (auto* inner = noteStartBox_->findChild<QLineEdit*>()) {
            inner->installEventFilter(this);
        }
        connect(noteStartBox_, &QSpinBox::editingFinished,
                this, &ProjectViewerDock::onNoteStartEdited);
        form->addRow(tr("Start:"), noteStartBox_);

        noteEndBox_ = new QSpinBox(page);
        noteEndBox_->setObjectName("noteEndBox");
        noteEndBox_->setRange(1, INT_MAX);
        noteEndBox_->setSuffix(tr(" ms"));
        noteEndBox_->installEventFilter(this);
        if (auto* inner = noteEndBox_->findChild<QLineEdit*>()) {
            inner->installEventFilter(this);
        }
        connect(noteEndBox_, &QSpinBox::editingFinished,
                this, &ProjectViewerDock::onNoteEndEdited);
        form->addRow(tr("End:"), noteEndBox_);

        noteDurationLabel_ = new QLabel(QString(), page);
        noteDurationLabel_->setObjectName("noteDurationLabel");
        noteDurationLabel_->setEnabled(false);
        form->addRow(tr("Duration:"), noteDurationLabel_);

        propertyStack_->insertWidget(kPageNote, page);
    }

    // Section caption above the property page. Bold so it reads as
    // a heading; hidden when nothing is selected (the no-selection
    // page has its own dimmed hint text).
    propertyCaption_ = new QLabel(root);
    propertyCaption_->setObjectName("projectViewerPropertyCaption");
    QFont captionFont = propertyCaption_->font();
    captionFont.setBold(true);
    propertyCaption_->setFont(captionFont);
    propertyCaption_->setContentsMargins(4, 6, 4, 2);
    propertyCaption_->setVisible(false);
    layout->addWidget(propertyCaption_);

    setPropertyPage(kPageNoSelection);
    layout->addWidget(propertyStack_);

    // Single multi-mode button for note placement / editing. Its
    // label cycles among three modes — see the noteBuffer_ MEMO in
    // the header. Initial label is "New Note ..." (Empty mode);
    // disabled until a NoteModel is attached.
    addNoteButton_ = new QPushButton(tr("New Note ..."), root);
    addNoteButton_->setObjectName("addNoteButton");
    addNoteButton_->setEnabled(false);
    connect(addNoteButton_, &QPushButton::clicked,
            this,           &ProjectViewerDock::onAddNoteClicked);
    // Press is logged separately from clicked so a phantom-click
    // (e.g. one that fires without a mouse-down) is visible in the
    // log as a missing "press" before its "clicked".
    connect(addNoteButton_, &QPushButton::pressed,
            this, [this]() {
                FLOG_DEBUG("ui.dock",
                           "add-note-press has-model={} "
                           "current-focus-widget={}",
                           noteModel_ != nullptr,
                           QApplication::focusWidget()
                             ? QApplication::focusWidget()
                                  ->objectName().toStdString()
                             : std::string("none"));
            });
    layout->addWidget(addNoteButton_);

    // ---- Reference-tone controls (issue #step6.3) ----------------------
    // Two combo boxes laid out side-by-side with their labels:
    //   "Hover tone:"    [Off | Continuous | On tap]
    //   "Waveform:"      [Sine | Triangle]
    // The "Hover tone" mode gates how mouse motion over the chromatic
    // grid plays a reference tone; clicks on the piano keyboard column
    // ALWAYS play (regardless of mode). Waveform is global.
    {
        auto* refToneForm = new QFormLayout();
        refToneForm->setContentsMargins(4, 4, 4, 4);

        hoverToneCombo_ = new QComboBox(root);
        hoverToneCombo_->setObjectName("hoverToneCombo");
        hoverToneCombo_->addItem(tr("Off"),         /*userData=*/0);
        hoverToneCombo_->addItem(tr("Continuous"),  /*userData=*/1);
        hoverToneCombo_->addItem(tr("On tap"),      /*userData=*/2);
        hoverToneCombo_->setCurrentIndex(static_cast<int>(hoverToneMode_));
        connect(hoverToneCombo_,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) {
                    const auto mode = static_cast<HoverToneMode>(idx);
                    if (mode == hoverToneMode_) return;
                    hoverToneMode_ = mode;
                    FLOG_DEBUG("ui.dock",
                               "hover-tone-mode-changed mode={}",
                               static_cast<int>(mode));
                    emit hoverToneModeChanged(static_cast<int>(mode));
                });
        refToneForm->addRow(tr("Hover tone:"), hoverToneCombo_);

        toneWaveformCombo_ = new QComboBox(root);
        toneWaveformCombo_->setObjectName("toneWaveformCombo");
        toneWaveformCombo_->addItem(tr("Sine"),     /*userData=*/0);
        toneWaveformCombo_->addItem(tr("Triangle"), /*userData=*/1);
        toneWaveformCombo_->setCurrentIndex(
            static_cast<int>(toneWaveform_));
        connect(toneWaveformCombo_,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) {
                    const auto wf = static_cast<audio::Waveform>(idx);
                    if (wf == toneWaveform_) return;
                    toneWaveform_ = wf;
                    FLOG_DEBUG("ui.dock",
                               "tone-waveform-changed waveform={}",
                               static_cast<int>(wf));
                    emit toneWaveformChanged(static_cast<int>(wf));
                });
        refToneForm->addRow(tr("Waveform:"), toneWaveformCombo_);

        layout->addLayout(refToneForm);
    }

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

void ProjectViewerDock::setNoteModel(
    std::shared_ptr<score::NoteModel> model)
{
    FLOG_DEBUG("ui.dock",
               "set-note-model attach={} prev-attached={} "
               "prev-selected-id={}",
               model != nullptr, noteModel_ != nullptr,
               selectedNoteId_.value_or(-1));
    if (noteModel_) {
        disconnect(noteModel_.get(), nullptr, this, nullptr);
    }
    noteModel_ = std::move(model);
    if (noteModel_) {
        connect(noteModel_.get(), &score::NoteModel::changed,
                this, &ProjectViewerDock::onNoteModelChanged);
    }
    if (selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    // Detaching the model drops any pending draft / editing buffer
    // since it has nothing to commit against.
    noteBuffer_.reset();
    if (addNoteButton_) {
        addNoteButton_->setEnabled(noteModel_ != nullptr);
    }
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
    // marker clears any active loop / note selection. MainWindow's
    // mirror plumbing already enforces this for the score widgets;
    // the dock has to enforce it locally too because the tree is
    // the origin of selection events when the user clicks a row.
    if (id.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (id.has_value() && selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
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
    if (id.has_value() && selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
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

void ProjectViewerDock::setSelectedNoteId(
    std::optional<std::int64_t> id)
{
    const std::int64_t requestedRaw = id.value_or(-1);
    if (id.has_value() && noteModel_
        && !noteModel_->indexOf(*id).has_value()) {
        FLOG_DEBUG("ui.dock",
                   "set-selected-note-id={} coerced=stale-id has-model={}",
                   requestedRaw, noteModel_ != nullptr);
        id = std::nullopt;
    }
    if (id.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (id.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (selectedNoteId_ == id) {
        FLOG_DEBUG("ui.dock",
                   "set-selected-note-id={} no-op-equal",
                   requestedRaw);
        refreshPropertyPage();
        return;
    }
    FLOG_DEBUG("ui.dock",
               "set-selected-note-id new={} prev={}",
               id.value_or(-1), selectedNoteId_.value_or(-1));
    selectedNoteId_ = id;
    // State-machine: a non-null selection enters Editing mode with
    // a buffer populated from the model. A null selection exits to
    // Empty (clears any active buffer — including a pending draft).
    if (id.has_value()) {
        populateBufferFromModel(*id);
    } else {
        noteBuffer_.reset();
    }

    // MEMO[#step6.2]: clear the tree selection when a note is
    // chosen. Notes have no tree row, but markers / loops do — and
    // their selection must visually clear so the dock doesn't show
    // a stale highlight from a different artifact kind.
    {
        const QSignalBlocker treeBlock(tree_);
        tree_->setCurrentItem(nullptr);
    }

    refreshPropertyPage();
    emit noteSelectionChanged(selectedNoteId_);
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

void ProjectViewerDock::setHoverToneMode(HoverToneMode mode) {
    if (mode == hoverToneMode_) return;
    hoverToneMode_ = mode;
    if (hoverToneCombo_) {
        // Block the combo's signal so this programmatic update
        // doesn't re-fire hoverToneModeChanged — MainWindow's
        // QSettings persistence would then double-write.
        const QSignalBlocker block(hoverToneCombo_);
        hoverToneCombo_->setCurrentIndex(static_cast<int>(mode));
    }
}

void ProjectViewerDock::setToneWaveform(audio::Waveform waveform) {
    if (waveform == toneWaveform_) return;
    toneWaveform_ = waveform;
    if (toneWaveformCombo_) {
        const QSignalBlocker block(toneWaveformCombo_);
        toneWaveformCombo_->setCurrentIndex(static_cast<int>(waveform));
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

void ProjectViewerDock::onNoteModelChanged() {
    FLOG_DEBUG("ui.dock",
               "note-model-changed size={} selected-id={} buffer-id={}",
               noteModel_ ? noteModel_->size() : 0,
               selectedNoteId_.value_or(-1),
               noteBuffer_.has_value() ? noteBuffer_->id : -1);
    // setInterval / setPitch keep the same ID; only remove drops
    // selection.
    if (selectedNoteId_.has_value() && noteModel_
        && !noteModel_->indexOf(*selectedNoteId_).has_value())
    {
        FLOG_DEBUG("ui.dock",
                   "note-model-changed cleared-selection id={} reason=removed",
                   *selectedNoteId_);
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    // If we're editing an existing note and it's been removed from
    // the model (e.g. via undo of an add), drop the buffer too. A
    // draft (buffer.id == 0) is not tied to the model and survives.
    if (noteBuffer_.has_value() && noteBuffer_->id > 0 && noteModel_
        && !noteModel_->indexOf(noteBuffer_->id).has_value())
    {
        FLOG_DEBUG("ui.dock",
                   "note-model-changed dropped-buffer id={} reason=removed",
                   noteBuffer_->id);
        noteBuffer_.reset();
    }
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
    // Notes never appear in the dock tree (step 6.2) — staff selection.
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
            setPropertyPage(kPageMarker);
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
            setPropertyPage(kPageLoop);
            return;
        }
    }
    // Note property page — driven by noteBuffer_, not the model.
    // The buffer is set in NewDraft / Editing modes; in Empty mode
    // the page goes back to "no selection".
    if (noteBuffer_.has_value()) {
        const auto& buf = *noteBuffer_;
        updatingPropertyPage_ = true;
        notePitchEdit_->setText(score::midiToSpn(buf.midi));
        notePitchMidiLabel_->setText(tr("(MIDI %1)").arg(buf.midi));
        noteEndBox_->setMinimum(static_cast<int>(buf.startMs) + 1);
        noteStartBox_->setValue(static_cast<int>(buf.startMs));
        noteEndBox_->setValue(static_cast<int>(buf.endMs));
        noteStartBox_->setMaximum(static_cast<int>(buf.endMs) - 1);
        noteDurationLabel_->setText(
            tr("%1 ms").arg(buf.endMs - buf.startMs));
        updatingPropertyPage_ = false;
        setPropertyPage(kPageNote);
        FLOG_DEBUG("ui.dock",
                   "refresh-property-page page=note buffer-id={} "
                   "midi={} spn='{}' start={} end={}",
                   buf.id, buf.midi,
                   score::midiToSpn(buf.midi).toStdString(),
                   buf.startMs, buf.endMs);
        updateAddNoteButtonLabel();
        return;
    }
    setPropertyPage(kPageNoSelection);
    FLOG_DEBUG("ui.dock", "refresh-property-page page=no-selection");
    updateAddNoteButtonLabel();
}

void ProjectViewerDock::setPropertyPage(int pageIndex) {
    propertyStack_->setCurrentIndex(pageIndex);
    // Caption tracks the current page. Hide for no-selection so the
    // dimmed hint on the page reads on its own.
    switch (pageIndex) {
    case kPageMarker:
        propertyCaption_->setText(tr("Marker properties:"));
        propertyCaption_->setVisible(true);
        break;
    case kPageLoop:
        propertyCaption_->setText(tr("Loop properties:"));
        propertyCaption_->setVisible(true);
        break;
    case kPageNote:
        propertyCaption_->setText(tr("Note properties:"));
        propertyCaption_->setVisible(true);
        break;
    default:
        propertyCaption_->clear();
        propertyCaption_->setVisible(false);
        break;
    }
}

void ProjectViewerDock::updateAddNoteButtonLabel() {
    if (!addNoteButton_) return;
    // Editing mode (existing note selected): hide the button —
    // edits to the property page commit live, the inspector pattern
    // every major DAW uses. The button cycles only between Empty
    // and NewDraft for the keyboard-only "type a note in" flow.
    const bool editingExisting =
        noteBuffer_.has_value() && noteBuffer_->id > 0;
    addNoteButton_->setVisible(!editingExisting);
    if (editingExisting) return;
    const QString label = noteBuffer_.has_value()
        ? tr("Add Note")
        : tr("New Note ...");
    if (addNoteButton_->text() != label) {
        FLOG_DEBUG("ui.dock",
                   "button-label change to='{}' buffer-id={}",
                   label.toStdString(),
                   noteBuffer_.has_value() ? noteBuffer_->id : -1);
        addNoteButton_->setText(label);
    }
}

void ProjectViewerDock::populateBufferFromModel(std::int64_t id) {
    if (!noteModel_) return;
    const auto idx = noteModel_->indexOf(id);
    if (!idx) return;
    const auto& n = noteModel_->notes()[*idx];
    NoteBuffer buf;
    buf.id      = n.id;
    buf.startMs = n.startMs;
    buf.endMs   = n.endMs;
    buf.midi    = n.midi;
    buf.name    = n.name;
    noteBuffer_ = buf;
    FLOG_DEBUG("ui.dock",
               "populate-buffer-from-model id={} start={} end={} midi={}",
               buf.id, buf.startMs, buf.endMs, buf.midi);
}

void ProjectViewerDock::enterNoteDraftMode(std::int64_t startMs,
                                           std::int64_t endMs,
                                           int          midi) {
    FLOG_DEBUG("ui.dock",
               "enter-draft-mode start={} end={} midi={}",
               startMs, endMs, midi);
    NoteBuffer buf;
    buf.id      = 0;                // 0 = draft, not yet in model
    buf.startMs = startMs;
    buf.endMs   = endMs;
    buf.midi    = midi;
    noteBuffer_ = buf;
    // Clear any existing selection — draft is exclusive with row
    // selection.
    if (selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    // Move focus to the pitch field so user can start typing pitch
    // immediately.
    refreshPropertyPage();
    if (notePitchEdit_) notePitchEdit_->setFocus();
}

void ProjectViewerDock::exitNoteMode() {
    if (!noteBuffer_.has_value()) return;
    FLOG_DEBUG("ui.dock",
               "exit-note-mode prev-buffer-id={}",
               noteBuffer_->id);
    noteBuffer_.reset();
    refreshPropertyPage();
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
        // Cross-kind mutual exclusion: clear any other-kind
        // selection before promoting the marker.
        if (selectedLoopId_.has_value()) {
            selectedLoopId_.reset();
            emit loopSelectionChanged(selectedLoopId_);
        }
        if (selectedNoteId_.has_value()) {
            selectedNoteId_.reset();
            emit noteSelectionChanged(selectedNoteId_);
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
        if (selectedNoteId_.has_value()) {
            selectedNoteId_.reset();
            emit noteSelectionChanged(selectedNoteId_);
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
    // Current is null or a category header — clear every kind.
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
    if (selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
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
    // Notes have no tree row (step 6.2) — double-click only reaches
    // markers / loops.
    // Category header double-click is a no-op (Qt's default
    // expand/collapse behaviour is what we want here).
}

void ProjectViewerDock::onMarkerNameEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedMarkerId_ || !markerModel_) return;
    // MEMO: emit instead of calling markerModel_->rename directly so
    // MainWindow can capture the pre-edit name for the undo history
    // before applying. Dock keeps its model pointers for *reads*
    // (populating the property page); writes route through signals.
    emit markerRenameRequested(
        *selectedMarkerId_, markerNameEdit_->text());
    // Tree text refreshes via onMarkerModelChanged → rebuildMarkerSection.
}

void ProjectViewerDock::onMarkerPositionEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedMarkerId_ || !markerModel_) return;
    // editingFinished doesn't pass the new value, so we read it
    // directly from the spinbox. By the time this fires the user
    // has finished editing (Enter / focus-loss), so the value is
    // settled.
    emit markerPositionEditRequested(
        *selectedMarkerId_,
        static_cast<std::int64_t>(markerPositionBox_->value()));
}

void ProjectViewerDock::onLoopNameEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedLoopId_ || !loopModel_) return;
    emit loopRenameRequested(*selectedLoopId_, loopNameEdit_->text());
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
    emit loopRangeEditRequested(*selectedLoopId_, newStart, l.endMs);
}

void ProjectViewerDock::onLoopEndEdited() {
    if (updatingPropertyPage_) return;
    if (!selectedLoopId_ || !loopModel_) return;
    const auto idx = loopModel_->indexOf(*selectedLoopId_);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    const std::int64_t newEnd = loopEndBox_->value();
    if (newEnd <= l.startMs) return;
    emit loopRangeEditRequested(*selectedLoopId_, l.startMs, newEnd);
}

void ProjectViewerDock::onLoopArmedToggled(bool checked) {
    if (updatingPropertyPage_) return;
    if (!selectedLoopId_) return;
    // MainWindow holds the actual transport state. We just relay
    // the request — MainWindow will flip our armedLoopId_ via
    // setArmedLoopId() once the change has actually taken effect.
    emit loopArmToggleRequested(*selectedLoopId_, checked);
}

// ---- Note property page edits (state-machine, step 6.1) ----------
//
// In NewDraft and Editing modes, field edits mutate noteBuffer_ only.
// The noteModel is touched at commit time (button click). In Empty
// mode the fields are inert (page is "no selection").

void ProjectViewerDock::onNotePitchEdited() {
    if (updatingPropertyPage_) {
        FLOG_TRACE("ui.dock",
                   "note-pitch-edited skipped reason=updating-property-page");
        return;
    }
    if (!noteBuffer_.has_value()) {
        FLOG_DEBUG("ui.dock",
                   "note-pitch-edited skipped reason=no-buffer");
        return;
    }
    const QString text = notePitchEdit_->text().trimmed();
    const int midi = score::spnToMidi(text);
    if (midi < 0 || !score::NoteModel::isAcceptedPitch(midi)) {
        FLOG_DEBUG("ui.dock",
                   "note-pitch-edited buffer-id={} text='{}' parsed={} "
                   "accepted=no buffer-midi={} action=revert",
                   noteBuffer_->id, text.toStdString(),
                   midi, noteBuffer_->midi);
        updatingPropertyPage_ = true;
        notePitchEdit_->setText(score::midiToSpn(noteBuffer_->midi));
        notePitchMidiLabel_->setText(tr("(MIDI %1)").arg(noteBuffer_->midi));
        updatingPropertyPage_ = false;
        return;
    }
    if (midi == noteBuffer_->midi) {
        FLOG_TRACE("ui.dock",
                   "note-pitch-edited buffer-id={} midi={} no-op-equal",
                   noteBuffer_->id, midi);
        return;
    }
    FLOG_DEBUG("ui.dock",
               "note-pitch-edited buffer-id={} from={} to={} accepted=yes",
               noteBuffer_->id, noteBuffer_->midi, midi);
    noteBuffer_->midi = midi;
    updatingPropertyPage_ = true;
    notePitchMidiLabel_->setText(tr("(MIDI %1)").arg(midi));
    updatingPropertyPage_ = false;
    // Editing mode (buffer.id > 0): commit live so the model writes
    // through immediately — same gesture markers and loops use. The
    // model emits changed → refreshPropertyPage re-syncs the buffer.
    // For drafts (id == 0), the field-mutation-only flow stays as
    // before: nothing is in the model yet.
    if (noteBuffer_->id > 0) {
        emit noteCommitChangesRequested(noteBuffer_->id,
                                        noteBuffer_->startMs,
                                        noteBuffer_->endMs,
                                        noteBuffer_->midi);
    }
}

void ProjectViewerDock::onNoteStartEdited() {
    if (updatingPropertyPage_) return;
    if (!noteBuffer_.has_value()) {
        FLOG_DEBUG("ui.dock",
                   "note-start-edited skipped reason=no-buffer");
        return;
    }
    const std::int64_t newStart = noteStartBox_->value();
    if (newStart >= noteBuffer_->endMs) {
        FLOG_DEBUG("ui.dock",
                   "note-start-edited buffer-id={} new-start={} "
                   "buffer-end={} action=reject reason=start>=end",
                   noteBuffer_->id, newStart, noteBuffer_->endMs);
        return;
    }
    if (newStart == noteBuffer_->startMs) return;
    FLOG_DEBUG("ui.dock",
               "note-start-edited buffer-id={} from={} to={}",
               noteBuffer_->id, noteBuffer_->startMs, newStart);
    noteBuffer_->startMs = newStart;
    updatingPropertyPage_ = true;
    noteEndBox_->setMinimum(static_cast<int>(newStart) + 1);
    noteDurationLabel_->setText(
        tr("%1 ms").arg(noteBuffer_->endMs - newStart));
    updatingPropertyPage_ = false;
    if (noteBuffer_->id > 0) {
        emit noteCommitChangesRequested(noteBuffer_->id,
                                        noteBuffer_->startMs,
                                        noteBuffer_->endMs,
                                        noteBuffer_->midi);
    }
}

void ProjectViewerDock::onNoteEndEdited() {
    if (updatingPropertyPage_) return;
    if (!noteBuffer_.has_value()) {
        FLOG_DEBUG("ui.dock",
                   "note-end-edited skipped reason=no-buffer");
        return;
    }
    const std::int64_t newEnd = noteEndBox_->value();
    if (newEnd <= noteBuffer_->startMs) {
        FLOG_DEBUG("ui.dock",
                   "note-end-edited buffer-id={} new-end={} "
                   "buffer-start={} action=reject reason=end<=start",
                   noteBuffer_->id, newEnd, noteBuffer_->startMs);
        return;
    }
    if (newEnd == noteBuffer_->endMs) return;
    FLOG_DEBUG("ui.dock",
               "note-end-edited buffer-id={} from={} to={}",
               noteBuffer_->id, noteBuffer_->endMs, newEnd);
    noteBuffer_->endMs = newEnd;
    updatingPropertyPage_ = true;
    noteStartBox_->setMaximum(static_cast<int>(newEnd) - 1);
    noteDurationLabel_->setText(
        tr("%1 ms").arg(newEnd - noteBuffer_->startMs));
    updatingPropertyPage_ = false;
    if (noteBuffer_->id > 0) {
        emit noteCommitChangesRequested(noteBuffer_->id,
                                        noteBuffer_->startMs,
                                        noteBuffer_->endMs,
                                        noteBuffer_->midi);
    }
}

void ProjectViewerDock::onAddNoteClicked() {
    const QString focusName = QApplication::focusWidget()
        ? QApplication::focusWidget()->objectName()
        : QString();
    if (!noteBuffer_.has_value()) {
        // Empty mode: user is asking to begin a fresh draft. We
        // don't know the playback ms — MainWindow does. Emit the
        // signal and let it call enterNoteDraftMode() back on us.
        FLOG_DEBUG("ui.dock",
                   "button-click mode=empty action=request-draft "
                   "focus-at-click={}",
                   focusName.toStdString());
        emit noteAddRequested();
        return;
    }
    const auto buf = *noteBuffer_;
    // NewDraft is the only mode that reaches this branch — Editing
    // hides the button entirely (live-commit via per-field edits).
    FLOG_DEBUG("ui.dock",
               "button-click mode=new-draft action=commit "
               "start={} end={} midi={}",
               buf.startMs, buf.endMs, buf.midi);
    emit noteCommitNewRequested(buf.startMs, buf.endMs, buf.midi);
    exitNoteMode();
}

// ---- input forwarding ---------------------------------------------------

bool ProjectViewerDock::eventFilter(QObject* watched, QEvent* event) {
    // MEMO: reject Qt::ShortcutOverride for Ctrl+Z on every input
    // we install on (the property-page spinboxes / line edits).
    // QSpinBox / QLineEdit otherwise claim the key for their own
    // text-undo, which hides MainWindow's document-level undo
    // whenever the user has just typed into a dock field. With
    // this rejection the keypress falls through to the
    // application's QShortcut machinery.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->matches(QKeySequence::Undo)) {
            event->ignore();
            return true;
        }
    }

    // Focus tracking on note property page widgets. Helps trace
    // which widget had focus when something unexpected (e.g. an
    // Add Note click) fires.
    if (event->type() == QEvent::FocusIn
        || event->type() == QEvent::FocusOut) {
        const char* widgetTag = nullptr;
        if      (watched == notePitchEdit_) widgetTag = "note-pitch";
        else if (watched == noteStartBox_)  widgetTag = "note-start";
        else if (watched == noteEndBox_)    widgetTag = "note-end";
        if (widgetTag) {
            const char* dir = (event->type() == QEvent::FocusIn)
                ? "focus-in" : "focus-out";
            // Log the key-press / mouse-press reason if Qt provided
            // one (Qt::TabFocusReason, MouseFocusReason, etc).
            auto* fe = static_cast<QFocusEvent*>(event);
            FLOG_DEBUG("ui.dock",
                       "{} widget={} reason={} selected-note-id={}",
                       dir, widgetTag,
                       static_cast<int>(fe->reason()),
                       selectedNoteId_.value_or(-1));
        }
    }

    // Key tracking on the pitch line edit specifically — log every
    // Enter/Return so we can see the keystroke that triggers
    // editingFinished and any potential propagation.
    if (watched == notePitchEdit_
        && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return
            || keyEvent->key() == Qt::Key_Enter) {
            FLOG_DEBUG("ui.dock",
                       "note-pitch-key key=enter text='{}' "
                       "selected-id={}",
                       notePitchEdit_->text().toStdString(),
                       selectedNoteId_.value_or(-1));
        }
    }

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
                if (selectedNoteId_.has_value()) {
                    FLOG_DEBUG("ui.dock",
                               "del-key on=note id={} "
                               "emit=note-delete-requested",
                               *selectedNoteId_);
                    emit noteDeleteRequested(*selectedNoteId_);
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
