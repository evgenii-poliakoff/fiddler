#include "ui/MainWindow.h"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "audio/Decoder.h"
#include "audio/Player.h"
#include "audio/WaveformOverview.h"
#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"
#include "score/Pitch.h"
#include "score/Serialize.h"
#include "ui/ProjectViewerDock.h"
#include "ui/ScoreOverlayBase.h"
#include "ui/StaffWidget.h"
#include "ui/WaveformWidget.h"
#include "util/Log.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

namespace fiddler::ui {

namespace {

// Bucket count is the overview's *internal* peak resolution. The
// widget downsamples to its current pixel width on every paint, so
// a generous fixed count covers any plausible window width plus
// any practical zoom level without rebuilding.
//
// 4096 (pre-#49) was sized for the fit-to-window pixel density and
// turned visibly chunky at deeper zoom — each bucket spanning many
// pixels of the painted column. 32768 keeps ≤ ~3 px / bucket even
// at 1 % zoom on a wide widget. Memory: 32768 × 2 ch × 8 B = 512 KB,
// still trivial.
//
// At extreme zoom (one-sample-per-pixel territory) we'd need a
// proper multi-resolution / sample-level fallback — Audacity,
// Logic, Ableton, Reaper all do this with on-disk peak pyramids.
// Filed as follow-up; not needed for typical fiddle-practice zoom.
constexpr std::size_t kOverviewBuckets = 32768;

// Minimum source-time gap between same-kind tap-placements (#17).
// A rapid double-tap of B / M produces two artifacts at nearly the
// same ms — almost always an accident the user will then have to
// undo. We reject the second tap if any existing same-kind artifact
// sits within this window. 50 ms is below typical accidental retap
// rates (~100 ms) and above the fastest intentional placement
// (e.g. a triplet at 240 bpm = ~83 ms per note), so the rule
// catches accidents without limiting genuine fast tapping.
//
// MEMO: this lives at the gesture layer, not in the models. The
// models stay permissive — placing close artifacts via the dock's
// spinbox or programmatically is still allowed.
//
// MEMO[zoom]: a fixed source-time threshold won't be the right
// shape once zoom lands. At deep zoom-in 50 ms might be many
// hundreds of pixels apart and the user genuinely wants to place
// distinct artifacts that close; at zoom-out 50 ms is sub-pixel
// and the user can't visually distinguish them anyway. The
// future criterion is likely a *pixel* proximity threshold (e.g.
// reject within ~5 px), computed via xToMs/msToX at the current
// zoom. Re-evaluate this constant when adding zoom (issue TBD).
constexpr std::int64_t kMinTapSeparationMs = 50;

// Tradition-named time-signature presets, surfaced as the primary
// picker for the user. The order is roughly "most common in Irish
// trad first". When the user picks one, the entire TimeSignature
// (numerator, denominator, tuneType label) is pushed into the model.
//
// Air and free-meter / crooked tunes are reachable in the model but
// don't have a dedicated UI here yet — that's a deferred follow-on
// (see project_handbook_for_self_taught.md and project_crooked_tunes.md).
struct TuneTypePreset {
    const char* label;        // user-visible: "Reel (4/4)"
    const char* tuneType;     // metadata: "Reel"
    int         numerator;
    int         denominator;
};

constexpr std::array<TuneTypePreset, 10> kTuneTypePresets = {{
    { "Reel (4/4)",       "Reel",        4, 4 },
    { "Hornpipe (4/4)",   "Hornpipe",    4, 4 },
    { "Polka (2/4)",      "Polka",       2, 4 },
    { "March (2/4)",      "March",       2, 4 },
    { "Single Jig (6/8)", "Single Jig",  6, 8 },
    { "Double Jig (6/8)", "Double Jig",  6, 8 },
    { "Slip Jig (9/8)",   "Slip Jig",    9, 8 },
    { "Slide (12/8)",     "Slide",      12, 8 },
    { "Waltz (3/4)",      "Waltz",       3, 4 },
    { "Mazurka (3/4)",    "Mazurka",     3, 4 },
}};

// MEMO[#26]: QMessageBox::warning blocks on exec() in offscreen
// Qt — the same platform the test suite uses. Routing through
// this helper makes the error-path code paths testable: tests
// observe the bool return from save/openProject instead of
// driving a modal that has no one to click. Lives in the
// top-level anonymous namespace so the helper is visible to
// open* / save* call sites throughout the TU (#43 needs it
// from openByPath, which sits above the project-save section).
bool isOffscreenPlatform() {
    return qEnvironmentVariable("QT_QPA_PLATFORM")
        .compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0;
}

void showWarning(QWidget* parent, const QString& title,
                 const QString& text)
{
    if (isOffscreenPlatform()) return;
    QMessageBox::warning(parent, title, text);
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , player_(std::make_unique<audio::Player>())
    , barlineModel_(std::make_shared<score::BarlineModel>())
    , markerModel_(std::make_shared<score::MarkerModel>())
    , loopModel_(std::make_shared<score::LoopModel>())
    , noteModel_(std::make_shared<score::NoteModel>()) {
    // Listen for loop removals so we can drop armedLoopId_ if the
    // armed loop disappears (Del or Ctrl+Z). The model's signal is
    // the only way to find out; widgets and the dock route their
    // delete-requested signals through us, but a third party (e.g.
    // a future scripted edit) could remove a loop too.
    connect(loopModel_.get(), &score::LoopModel::changed,
            this, &MainWindow::onLoopModelChanged);
    // Initialise the model with the first preset (Reel 4/4) so the
    // staff's tune-type label matches what the combo box shows on
    // first launch. The combo defaults to index 0; without this
    // sync, the model would carry an empty tuneType string and the
    // staff would render the digits but not the "Reel" label above
    // them — confusing for a user who hasn't yet touched the combo.
    {
        const auto& initial = kTuneTypePresets[0];
        barlineModel_->setTimeSignature({
            initial.numerator,
            initial.denominator,
            QString::fromUtf8(initial.tuneType),
        });
    }

    setWindowTitle("Fiddler");
    buildMenus();
    buildCentralWidget();

    // MEMO[#26]: dirty tracking is driven by explicit calls to
    // recomputeDirty() at each undo-history push site and after
    // each onUndo pop (not by connecting to model `changed()`
    // signals, which would fire BEFORE the push and read a stale
    // history size). Non-undoable mutations like tune-type picks
    // set `nonUndoableDirty_` and recompute.

    // Wrap-pause QTimer — one persistent instance, started fresh
    // each time we enter the pause-between-repeats window. Holding
    // it as a member (not a static QTimer::singleShot) lets the user
    // cancel it explicitly via cancelPendingWrap when they press
    // Play or seek during the pause. See issue #13.
    wrapTimer_ = new QTimer(this);
    wrapTimer_->setSingleShot(true);
    connect(wrapTimer_, &QTimer::timeout, this, [this]() {
        wrapPending_ = false;
        // If we entered the pause for a SPECIFIC armed loop,
        // defensively check it's still the armed one. The user may
        // have disarmed or armed a different loop during the pause;
        // either way the auto-resume becomes a no-op rather than
        // playing the wrong loop. nullopt means "this was a generic
        // Play-pre-roll, no loop in particular" — just resume.
        if (wrapTargetLoopId_.has_value()
            && (!armedLoopId_.has_value()
                || *armedLoopId_ != *wrapTargetLoopId_))
        {
            return;
        }
        if (!player_) return;
        player_->play();
        playButton_->setText(tr("Pause"));
    });
    // MEMO: restoreLayout AFTER buildCentralWidget so the dock
    // exists before we ask Qt to push its persisted state into it.
    // Qt's restoreState matches dock widgets by objectName — set
    // in ProjectViewerDock's ctor.
    restoreLayout();
    statusBar()->showMessage("Ready");

    // Tap-to-place: 'B' anywhere in the window adds a barline at the
    // player's current source-time position. WindowShortcut context
    // means it fires regardless of which child widget has focus, so
    // the user can keep clicking around the waveform / staff and
    // still tap-along.
    tapBarShortcut_ = new QShortcut(QKeySequence(Qt::Key_B), this);
    tapBarShortcut_->setContext(Qt::WindowShortcut);
    connect(tapBarShortcut_, &QShortcut::activated,
            this, &MainWindow::onTapBarline);

    // 'M' anywhere in the window adds a marker at the player's
    // current source-time position. Same shape as 'B' for barlines.
    tapMarkerShortcut_ = new QShortcut(QKeySequence(Qt::Key_M), this);
    tapMarkerShortcut_->setContext(Qt::WindowShortcut);
    connect(tapMarkerShortcut_, &QShortcut::activated,
            this, &MainWindow::onTapMarker);

    // 'L' anywhere in the window creates a loop spanning the user's
    // primary selection's ms and the secondary anchor's ms. Both
    // come from the score widgets via Ctrl+click multi-select.
    createLoopShortcut_ = new QShortcut(QKeySequence(Qt::Key_L), this);
    createLoopShortcut_->setContext(Qt::WindowShortcut);
    connect(createLoopShortcut_, &QShortcut::activated,
            this, &MainWindow::onCreateLoop);

    // Ctrl+Z: reverse the most recent user-driven mutation,
    // regardless of kind. Dispatched via the unified undoHistory_.
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    undoShortcut_->setContext(Qt::WindowShortcut);
    connect(undoShortcut_, &QShortcut::activated,
            this, &MainWindow::onUndo);

    // Del: remove the currently-selected artifact (barline or
    // marker — selection is mutually exclusive between the two).
    // Window-scope so the shortcut works regardless of which child
    // has focus. The widgets' own Key_Delete handlers + the dock's
    // tree event filter stay in place for standalone unit tests; in
    // MainWindow context this shortcut intercepts the key first.
    deleteBarShortcut_ = new QShortcut(QKeySequence(Qt::Key_Delete), this);
    deleteBarShortcut_->setContext(Qt::WindowShortcut);
    connect(deleteBarShortcut_, &QShortcut::activated,
            this, &MainWindow::onDeleteSelectedArtifact);
}

MainWindow::~MainWindow() = default;

const score::BarlineModel& MainWindow::barlineModel() const noexcept {
    return *barlineModel_;
}

const score::MarkerModel& MainWindow::markerModel() const noexcept {
    return *markerModel_;
}

const score::LoopModel& MainWindow::loopModel() const noexcept {
    return *loopModel_;
}

const score::NoteModel& MainWindow::noteModel() const noexcept {
    return *noteModel_;
}

void MainWindow::buildMenus() {
    openAction_ = new QAction(tr("&Open…"), this);
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &MainWindow::onOpenFile);

    saveAction_ = new QAction(tr("&Save"), this);
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &MainWindow::onSave);

    saveAsAction_ = new QAction(tr("Save &As…"), this);
    saveAsAction_->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction_, &QAction::triggered, this, &MainWindow::onSaveAs);

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(openAction_);

    // Open Recent (#43). objectName lets integration tests locate
    // the submenu without walking the menu by index.
    recentFilesMenu_ = fileMenu->addMenu(tr("Open &Recent"));
    recentFilesMenu_->menuAction()->setObjectName("openRecentMenu");
    connect(recentFilesMenu_, &QMenu::aboutToShow,
            this, &MainWindow::rebuildRecentFilesMenu);
    // Build once now so the submenu's enabled state is right
    // before the user even pops it open.
    rebuildRecentFilesMenu();

    fileMenu->addSeparator();
    fileMenu->addAction(saveAction_);
    fileMenu->addAction(saveAsAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit,
                        this, &QWidget::close);
}

void MainWindow::buildCentralWidget() {
    auto* central = new QWidget(this);
    auto* layout  = new QVBoxLayout(central);

    statusLabel_ = new QLabel(tr("No file loaded."), central);
    layout->addWidget(statusLabel_);

    // Transport row: Play / Stop buttons on the left, the tune-type
    // picker on the right. Object names give integration tests a
    // stable handle to find each widget without reaching into private
    // state.
    auto* transport = new QHBoxLayout();
    playButton_ = new QPushButton(tr("Play"), central);
    stopButton_ = new QPushButton(tr("Stop"), central);
    playButton_->setObjectName("playButton");
    stopButton_->setObjectName("stopButton");
    playButton_->setEnabled(false);
    stopButton_->setEnabled(false);
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::onPlayPause);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStop);
    transport->addWidget(playButton_);
    transport->addWidget(stopButton_);
    transport->addSpacing(20);

    auto* sigLabel = new QLabel(tr("Tune type:"), central);
    transport->addWidget(sigLabel);
    tuneTypeCombo_ = new QComboBox(central);
    tuneTypeCombo_->setObjectName("tuneTypeCombo");
    for (const auto& preset : kTuneTypePresets) {
        tuneTypeCombo_->addItem(QString::fromUtf8(preset.label));
    }
    // Use `activated` (not `currentIndexChanged`) so programmatic
    // changes from setCurrentIndex() don't loop back into the slot.
    // We only react to actual user picks.
    connect(tuneTypeCombo_, &QComboBox::activated,
            this, &MainWindow::onTuneTypePresetChosen);
    transport->addWidget(tuneTypeCombo_);
    transport->addSpacing(20);

    // Global pre-roll spinbox + enable checkbox (issue #16).
    // Disabled checkbox = passive listening mode (no pre-roll, no
    // wrap silence). Enabled = practice mode (countdown before
    // every Play press AND between loop repeats). The spinbox
    // stays visible-but-greyed when disabled so the user sees
    // their preferred value without losing it.
    //
    // MEMO: editingFinished, not valueChanged — see the marker
    // Position spinbox for the leading-zero rationale.
    auto* prerollLabel = new QLabel(tr("Pre-roll:"), central);
    // MEMO: object name so tools/screenshots can find the label
    // when framing the "Pre-roll:" group as one region (label +
    // checkbox + spinbox). No behavioural effect at runtime.
    prerollLabel->setObjectName("prerollLabel");
    transport->addWidget(prerollLabel);
    prerollEnabledBox_ = new QCheckBox(central);
    prerollEnabledBox_->setObjectName("prerollEnabledBox");
    prerollEnabledBox_->setToolTip(
        tr("Enable practice-mode pre-roll countdown before each Play"));
    prerollEnabledBox_->setChecked(prerollEnabled_);
    connect(prerollEnabledBox_, &QCheckBox::toggled,
            this, &MainWindow::onPrerollEnabledToggled);
    transport->addWidget(prerollEnabledBox_);
    prerollBox_ = new QSpinBox(central);
    prerollBox_->setObjectName("prerollBox");
    prerollBox_->setRange(0, 5000);
    prerollBox_->setSingleStep(100);
    prerollBox_->setSuffix(tr(" ms"));
    prerollBox_->setValue(prerollMs_);
    prerollBox_->setEnabled(prerollEnabled_);
    connect(prerollBox_, &QSpinBox::editingFinished,
            this, &MainWindow::onPrerollChanged);
    // MEMO: see eventFilter() — installed on BOTH the spinbox and
    // its inner QLineEdit. QSpinBox wraps a QLineEdit for text
    // entry; when the user is typing, the inner widget is the
    // focus widget and receives ShortcutOverride directly.
    // Filtering only the spinbox would miss those events.
    prerollBox_->installEventFilter(this);
    if (auto* innerEdit = prerollBox_->findChild<QLineEdit*>()) {
        innerEdit->installEventFilter(this);
    }
    transport->addWidget(prerollBox_);
    transport->addStretch();
    layout->addLayout(transport);

    // Waveform: the upper view. Stretches to fill the extra vertical
    // space; barline ticks line up 1:1 with the staff below.
    waveform_ = new WaveformWidget(central);
    waveform_->setObjectName("waveformWidget");
    waveform_->setBarlineModel(barlineModel_);
    waveform_->setMarkerModel(markerModel_);
    waveform_->setLoopModel(loopModel_);
    waveform_->setNoteModel(noteModel_);
    connect(waveform_, &WaveformWidget::seekRequested,
            this, [this](std::int64_t ms) {
                // MEMO: log includes the source ("waveform-click")
                // because onSeek() is a primitive shared by three
                // sources (waveform, staff, position-slider) and the
                // log is the only place that distinguishes them.
                FLOG_DEBUG("ui.score",
                           "seek ms={} via=waveform-click", ms);
                onSeek(static_cast<int>(ms));
            });
    connect(waveform_, &WaveformWidget::barlineSelectionChanged,
            this, &MainWindow::onWaveformBarlineSelectionChanged);
    connect(waveform_, &WaveformWidget::markerSelectionChanged,
            this, &MainWindow::onWaveformMarkerSelectionChanged);
    connect(waveform_, &WaveformWidget::loopSelectionChanged,
            this, &MainWindow::onWaveformLoopSelectionChanged);
    connect(waveform_, &WaveformWidget::secondaryAnchorChanged,
            this, &MainWindow::onWaveformSecondaryAnchorChanged);
    connect(waveform_, &WaveformWidget::barlineDeleteRequested,
            this, &MainWindow::onBarlineDeleteRequested);
    connect(waveform_, &WaveformWidget::markerDeleteRequested,
            this, &MainWindow::onMarkerDeleteRequested);
    // MEMO[#step6.1]: notes don't paint on the waveform yet, but the
    // waveform's plain-seek branch emits noteSelectionChanged on every
    // empty-space click so the staff and dock can drop their note
    // selection in lockstep. See onWaveformNoteSelectionChanged.
    connect(waveform_, &WaveformWidget::noteSelectionChanged,
            this, &MainWindow::onWaveformNoteSelectionChanged);
    // Empty-space click → discard any pending note draft in the
    // dock. The selection chain alone misses this because in
    // NewDraft mode no widget has selectedNoteId set, so a
    // setSelectedNoteId(nullopt) call hits the no-op-equal early
    // return. Hooking emptySpaceClicked directly avoids that.
    connect(waveform_, &WaveformWidget::emptySpaceClicked,
            this, [this]() {
                if (projectViewerDock_) projectViewerDock_->exitNoteMode();
            });
    // Viewport / zoom (#49). Either widget can be the source of
    // a zoom gesture; the slot syncs the partner and the scrollbar.
    connect(waveform_, &ScoreOverlayBase::viewportChanged,
            this, &MainWindow::onViewportChanged);
    connect(waveform_, &ScoreOverlayBase::userScrolled,
            this, [this]() { setFollowPlayback(false); });
    connect(waveform_, &WaveformWidget::markerDragRequested,
            this, &MainWindow::onMarkerDragRequested);
    connect(waveform_, &WaveformWidget::loopDragRequested,
            this, &MainWindow::onLoopDragRequested);
    connect(waveform_, &WaveformWidget::dragStarted,
            this, &MainWindow::onDragStarted);
    connect(waveform_, &WaveformWidget::dragEnded,
            this, &MainWindow::onDragEnded);
    connect(waveform_, &WaveformWidget::markerDragCommitted,
            this, [this](std::int64_t id,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                commitMarkerDrag(id, fromMs, toMs, "waveform");
            });
    connect(waveform_, &WaveformWidget::loopDragCommitted,
            this, [this](std::int64_t id, bool isStart,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                commitLoopDrag(id, isStart, fromMs, toMs, "waveform");
            });
    // Loop body / create drags (issue #62).
    connect(waveform_, &WaveformWidget::loopMoveCommitted,
            this, [this](std::int64_t id,
                         std::int64_t fromStart, std::int64_t fromEnd,
                         std::int64_t toStart,   std::int64_t toEnd) {
                commitLoopMoveDrag(id, fromStart, fromEnd,
                                   toStart, toEnd, "waveform");
            });
    connect(waveform_, &WaveformWidget::loopCreateCommitted,
            this, [this](std::int64_t startMs, std::int64_t endMs) {
                onLoopCreateCommitted(startMs, endMs, "waveform");
            });
    connect(waveform_, &WaveformWidget::loopCreateDragRequested,
            this, [this](std::optional<std::int64_t> startMs,
                         std::optional<std::int64_t> endMs) {
                // Sister-mirror: staff paints the phantom too while
                // the user draws on the waveform.
                if (staff_) staff_->setPhantomLoopGhost(startMs, endMs);
            });
    // MEMO[#step6.2]: indent the waveform by the staff's piano-keyboard
    // column width so its left edge lines up with the staff's chromatic
    // grid. The playhead cursor on both widgets then lands at the same
    // x for the same source-ms, drawing one unbroken vertical line
    // across the waveform AND the staff at the playback position.
    auto* waveformRow = new QHBoxLayout();
    waveformRow->setContentsMargins(0, 0, 0, 0);
    waveformRow->addSpacing(StaffWidget::kKeyboardWidthPx);
    waveformRow->addWidget(waveform_, /*stretch=*/1);
    layout->addLayout(waveformRow, /*stretch=*/1);

    // Staff: the lower view. Fixed-ish height (its sizeHint), shares
    // the same models and source-time axis as the waveform.
    staff_ = new StaffWidget(central);
    staff_->setObjectName("staffWidget");
    staff_->setBarlineModel(barlineModel_);
    staff_->setMarkerModel(markerModel_);
    staff_->setLoopModel(loopModel_);
    staff_->setNoteModel(noteModel_);
    connect(staff_, &StaffWidget::seekRequested,
            this, [this](std::int64_t ms) {
                FLOG_DEBUG("ui.score",
                           "seek ms={} via=staff-click", ms);
                onSeek(static_cast<int>(ms));
            });
    connect(staff_, &StaffWidget::barlineSelectionChanged,
            this, &MainWindow::onStaffBarlineSelectionChanged);
    connect(staff_, &StaffWidget::markerSelectionChanged,
            this, &MainWindow::onStaffMarkerSelectionChanged);
    connect(staff_, &StaffWidget::loopSelectionChanged,
            this, &MainWindow::onStaffLoopSelectionChanged);
    connect(staff_, &StaffWidget::secondaryAnchorChanged,
            this, &MainWindow::onStaffSecondaryAnchorChanged);
    connect(staff_, &StaffWidget::barlineDeleteRequested,
            this, &MainWindow::onBarlineDeleteRequested);
    connect(staff_, &StaffWidget::markerDeleteRequested,
            this, &MainWindow::onMarkerDeleteRequested);
    connect(staff_, &StaffWidget::noteSelectionChanged,
            this, &MainWindow::onStaffNoteSelectionChanged);
    connect(staff_, &StaffWidget::noteDeleteRequested,
            this, &MainWindow::onNoteDeleteRequested);
    connect(staff_, &StaffWidget::emptySpaceClicked,
            this, [this]() {
                if (projectViewerDock_) projectViewerDock_->exitNoteMode();
            });
    // Click on a chromatic row commits a note immediately at the
    // clicked (ms, midi). Step 6.2 piano-roll gesture.
    connect(staff_, &StaffWidget::placeNoteRequested,
            this, &MainWindow::onStaffPlaceNoteRequested);
    // Drag gestures (issue #60): move + resize on existing notes,
    // and drag-to-create on the empty grid.
    connect(staff_, &StaffWidget::noteDragCommitted,
            this, &MainWindow::onStaffNoteDragCommitted);
    connect(staff_, &StaffWidget::noteCreateCommitted,
            this, &MainWindow::onStaffNoteCreateCommitted);
    connect(staff_, &ScoreOverlayBase::viewportChanged,
            this, &MainWindow::onViewportChanged);
    connect(staff_, &ScoreOverlayBase::userScrolled,
            this, [this]() { setFollowPlayback(false); });
    connect(staff_, &StaffWidget::markerDragRequested,
            this, &MainWindow::onMarkerDragRequested);
    connect(staff_, &StaffWidget::loopDragRequested,
            this, &MainWindow::onLoopDragRequested);
    connect(staff_, &StaffWidget::dragStarted,
            this, &MainWindow::onDragStarted);
    connect(staff_, &StaffWidget::dragEnded,
            this, &MainWindow::onDragEnded);
    connect(staff_, &StaffWidget::markerDragCommitted,
            this, [this](std::int64_t id,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                commitMarkerDrag(id, fromMs, toMs, "staff");
            });
    connect(staff_, &StaffWidget::loopDragCommitted,
            this, [this](std::int64_t id, bool isStart,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                commitLoopDrag(id, isStart, fromMs, toMs, "staff");
            });
    // Loop body drag-move + drag-create are BOTH waveform-only
    // (issue #62, conflict resolution). The staff's armsLoopMove
    // returns false so it can't emit loopMoveCommitted; we leave
    // no connect for it. Loops still paint on the staff for
    // visual context — see paintLoops in StaffWidget.
    layout->addWidget(staff_);

    // Mirror the zoom-anchor guide across both widgets so the
    // dashed amber line is continuous from waveform into staff.
    // The OWNER widget (the one under the mouse with Ctrl held)
    // emits zoomAnchorGuideChanged; the sister widget receives
    // setMirroredZoomAnchorMs and paints the guide at the same
    // source-ms. The receiver does not re-emit — see the slot's
    // comment for the no-loop guarantee.
    connect(waveform_, &ScoreOverlayBase::zoomAnchorGuideChanged,
            staff_,    &ScoreOverlayBase::setMirroredZoomAnchorAxisX);
    connect(staff_,    &ScoreOverlayBase::zoomAnchorGuideChanged,
            waveform_, &ScoreOverlayBase::setMirroredZoomAnchorAxisX);

    // Viewport scrollbar (#49). Visible only when the user has
    // zoomed in (waveform_->isZoomed() == true); hidden otherwise
    // so the layout doesn't sprout a permanent inert band. Maps
    // [0, durationMs] in source time; the page-step matches the
    // current viewport span.
    viewportScrollBar_ = new QScrollBar(Qt::Horizontal, central);
    viewportScrollBar_->setObjectName("viewportScrollBar");
    viewportScrollBar_->setRange(0, 0);
    viewportScrollBar_->setVisible(false);
    connect(viewportScrollBar_, &QAbstractSlider::valueChanged,
            this, [this](int newStartMs) {
                if (suppressViewportScrollBarSignals_) return;
                const auto span = waveform_ ? waveform_->viewportSpanMs() : 0;
                if (span <= 0) return;
                FLOG_DEBUG("ui.zoom",
                           "scroll start={} ms span={} ms via=scrollbar",
                           newStartMs, span);
                applyViewport(newStartMs, newStartMs + span);
                // Manual scroll during playback breaks follow.
                setFollowPlayback(false);
            });
    // Indent to match the waveform / staff time axis (see waveformRow).
    auto* scrollBarRow = new QHBoxLayout();
    scrollBarRow->setContentsMargins(0, 0, 0, 0);
    scrollBarRow->addSpacing(StaffWidget::kKeyboardWidthPx);
    scrollBarRow->addWidget(viewportScrollBar_);
    layout->addLayout(scrollBarRow);

    positionSlider_ = new QSlider(Qt::Horizontal, central);
    positionSlider_->setObjectName("positionSlider");
    positionSlider_->setRange(0, 0);
    positionSlider_->setEnabled(false);
    connect(positionSlider_, &QSlider::sliderMoved,
            this, [this](int ms) {
                // MEMO: sliderMoved (not valueChanged) only fires
                // for user-driven motion, so this lambda doesn't log
                // for programmatic setValue calls during
                // updatePosition(). That's the right behaviour —
                // those programmatic updates are not user actions.
                FLOG_DEBUG("ui.transport",
                           "seek ms={} via=position-slider", ms);
                onSeek(ms);
            });
    // Indent to match the waveform / staff time axis (see waveformRow).
    auto* positionRow = new QHBoxLayout();
    positionRow->setContentsMargins(0, 0, 0, 0);
    positionRow->addSpacing(StaffWidget::kKeyboardWidthPx);
    positionRow->addWidget(positionSlider_);
    layout->addLayout(positionRow);

    auto* tempoRow = new QHBoxLayout();
    tempoLabel_ = new QLabel(tr("Tempo: 100%"), central);
    tempoLabel_->setObjectName("tempoLabel");
    tempoSlider_ = new QSlider(Qt::Horizontal, central);
    tempoSlider_->setObjectName("tempoSlider");
    tempoSlider_->setRange(25, 100);
    tempoSlider_->setValue(100);
    tempoSlider_->setEnabled(false);
    connect(tempoSlider_, &QSlider::valueChanged,
            this, &MainWindow::onTempoChanged);
    tempoRow->addWidget(tempoLabel_);
    tempoRow->addWidget(tempoSlider_, /*stretch=*/1);
    layout->addLayout(tempoRow);

    // No trailing addStretch — waveform_ already claims any extra
    // vertical space via its layout stretch factor.
    setCentralWidget(central);

    // Right-side artifact viewer / inspector. MEMO: convention from
    // editing-focused tools (DaVinci Resolve, Final Cut, Audition,
    // GarageBand) — Inspector / properties panels live on the right.
    // The dock displays markers and (future) loops, and edits them
    // via the property page.
    projectViewerDock_ = new ProjectViewerDock(this);
    projectViewerDock_->setMarkerModel(markerModel_);
    projectViewerDock_->setLoopModel(loopModel_);
    projectViewerDock_->setNoteModel(noteModel_);
    // Sync the dock's countdown-widget visibility with our current
    // prerollEnabled_ state. restoreLayout() will push the
    // persisted value through later if a previous session saved
    // one; this initial call keeps things consistent for the
    // first-ever launch.
    projectViewerDock_->setPrerollEnabled(prerollEnabled_);
    addDockWidget(Qt::RightDockWidgetArea, projectViewerDock_);
    connect(projectViewerDock_,
            &ProjectViewerDock::markerSelectionChanged,
            this, &MainWindow::onDockMarkerSelectionChanged);
    connect(projectViewerDock_,
            &ProjectViewerDock::markerActivated,
            this, &MainWindow::onMarkerActivated);
    connect(projectViewerDock_,
            &ProjectViewerDock::markerDeleteRequested,
            this, &MainWindow::onMarkerDeleteRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopSelectionChanged,
            this, &MainWindow::onDockLoopSelectionChanged);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopDeleteRequested,
            this, &MainWindow::onLoopDeleteRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopActivated,
            this, &MainWindow::onLoopActivated);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopArmToggleRequested,
            this, &MainWindow::onLoopArmToggleRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopAnchorAddRequested,
            this, &MainWindow::onDockLoopAnchorAddRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopAnchorClearRequested,
            this, &MainWindow::onDockLoopAnchorClearRequested);

    // Property-page edits — captured here (rather than in the dock)
    // so the pre-edit snapshot can land in the undo history before
    // the model takes the new value.
    connect(projectViewerDock_,
            &ProjectViewerDock::markerRenameRequested,
            this, &MainWindow::onDockMarkerRenameRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::markerPositionEditRequested,
            this, &MainWindow::onDockMarkerPositionEditRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopRenameRequested,
            this, &MainWindow::onDockLoopRenameRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::loopRangeEditRequested,
            this, &MainWindow::onDockLoopRangeEditRequested);

    // Note dock signals (step 6.1).
    connect(projectViewerDock_,
            &ProjectViewerDock::noteSelectionChanged,
            this, &MainWindow::onDockNoteSelectionChanged);
    connect(projectViewerDock_,
            &ProjectViewerDock::noteDeleteRequested,
            this, &MainWindow::onNoteDeleteRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::noteActivated,
            this, &MainWindow::onNoteActivated);
    connect(projectViewerDock_,
            &ProjectViewerDock::noteAddRequested,
            this, &MainWindow::onDockAddNoteRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::noteCommitNewRequested,
            this, &MainWindow::onDockNoteCommitNewRequested);
    connect(projectViewerDock_,
            &ProjectViewerDock::noteCommitChangesRequested,
            this, &MainWindow::onDockNoteCommitChangesRequested);
    // Legacy per-field signals are no longer wired — see the
    // state-machine refactor in #step6.1.

    // ---- View menu ------------------------------------------------------
    // MEMO: built here (not in buildMenus) because it needs the
    // dock's toggleViewAction, and the dock isn't constructed until
    // this method runs. F4 mirrors the Inspector-toggle shortcut
    // used in DaVinci Resolve, Final Cut, and Qt Creator. Without
    // this menu, closing the dock via its X button would leave the
    // user with no way to reopen it short of restarting the app —
    // see issue #7.
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    auto* toggleProjectViewer = projectViewerDock_->toggleViewAction();
    toggleProjectViewer->setText(tr("&Project Viewer"));
    toggleProjectViewer->setShortcut(QKeySequence(Qt::Key_F4));
    // objectName lets integration tests reach the action without
    // walking the menu by index.
    toggleProjectViewer->setObjectName("toggleProjectViewerAction");
    viewMenu->addAction(toggleProjectViewer);

    viewMenu->addSeparator();

    // Zoom (#49). Ctrl+= / Ctrl+- / Ctrl+0 are the conventional
    // bindings — Logic, Ableton, Audacity, Audition all agree.
    auto* zoomInAct = new QAction(tr("Zoom &In"), this);
    zoomInAct->setObjectName("zoomInAction");
    zoomInAct->setShortcuts({ QKeySequence(QKeySequence::ZoomIn),
                              QKeySequence(Qt::CTRL | Qt::Key_Equal) });
    connect(zoomInAct, &QAction::triggered, this, &MainWindow::onZoomIn);
    viewMenu->addAction(zoomInAct);

    auto* zoomOutAct = new QAction(tr("Zoom &Out"), this);
    zoomOutAct->setObjectName("zoomOutAction");
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAct, &QAction::triggered, this, &MainWindow::onZoomOut);
    viewMenu->addAction(zoomOutAct);

    auto* zoomFitAct = new QAction(tr("&Fit to Window"), this);
    zoomFitAct->setObjectName("zoomFitAction");
    zoomFitAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(zoomFitAct, &QAction::triggered, this, &MainWindow::onZoomFit);
    viewMenu->addAction(zoomFitAct);

    viewMenu->addSeparator();

    // Follow Playback (#49). Default ON. The view auto-pages
    // forward when the cursor reaches the right edge during
    // playback; gets toggled OFF by any manual scroll.
    followPlaybackAction_ = new QAction(tr("&Follow Playback"), this);
    followPlaybackAction_->setObjectName("followPlaybackAction");
    followPlaybackAction_->setCheckable(true);
    followPlaybackAction_->setChecked(followPlayback_);
    connect(followPlaybackAction_, &QAction::toggled,
            this, [this](bool on) {
                followPlayback_ = on;
                FLOG_DEBUG("ui.zoom",
                           "follow-playback set={} via=menu", on);
            });
    viewMenu->addAction(followPlaybackAction_);
}

void MainWindow::onOpenFile() {
    // MEMO[#26]: same dialog handles audio and project files.
    // Dispatch on the suffix — `.fdlp` loads a saved project,
    // anything else loads as audio. The combined filter spares
    // the user from a separate "Open Project" menu entry.
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open audio or project file"),
        {},
        tr("Audio files (*.wav *.flac *.mp3 *.ogg *.m4a *.aac *.opus);;"
           "Fiddler project (*.fdlp);;"
           "All files (*)"));
    if (path.isEmpty()) {
        // MEMO: ui-event log — user dismissed the file dialog.
        // See feedback_logs_drive_tests.md for the contract.
        FLOG_DEBUG("ui.file", "open: cancelled");
        return;
    }

    openByPath(path);
}

bool MainWindow::openByPath(const QString& path) {
    // Single dispatch helper shared by onOpenFile and the Open
    // Recent submenu (#43). Routes on suffix, surfaces a warning
    // for missing audio / corrupted projects, prunes the MRU on
    // missing-file. openProject / loadFile handle their own
    // "open failed" warnings; we layer the MRU bookkeeping on top.
    const QString absolute = QFileInfo(path).absoluteFilePath();

    if (!QFileInfo::exists(absolute)) {
        showWarning(this, tr("File not found"),
            tr("This file is no longer available:\n%1").arg(absolute));
        FLOG_WARN("ui.file", "open-recent: missing path={}",
                  absolute.toStdString());
        // Drop the dead entry from the MRU so the user isn't
        // offered it again next time the menu opens.
        QSettings settings;
        QStringList recent = settings.value(
            QStringLiteral("mainwindow/recentFiles")).toStringList();
        if (recent.removeAll(absolute) > 0) {
            settings.setValue(
                QStringLiteral("mainwindow/recentFiles"), recent);
        }
        return false;
    }

    bool ok = false;
    if (absolute.endsWith(QStringLiteral(".fdlp"), Qt::CaseInsensitive)) {
        ok = openProject(absolute);
    } else {
        ok = loadFile(absolute);
        if (!ok) {
            showWarning(this, tr("Open failed"),
                tr("Could not open: %1").arg(absolute));
        }
    }

    if (ok) {
        pushRecentFile(absolute);
    }
    return ok;
}

void MainWindow::pushRecentFile(const QString& path) {
    QSettings settings;
    QStringList recent = settings.value(
        QStringLiteral("mainwindow/recentFiles")).toStringList();
    // Dedupe by exact path; the absolute form was canonicalised by
    // the caller (openByPath uses QFileInfo::absoluteFilePath).
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentFiles) {
        recent.removeLast();
    }
    settings.setValue(QStringLiteral("mainwindow/recentFiles"), recent);
    FLOG_DEBUG("ui.file", "recent: push path={} size={}",
               path.toStdString(), recent.size());

    // MEMO[#43 smoke]: when the MRU starts empty, the submenu is
    // disabled — and a disabled QMenu never fires aboutToShow, so
    // pushes during this session would have stayed invisible until
    // the next launch. Re-running the build here flips the menu to
    // enabled and populates the entries immediately.
    if (recentFilesMenu_) {
        rebuildRecentFilesMenu();
    }
}

QStringList MainWindow::recentFiles() const {
    QSettings settings;
    return settings.value(
        QStringLiteral("mainwindow/recentFiles")).toStringList();
}

void MainWindow::clearRecentFiles() {
    QSettings settings;
    settings.remove(QStringLiteral("mainwindow/recentFiles"));
    if (recentFilesMenu_) {
        rebuildRecentFilesMenu();
    }
    FLOG_DEBUG("ui.file", "recent: cleared");
}

void MainWindow::rebuildRecentFilesMenu() {
    if (!recentFilesMenu_) return;
    recentFilesMenu_->clear();

    const QStringList recent = recentFiles();
    if (recent.isEmpty()) {
        recentFilesMenu_->setEnabled(false);
        return;
    }
    recentFilesMenu_->setEnabled(true);

    for (const QString& path : recent) {
        const QString filename = QFileInfo(path).fileName();
        auto* action = recentFilesMenu_->addAction(filename);
        action->setToolTip(path);
        // Toolbutton-style tooltips don't show on menu items by
        // default; status-tip lights up the status bar so the user
        // still sees the full path on hover.
        action->setStatusTip(path);
        connect(action, &QAction::triggered, this, [this, path]() {
            FLOG_DEBUG("ui.file", "open-recent: pick path={}",
                       path.toStdString());
            openByPath(path);
        });
    }

    recentFilesMenu_->addSeparator();
    auto* clearAction = recentFilesMenu_->addAction(tr("&Clear Recent Files"));
    clearAction->setObjectName("clearRecentFilesAction");
    connect(clearAction, &QAction::triggered,
            this, &MainWindow::clearRecentFiles);
}

bool MainWindow::loadFile(const QString& path) {
    if (!player_->load(path.toStdString())) {
        FLOG_DEBUG("ui.file", "open: failed path={}", path.toStdString());
        statusLabel_->setText(tr("No file loaded."));
        return false;
    }
    // MEMO[#26]: remember the absolute path so a subsequent
    // Save / Save As can persist the audio-file reference in
    // the .fdlp. Stored as an absolute path; cross-machine
    // portability is a future concern.
    currentAudioPath_ = QFileInfo(path).absoluteFilePath();

    const auto duration = player_->duration();
    const bool hasAudio = player_->hasAudioOutput();
    FLOG_DEBUG("ui.file",
               "open: path={} duration={} ms audio={}",
               path.toStdString(), duration.count(), hasAudio);
    {
        QSignalBlocker block(positionSlider_);
        positionSlider_->setRange(0, static_cast<int>(duration.count()));
        positionSlider_->setValue(0);
    }
    positionSlider_->setEnabled(true);
    // No audio output (headless CI, broken audio config) → file is
    // still loaded for visualisation and seek, but Play would no-op.
    // Stop stays enabled as a "rewind to start" affordance.
    playButton_->setEnabled(hasAudio);
    stopButton_->setEnabled(true);
    playButton_->setText(tr("Play"));

    // Reset tempo to 100% on every fresh load — old slider state from
    // a previous file shouldn't carry over.
    {
        QSignalBlocker block(tempoSlider_);
        tempoSlider_->setValue(100);
    }
    tempoSlider_->setEnabled(true);
    tempoLabel_->setText(tr("Tempo: 100%"));
    player_->setTempoRatio(1.0);

    // Clear any stale waveform + reset cursor immediately. The fresh
    // overview will arrive asynchronously below.
    waveform_->setOverview(nullptr);
    waveform_->setPositionMs(0);

    // A new file means the previous file's annotations are no
    // longer meaningful — drop them. The widgets + dock observe
    // the models and will repaint to empty automatically. The
    // combined undo history goes too.
    barlineModel_->clear();
    markerModel_->clear();
    loopModel_->clear();
    noteModel_->clear();
    undoHistory_.clear();

    // Disarm any previously-armed loop. loopModel_->clear() above
    // would have invalidated armedLoopId_ via onLoopModelChanged
    // anyway, but resetting wrapPending_ is the explicit reason for
    // doing it here too.
    armedLoopId_.reset();
    cancelPendingWrap();
    if (projectViewerDock_) {
        projectViewerDock_->setArmedLoopId(std::nullopt);
    }
    // Reset the wrap-detection tracker — fresh file, no prior tick
    // observed yet.
    previousPosMs_ = -1;

    // Tell the staff how long this file is (so its msToX mapping
    // works). The waveform gets its duration via the WaveformOverview
    // shared_ptr later.
    staff_->setDurationMs(duration.count());
    staff_->setPositionMs(0);

    // Reset the viewport on every fresh load — old zoom state from
    // a previous file shouldn't carry over (#49). Also re-engages
    // Follow Playback in case a prior manual scroll switched it off.
    resetViewport();
    setFollowPlayback(true);

    statusLabel_->setText(tr("Loaded: %1  (%2 ms)%3")
        .arg(path)
        .arg(duration.count())
        .arg(hasAudio ? QString() : tr("  [no audio output]")));

    if (!positionTimer_) {
        positionTimer_ = new QTimer(this);
        positionTimer_->setInterval(50);
        connect(positionTimer_, &QTimer::timeout,
                this, &MainWindow::updatePosition);
    }
    positionTimer_->start();

    // Kick off the overview build on a worker thread. We detach
    // (rather than store a joinable thread) so a slow build never
    // blocks the GUI when the user opens the next file. A generation
    // counter on the post-back guards against an old build's result
    // landing after a newer file has been loaded.
    const auto generation = ++overviewGeneration_;
    const std::string utf8Path = path.toStdString();
    QPointer<MainWindow> self(this);
    std::thread([self, utf8Path, generation]() {
        audio::Decoder dec;
        if (!dec.open(utf8Path)) {
            FLOG_WARN("waveform", "overview build: decoder failed to open {}",
                      utf8Path);
            return;
        }
        auto built = audio::buildOverview(dec, kOverviewBuckets);
        if (!built) return;
        auto shared = std::make_shared<const audio::WaveformOverview>(
            std::move(*built));
        QMetaObject::invokeMethod(qApp, [self, generation, shared]() {
            if (!self) return;
            if (generation != self->overviewGeneration_.load()) return;
            self->waveform_->setOverview(shared);
            FLOG_DEBUG("waveform",
                       "overview installed: gen={}, buckets={}, dur={} ms",
                       generation, shared->bucketCount(),
                       shared->duration().count());
        }, Qt::QueuedConnection);
    }).detach();

    // MEMO[#26]: a fresh audio load detaches from any previously-
    // bound project file. clear() above fired changed() which
    // already flipped dirty_ true; reset both here so the window
    // title is bare "Fiddler" until the user either saves or
    // adds annotations.
    projectPath_.clear();
    markClean();
    updateWindowTitle();

    return true;
}

void MainWindow::onPlayPause() {
    if (!player_) return;
    if (player_->state() == audio::TransportState::Playing) {
        const auto pos = player_->position().count();
        player_->pause();
        playButton_->setText(tr("Play"));
        FLOG_DEBUG("ui.transport", "pause at={} ms", pos);
        return;
    }

    // MEMO: special-case for issue #13 — the user clicked the
    // play/pause button DURING a wrap-pause. The button label still
    // reads "Pause" at this moment (we deliberately don't flip it
    // when entering the wrap-pause; the loop is conceptually still
    // in playback mode, just experiencing a temporary silence). The
    // user's gesture means "cancel the auto-resume, pause for real":
    //   * stop the wrap timer
    //   * leave the player paused at startMs (where it was seeked
    //     to during wrap entry)
    //   * cancel the depleting countdown
    //   * keep the loop armed (a subsequent Play press will resume,
    //     and the loop wraps as normal on the next endMs crossing)
    // Use Stop if you want to fully disarm.
    if (wrapPending_) {
        cancelPendingWrap();
        playButton_->setText(tr("Play"));
        const auto pos = player_->position().count();
        FLOG_DEBUG("ui.transport",
                   "wrap-pause cancelled via=play-button at={} ms", pos);
        return;
    }

    // Transitioning to Play.
    auto pos = player_->position().count();

    // MEMO: load-bearing UX rule for the armed-loop path. If the
    // user has armed a loop (via the Arm checkbox or by selecting
    // it and reaching here without a double-click) and presses Play
    // while the cursor is OUTSIDE [startMs, endMs), seek to
    // startMs first. The intent of pressing Play with a loop armed
    // is "play the loop now". Without this seek, two confusing
    // cases bite the user:
    //
    //   * pos < startMs: they hear the whole tune up to endMs once
    //     before the loop starts wrapping (could be 30+ seconds of
    //     "wrong" playback before the first wrap fires).
    //
    //   * pos >= endMs: the wrap path fires once on the first GUI
    //     poll, but the user still spent that one frame at the
    //     wrong position. Cleaner to land at startMs immediately.
    //
    // Inside [startMs, endMs) we leave the position alone — that's
    // the "armed mid-listen and resumed" case, where preserving
    // continuity matters.
    if (armedLoopId_.has_value() && loopModel_) {
        if (const auto idx = loopModel_->indexOf(*armedLoopId_)) {
            const auto& loop = loopModel_->loops()[*idx];
            if (pos < loop.startMs || pos >= loop.endMs) {
                player_->seek(std::chrono::milliseconds{loop.startMs});
                pos = loop.startMs;
                previousPosMs_ = pos;
                FLOG_DEBUG("ui.transport",
                           "play-armed-seek id={} to={}",
                           *armedLoopId_, loop.startMs);
            }
        }
    }

    // MEMO: route through startPlayback so the global pre-roll
    // (issue #16) fires uniformly. When pre-roll is disabled or
    // 0 this is an immediate play; otherwise the user gets the
    // "ready, set, go" countdown before audio actually starts.
    // The Ableton-style Follow Playback re-engage lives inside
    // startPlayback so dock-double-click and Play-button alike
    // re-engage it uniformly (#49 smoke).
    startPlayback();
    FLOG_DEBUG("ui.transport",
               "play from={} ms audio={} preroll={}",
               pos, player_->hasAudioOutput(), prerollMs());
}

void MainWindow::onStop() {
    if (!player_) return;
    player_->stop();
    playButton_->setText(tr("Play"));
    QSignalBlocker block(positionSlider_);
    positionSlider_->setValue(0);

    // MEMO: Stop is also the user's "exit loop mode" gesture, per
    // the design discussion. Disarm here so the next Play button
    // press resumes normal (non-wrapping) playback. wrapPending_
    // resets too — any in-flight pause-between-repeats timer
    // becomes a no-op when its lambda checks armedLoopId_.
    const bool wasArmed = armedLoopId_.has_value();
    armedLoopId_.reset();
    cancelPendingWrap();
    if (wasArmed && projectViewerDock_) {
        projectViewerDock_->setArmedLoopId(std::nullopt);
    }
    // Stop rewinds to 0 — sync the wrap-detection tracker too so a
    // subsequent Play won't see a stale previous-position from
    // before the Stop.
    previousPosMs_ = 0;

    FLOG_DEBUG("ui.transport", "stop rewind=0 disarmed={}", wasArmed);
}

void MainWindow::onSeek(int positionMs) {
    // MEMO: this is a primitive called by three different UI sources
    // (waveform-click, staff-click, position-slider drag); the
    // *source* is logged at the call site so each source's log line
    // is distinct ("seek via waveform" vs "seek via slider"). Adding
    // a log here would either duplicate or lose source information.
    if (!player_) return;
    // MEMO: issue #13 — a user-initiated seek during a wrap-pause
    // cancels the auto-resume. The player stays paused at the new
    // position; the user presses Play to resume. Loop arming is
    // unchanged. Without this, a seek past endMs would race the
    // resume timer and cause a double-countdown on the next tick.
    if (wrapPending_) {
        cancelPendingWrap();
        playButton_->setText(tr("Play"));
        FLOG_DEBUG("ui.transport",
                   "wrap-pause cancelled via=seek at={} ms", positionMs);
    }
    player_->seek(std::chrono::milliseconds{positionMs});
    // Sync the previous-position tracker so the next updatePosition
    // tick doesn't see a fake "natural crossing" of endMs from a
    // user-initiated jump. See issue #13 + wrapShouldFire().
    previousPosMs_ = positionMs;

    // MEMO[#40]: drive the playback cursor inline so it follows the
    // user's seek at mouse-move rate (~60 Hz) instead of waiting for
    // the 50 ms position timer. Without this, dragging a marker or
    // waveform-clicking shows a stale cursor for up to 50 ms — which
    // looks like a frozen red line during fast drags. Cheap: both
    // setters just store an int and queue a paint.
    if (waveform_) waveform_->setPositionMs(positionMs);
    if (staff_)    staff_->setPositionMs(positionMs);
}

// ---- zoom / viewport (#49) -----------------------------------------------

void MainWindow::applyViewport(std::int64_t startMs, std::int64_t endMs) {
    if (!waveform_ || !staff_) return;
    // MEMO[#49 smoke]: re-anchor the follow-playback motion
    // detector to the current playhead so the very next
    // updatePosition tick sees "no motion" and skips its
    // page-flip. Without this, the page-flip rule (only fire
    // when the playhead moved since last tick) misfires after a
    // viewport-changing gesture: if the viewport changes from
    // applyViewport but no updatePosition tick has run since the
    // last seek, the previous tick's lastFollowPosMs_ would be
    // stale (the pre-seek value), and the next tick would see
    // the seek as "motion" and immediately undo our viewport.
    if (player_) {
        lastFollowPosMs_ = player_->position().count();
    }

    // setViewport is idempotent; calling it on both widgets even
    // when one has already absorbed the change is cheap (early-out
    // on equal state). The scrollbar update has to be guarded
    // because valueChanged would otherwise loop right back here.
    waveform_->setViewport(startMs, endMs);
    staff_->setViewport(startMs, endMs);

    if (viewportScrollBar_) {
        suppressViewportScrollBarSignals_ = true;
        const auto dur  = waveform_->durationMs();
        const auto span = waveform_->viewportSpanMs();
        if (waveform_->isZoomed()) {
            viewportScrollBar_->setRange(0, static_cast<int>(dur - span));
            viewportScrollBar_->setPageStep(static_cast<int>(span));
            viewportScrollBar_->setValue(
                static_cast<int>(waveform_->viewportStartMs()));
            viewportScrollBar_->setVisible(true);
        } else {
            viewportScrollBar_->setRange(0, 0);
            viewportScrollBar_->setVisible(false);
        }
        suppressViewportScrollBarSignals_ = false;
    }
}

void MainWindow::resetViewport() {
    applyViewport(0, 0);
}

void MainWindow::onViewportChanged(std::int64_t startMs, std::int64_t endMs) {
    // Mirror to the sister widget + scrollbar. applyViewport is
    // re-entry-safe via setViewport's no-op-on-equal-state guard.
    applyViewport(startMs, endMs);
}

void MainWindow::setFollowPlayback(bool follow) {
    if (followPlayback_ == follow) return;
    followPlayback_ = follow;
    if (followPlaybackAction_) {
        QSignalBlocker block(followPlaybackAction_);
        followPlaybackAction_->setChecked(follow);
    }
    FLOG_DEBUG("ui.zoom",
               "follow-playback set={} via=auto", follow);
}

void MainWindow::bringIntoView(std::int64_t targetMs) {
    if (!waveform_ || !waveform_->isZoomed()) return;
    const auto vStart = waveform_->viewportStartMs();
    const auto vEnd   = waveform_->viewportEndMs();
    const auto span   = vEnd - vStart;
    const auto dur    = waveform_->durationMs();
    if (span <= 0) return;

    // Re-center only when the target is OUTSIDE the viewport — if
    // it's already visible, leave the view alone (user might be
    // looking at a specific framing).
    if (targetMs >= vStart && targetMs < vEnd) return;

    // Same 20 % lead-in the follow-playback page-flip uses (#49).
    constexpr double kLeadInFraction = 0.20;
    const auto leadIn =
        static_cast<std::int64_t>(span * kLeadInFraction);
    std::int64_t newStart = targetMs - leadIn;
    std::int64_t newEnd   = newStart + span;
    if (newStart < 0) {
        newStart = 0;
        newEnd   = span;
    }
    if (newEnd > dur) {
        newEnd   = dur;
        newStart = std::max<std::int64_t>(0, dur - span);
    }
    applyViewport(newStart, newEnd);
}

void MainWindow::onZoomIn() {
    if (!waveform_ || waveform_->durationMs() <= 0) return;
    // Anchor on the playback cursor when audio is loaded, else
    // viewport center.
    const std::int64_t cur =
        player_ ? player_->position().count() : 0;
    waveform_->zoomBy(1.0 / std::sqrt(2.0), cur);
    FLOG_DEBUG("ui.zoom", "zoom-in start={} end={} via=key",
               waveform_->viewportStartMs(), waveform_->viewportEndMs());
}

void MainWindow::onZoomOut() {
    if (!waveform_ || waveform_->durationMs() <= 0) return;
    const std::int64_t cur =
        player_ ? player_->position().count() : 0;
    waveform_->zoomBy(std::sqrt(2.0), cur);
    FLOG_DEBUG("ui.zoom", "zoom-out start={} end={} via=key",
               waveform_->viewportStartMs(), waveform_->viewportEndMs());
}

void MainWindow::onZoomFit() {
    if (!waveform_ || waveform_->durationMs() <= 0) return;
    resetViewport();
    FLOG_DEBUG("ui.zoom", "zoom-fit via=key");
}

void MainWindow::onTempoChanged(int percent) {
    if (!player_) return;
    tempoLabel_->setText(tr("Tempo: %1%").arg(percent));
    player_->setTempoRatio(percent / 100.0);
    // MEMO: every value step during a drag emits one debug line.
    // Verbose for slow drags (~75 lines for a 100→25 sweep), but the
    // log is meant to reproduce the user's path — a debounce would
    // hide the intermediate seeks the user actually heard. Filter
    // with `--log-filter='!ui.tempo'` if it gets in the way.
    FLOG_DEBUG("ui.tempo", "tempo={}% ratio={:.4f}",
               percent, percent / 100.0);
}

// ---- score-related slots ------------------------------------------------

void MainWindow::onTapBarline() {
    // The 'B' key — primary barline-placement gesture. We capture
    // the player's current source-time position and ask the model to
    // record it. The model handles sorted insertion + exact-duplicate
    // rejection; both widgets receive `changed()` and repaint.
    if (!player_ || !player_->duration().count()) return;
    const auto pos        = player_->position().count();

    // Near-duplicate guard (#17). Reject if any existing barline is
    // within kMinTapSeparationMs of pos — almost always an accidental
    // double-tap. The model itself only rejects EXACT duplicates;
    // this widening lives at the gesture layer.
    if (const auto near =
            barlineModel_->nearest(pos, kMinTapSeparationMs)) {
        FLOG_DEBUG("ui.score",
                   "tap-place ms={} ignored (within {} ms of index={}) size={}",
                   pos, kMinTapSeparationMs, *near, barlineModel_->size());
        return;
    }

    const auto sizeBefore = barlineModel_->size();
    const auto inserted   = barlineModel_->add(pos);

    // MEMO: BarlineModel::add returns the existing index on
    // duplicates (not nullopt), so `inserted.has_value()` alone
    // doesn't distinguish "really inserted" from "duplicate-rejected".
    // We compare size before/after to tell them apart. A future API
    // cleanup could change BarlineModel::add to return nullopt on
    // duplicate; until then we work around here. The relevant unit
    // test is "BarlineModel: add at an existing ms is rejected
    // silently" — that test pins the current contract.
    const bool actuallyAdded =
        inserted.has_value() && barlineModel_->size() > sizeBefore;

    // Auto-select the newly-placed barline. This makes "tap B, press
    // Del" work as a symmetric pair — without auto-select, the user
    // would have to first click the (1-pixel-wide) tick to select it
    // before Del had anything to remove. The waveform's
    // setSelectedBarline emits barlineSelectionChanged, which the
    // mirror plumbing forwards to the staff. We only re-select on a
    // genuine insert; for a duplicate the existing bar is already
    // there and was likely already selected.
    if (actuallyAdded && waveform_) {
        waveform_->setSelectedBarline(inserted);
    }

    if (actuallyAdded) {
        // Push to the unified LIFO so Ctrl+Z can reverse this
        // placement regardless of what's been done since.
        pushUndoEntry(undo::AddBarline{pos});
        FLOG_DEBUG("ui.score",
                   "tap-place ms={} index={} size={} sel={}",
                   pos, *inserted, barlineModel_->size(), *inserted);
    } else {
        FLOG_DEBUG("ui.score",
                   "tap-place ms={} ignored (duplicate) size={}",
                   pos, barlineModel_->size());
    }
}

void MainWindow::onTapMarker() {
    // The 'M' key — primary marker-placement gesture. Like 'B'
    // for barlines, but markers are auto-named, and the model
    // permits multiple markers at the same ms with different
    // names (see MarkerModel::add).
    if (!player_ || !player_->duration().count()) return;
    const auto pos = player_->position().count();

    // Near-duplicate guard (#17). Reject the tap if any existing
    // marker is within kMinTapSeparationMs of pos. Without this,
    // a rapid double-tap leaves an extra "Mark N+1" sitting on top
    // of "Mark N" in the dock — clutter the user has to undo.
    // The model stays permissive: placing two markers at the same
    // ms is still possible via the dock spinbox or programmatically,
    // just not via the rapid-fire tap gesture.
    if (const auto nearId =
            markerModel_->nearest(pos, kMinTapSeparationMs)) {
        FLOG_DEBUG("ui.score",
                   "tap-marker ms={} ignored (within {} ms of id={}) size={}",
                   pos, kMinTapSeparationMs, *nearId, markerModel_->size());
        return;
    }

    const auto id  = markerModel_->add(pos);

    // Auto-select the newly-placed marker. Same pattern as
    // tap-place barline: makes "tap M, press Del" symmetric.
    if (waveform_) waveform_->setSelectedMarkerId(id);

    pushUndoEntry(undo::AddMarker{id});
    FLOG_DEBUG("ui.score",
               "tap-marker ms={} id={} size={}",
               pos, id, markerModel_->size());
}

void MainWindow::onUndo() {
    // Ctrl+Z — reverse the most recent user-driven mutation.
    // Unlike the old placement-only path, every push site (tap,
    // create-loop, drag commit, dock edit, delete) records its
    // own UndoEntry, so the dispatch is total: pop the back and
    // visit the variant. Under this design there is no "stale
    // entry" case — deletes are themselves entries that re-add.
    if (undoHistory_.empty()) {
        FLOG_DEBUG("ui.score", "undo empty (no-op)");
        return;
    }
    const auto entry = undoHistory_.back();
    undoHistory_.pop_back();

    // applyingUndo_ guards every push site so the model mutations
    // we issue here don't push their own entries (which would
    // amount to "redo on next Ctrl+Z" — out of scope per #20).
    applyingUndo_ = true;
    const char* kind = "?";
    std::visit([&](const auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, undo::AddBarline>) {
            // Reverse a placement by removing the barline at the
            // recorded ms. The model is keyed by sourceMs so we
            // look the index up just-in-time — barlines may have
            // shifted in the sorted vector if other ones were
            // added after this one.
            const auto it = std::lower_bound(
                barlineModel_->barlines().begin(),
                barlineModel_->barlines().end(),
                e.sourceMs);
            if (it != barlineModel_->barlines().end() && *it == e.sourceMs) {
                const auto idx = static_cast<std::size_t>(
                    it - barlineModel_->barlines().begin());
                barlineModel_->removeAt(idx);
            }
            kind = "add-barline";
        } else if constexpr (std::is_same_v<E, undo::AddMarker>) {
            markerModel_->remove(e.id);
            kind = "add-marker";
        } else if constexpr (std::is_same_v<E, undo::AddLoop>) {
            loopModel_->remove(e.id);
            kind = "add-loop";
        } else if constexpr (std::is_same_v<E, undo::AddNote>) {
            noteModel_->remove(e.id);
            kind = "add-note";
        } else if constexpr (std::is_same_v<E, undo::EditMarkerPos>) {
            markerModel_->setPosition(e.id, e.prevSourceMs);
            kind = "edit-marker-pos";
        } else if constexpr (std::is_same_v<E, undo::EditLoopRange>) {
            loopModel_->setRange(e.id, e.prevStartMs, e.prevEndMs);
            kind = "edit-loop-range";
        } else if constexpr (std::is_same_v<E, undo::EditNoteInterval>) {
            noteModel_->setInterval(e.id, e.prevStartMs, e.prevEndMs);
            kind = "edit-note-interval";
        } else if constexpr (std::is_same_v<E, undo::EditNotePitch>) {
            noteModel_->setPitch(e.id, e.prevMidi);
            kind = "edit-note-pitch";
        } else if constexpr (std::is_same_v<E, undo::RenameMarker>) {
            markerModel_->rename(e.id, e.prevName);
            kind = "rename-marker";
        } else if constexpr (std::is_same_v<E, undo::RenameLoop>) {
            loopModel_->rename(e.id, e.prevName);
            kind = "rename-loop";
        } else if constexpr (std::is_same_v<E, undo::RenameNote>) {
            noteModel_->rename(e.id, e.prevName);
            kind = "rename-note";
        } else if constexpr (std::is_same_v<E, undo::DeleteBarline>) {
            barlineModel_->add(e.sourceMs);
            kind = "delete-barline";
        } else if constexpr (std::is_same_v<E, undo::DeleteMarker>) {
            markerModel_->addWithId(e.id, e.sourceMs, e.name);
            kind = "delete-marker";
        } else if constexpr (std::is_same_v<E, undo::DeleteLoop>) {
            loopModel_->addWithId(e.id, e.startMs, e.endMs, e.name);
            kind = "delete-loop";
        } else if constexpr (std::is_same_v<E, undo::DeleteNote>) {
            noteModel_->addWithId(e.id, e.startMs, e.endMs, e.midi, e.name);
            kind = "delete-note";
        } else if constexpr (std::is_same_v<E, undo::EditPrerollMs>) {
            setPrerollMs(e.prevMs);
            kind = "edit-preroll-ms";
        } else if constexpr (std::is_same_v<E, undo::EditPrerollEnabled>) {
            setPrerollEnabled(e.prevEnabled);
            kind = "edit-preroll-enabled";
        }
    }, entry);
    applyingUndo_ = false;

    // The pop above may have brought undoHistory_ size back to
    // savedUndoSize_ — recompute so the asterisk clears when the
    // user has undone their way back to the saved state.
    recomputeDirty();

    FLOG_DEBUG("ui.score",
               "undo kind={} bar-size={} marker-size={} "
               "loop-size={} note-size={}",
               kind,
               barlineModel_->size(), markerModel_->size(),
               loopModel_->size(), noteModel_->size());
}

void MainWindow::onCreateLoop() {
    // MEMO: read primary + secondary anchors from the waveform —
    // staff and dock mirror the same values. Both must be populated
    // for the gesture to fire.
    if (!waveform_ || !loopModel_) return;
    const auto primMs = waveform_->primaryAnchorMs();
    const auto secMs  = waveform_->secondaryAnchorMs();
    if (!primMs || !secMs) {
        FLOG_DEBUG("ui.score",
                   "create-loop ignored need=2-anchors primary={} secondary={}",
                   primMs.has_value() ? *primMs : -1,
                   secMs.has_value()  ? *secMs  : -1);
        return;
    }
    if (*primMs == *secMs) {
        // Both anchors at the same ms — a degenerate range. Refuse
        // rather than create a 1-ms loop the user almost certainly
        // didn't mean.
        FLOG_DEBUG("ui.score",
                   "create-loop ignored degenerate ms={}", *primMs);
        return;
    }
    const std::int64_t startMs = std::min(*primMs, *secMs);
    const std::int64_t endMs   = std::max(*primMs, *secMs);
    const auto id = loopModel_->add(startMs, endMs);
    if (id == 0) {
        // Should be unreachable given the start<end guard above, but
        // log defensively in case LoopModel grows new validation.
        FLOG_DEBUG("ui.score",
                   "create-loop rejected by model start={} end={}",
                   startMs, endMs);
        return;
    }
    pushUndoEntry(undo::AddLoop{id});

    // Clear the secondary anchor so the gesture is "spent" and a
    // fresh L press needs a fresh Ctrl+click.
    waveform_->setSecondaryAnchorMs(std::nullopt);

    // Auto-select the new loop in the dock so its property page
    // shows immediately. Mirrors the auto-select pattern from
    // tap-place barline / marker.
    if (waveform_) waveform_->setSelectedLoopId(id);

    FLOG_DEBUG("ui.score",
               "create-loop id={} start={} end={} size={}",
               id, startMs, endMs, loopModel_->size());
}

void MainWindow::onTuneTypePresetChosen(int comboIndex) {
    if (comboIndex < 0
        || comboIndex >= static_cast<int>(kTuneTypePresets.size())) {
        return;
    }
    const auto& preset = kTuneTypePresets[static_cast<std::size_t>(comboIndex)];
    barlineModel_->setTimeSignature({
        preset.numerator,
        preset.denominator,
        QString::fromUtf8(preset.tuneType),
    });
    // MEMO[#26]: tune-type isn't in the undo history yet (would
    // be a small extension to UndoEntry; filed as future work).
    // For now, flag the project as dirty so Save still catches
    // the change in the .fdlp file.
    nonUndoableDirty_ = true;
    recomputeDirty();
    FLOG_DEBUG("ui.score",
               "time-sig label={} numerator={} denominator={}",
               preset.tuneType, preset.numerator, preset.denominator);
}

void MainWindow::onWaveformBarlineSelectionChanged(
    std::optional<std::size_t> index)
{
    // MEMO: see the mirroringSelection_ comment in MainWindow.h —
    // we early-return when called in response to our own forward,
    // so only the *originating* slot logs and propagates.
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (staff_) staff_->setSelectedBarline(index);
    mirroringSelection_ = false;

    if (index.has_value()) {
        FLOG_DEBUG("ui.score", "select index={} via=waveform size={}",
                   *index, barlineModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select cleared via=waveform size={}",
                   barlineModel_->size());
    }
}

void MainWindow::onStaffBarlineSelectionChanged(
    std::optional<std::size_t> index)
{
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (waveform_) waveform_->setSelectedBarline(index);
    mirroringSelection_ = false;

    if (index.has_value()) {
        FLOG_DEBUG("ui.score", "select index={} via=staff size={}",
                   *index, barlineModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select cleared via=staff size={}",
                   barlineModel_->size());
    }
}

void MainWindow::onWaveformMarkerSelectionChanged(
    std::optional<std::int64_t> id)
{
    // MEMO: three-way mirror across waveform / staff / dock. The
    // mirroringSelection_ guard is shared with the barline mirror —
    // a single bool is enough because at most one originating event
    // is in flight at a time.
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (staff_)             staff_->setSelectedMarkerId(id);
    if (projectViewerDock_) projectViewerDock_->setSelectedMarkerId(id);
    mirroringSelection_ = false;

    if (id.has_value()) {
        FLOG_DEBUG("ui.score", "select-marker id={} via=waveform size={}",
                   *id, markerModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select-marker cleared via=waveform size={}",
                   markerModel_->size());
    }
}

void MainWindow::onStaffMarkerSelectionChanged(
    std::optional<std::int64_t> id)
{
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (waveform_)          waveform_->setSelectedMarkerId(id);
    if (projectViewerDock_) projectViewerDock_->setSelectedMarkerId(id);
    mirroringSelection_ = false;

    if (id.has_value()) {
        FLOG_DEBUG("ui.score", "select-marker id={} via=staff size={}",
                   *id, markerModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select-marker cleared via=staff size={}",
                   markerModel_->size());
    }
}

void MainWindow::onMarkerActivated(std::int64_t id) {
    // "Jump and play" — double-click handler from the dock.
    // We seek to the marker's exact source-time, then route
    // through startPlayback so global pre-roll (issue #16) fires
    // uniformly. With pre-roll enabled the user gets a moment to
    // grab the violin between the click and audio starting.
    if (!markerModel_ || !player_) return;
    const auto idx = markerModel_->indexOf(id);
    if (!idx) return;
    const auto ms =
        static_cast<int>(markerModel_->markers()[*idx].sourceMs);

    // MEMO: route through onSeek so the existing seek log fires
    // and the position slider updates via the timer chain.
    onSeek(ms);
    // MEMO[#49 smoke]: dock activation is an explicit "navigate
    // here" intent — bring the marker into the viewport unconditionally,
    // independent of the Follow Playback toggle. The follow flag
    // is for "should the view chase the playhead during playback",
    // which is a different question.
    bringIntoView(ms);

    if (player_->state() != audio::TransportState::Playing) {
        startPlayback();
    }

    FLOG_DEBUG("ui.score",
               "marker-activated id={} ms={} via=dock", id, ms);
}

bool MainWindow::wrapShouldFire(std::int64_t previousPosMs,
                                std::int64_t currentPosMs,
                                std::int64_t endMs) noexcept {
    // First-tick guard: no previous position observed yet, so we
    // can't tell whether the current position came from natural
    // playback or from initial state. Refuse to wrap on the first
    // tick — the next one will have a real `previousPosMs`.
    if (previousPosMs < 0) return false;
    // Natural forward crossing of endMs is the only thing that
    // triggers a wrap. Both-before, both-after, and after→before
    // are all no-ops.
    return previousPosMs < endMs && currentPosMs >= endMs;
}

void MainWindow::enterWrapPauseForTest(std::int64_t loopId, int prerollMs) {
    // MEMO: production wrap entry lives in updatePosition's wrap
    // branch and depends on audio actually advancing position past
    // endMs. Headless CI hosts can't reliably reach that state. This
    // seam mirrors the production setup verbatim minus the seek (the
    // caller is responsible for placing the player wherever they
    // want before calling). Both MainWindow's armedLoopId_ and the
    // dock's setArmedLoopId are set so the post-condition matches
    // "user armed this loop, played past endMs, wrap-pause begun".
    armedLoopId_      = loopId;
    wrapPending_      = true;
    wrapTargetLoopId_ = loopId;
    if (projectViewerDock_) {
        projectViewerDock_->setArmedLoopId(loopId);
        projectViewerDock_->startCountdown(prerollMs);
    }
    if (wrapTimer_) wrapTimer_->start(prerollMs);
}

void MainWindow::startPlayback() {
    // Single source of truth for "transition the player to playing".
    // Routes through a pre-roll wrap-pause when the effective
    // pre-roll is > 0 (checkbox enabled AND prerollMs_ > 0) so the
    // user gets a "ready, set, go" countdown before audio starts;
    // otherwise plays immediately. Fires uniformly whether or not
    // a loop is armed — issue #16 generalised this beyond the
    // armed-loop case so simply pressing Play (or activating a
    // marker / loop) also gives the player time to switch from
    // mouse to instrument.
    //
    // MEMO: do NOT call from the wrap timer's lambda (timeout) —
    // that fires AFTER the wrap-pause that was the pre-roll for
    // the next iteration. Calling this would loop forever.
    if (!player_) return;

    // MEMO[#49 smoke]: every user-initiated transport start
    // re-engages Follow Playback (Ableton convention). Lives here
    // — rather than at each call site — so Play button, dock
    // double-click on a marker, and dock double-click on a loop
    // all re-engage uniformly. The wrap-timer's resume path does
    // NOT route through here (see MEMO above), so a natural
    // between-iterations wrap doesn't touch Follow either way.
    setFollowPlayback(true);

    const int effective = prerollMs();
    if (effective > 0) {
        // Enter pre-roll wrap-pause. Same machinery as between-
        // repeats wrap (issue #13's wrapTimer_), just initiated
        // from the start-playing path rather than from a natural
        // endMs crossing. wrapTargetLoopId_ may be nullopt (generic
        // Play, no specific loop to defend against re-arming).
        wrapPending_      = true;
        wrapTargetLoopId_ = armedLoopId_;
        player_->pause();
        if (projectViewerDock_) {
            projectViewerDock_->startCountdown(effective);
        }
        wrapTimer_->start(effective);
        // Conceptually we're entering "playback mode with a brief
        // silence first" — flip the button label to "Pause" so the
        // user sees that clicking again would cancel the pre-roll.
        // (Same Option-2 semantics as the between-repeats wrap.)
        playButton_->setText(tr("Pause"));
        const std::string idStr = armedLoopId_.has_value()
            ? std::to_string(*armedLoopId_)
            : std::string{"none"};
        FLOG_DEBUG("ui.transport",
                   "preroll-start id={} ms={}", idStr, effective);
        return;
    }

    // Pre-roll disabled or zero: play immediately.
    player_->play();
    playButton_->setText(tr("Pause"));
}

void MainWindow::cancelPendingWrap() {
    // Idempotent — safe to call when there's no wrap pending. The
    // timer's stop() is a no-op when the timer isn't active, and
    // the dock's cancelCountdown() short-circuits when the
    // countdown isn't running.
    const bool wasPending = wrapPending_;
    if (wrapTimer_) wrapTimer_->stop();
    wrapPending_ = false;
    wrapTargetLoopId_.reset();
    if (projectViewerDock_) projectViewerDock_->cancelCountdown();
    // MEMO: during wrap-pause we deliberately keep the button label
    // as "Pause" (the user might want to cancel the auto-resume —
    // see issue #13). Once the wrap IS cancelled the player is
    // paused for real, so the label needs to flip to "Play". All
    // cancel-paths (Play during wrap, seek during wrap, Stop,
    // disarm, toggle-pre-roll-off) end up here, so handling the
    // label centrally avoids each caller forgetting it (issue #16
    // smoke test 6.1 was the toggle-off case missing this).
    if (wasPending && playButton_) {
        playButton_->setText(tr("Play"));
    }
}

void MainWindow::onLoopActivated(std::int64_t id) {
    // "Jump and play" for loops — arm + seek to startMs + play.
    // The transport then wraps around back to startMs whenever the
    // GUI poll sees position >= endMs.
    if (!loopModel_ || !player_) return;
    const auto idx = loopModel_->indexOf(id);
    if (!idx) return;
    const auto& loop = loopModel_->loops()[*idx];

    armedLoopId_ = id;
    cancelPendingWrap();
    if (projectViewerDock_) {
        projectViewerDock_->setArmedLoopId(id);
    }

    onSeek(static_cast<int>(loop.startMs));
    // MEMO[#49 smoke]: dock activation always brings the target
    // into view, regardless of Follow Playback state — see
    // onMarkerActivated for the same convention.
    bringIntoView(loop.startMs);
    // Route through playArmedLoop so the pre-roll (issue #16) fires
    // before playback when prerollMs_ > 0. With pre-roll, double-
    // click means "jump, give me a moment to prepare, then play"
    // — practice-tool feel.
    startPlayback();

    FLOG_DEBUG("ui.score",
               "loop-activated id={} start={} end={} preroll={} via=dock",
               id, loop.startMs, loop.endMs, prerollMs_);
}

void MainWindow::onLoopArmToggleRequested(std::int64_t id, bool armed) {
    // The Armed checkbox path. Differs from onLoopActivated in two
    // ways: (1) no auto-seek (the user might be mid-listen — arming
    // should just enable wrap-around at endMs, not jolt them back to
    // start), (2) no auto-play (same rationale).
    if (!loopModel_) return;
    if (armed) {
        if (!loopModel_->indexOf(id)) return;
        armedLoopId_ = id;
        cancelPendingWrap();
        if (projectViewerDock_) {
            projectViewerDock_->setArmedLoopId(id);
        }
        FLOG_DEBUG("ui.score", "loop-armed id={} via=checkbox", id);
    } else {
        if (armedLoopId_ != id) return;
        armedLoopId_.reset();
        cancelPendingWrap();
        if (projectViewerDock_) {
            projectViewerDock_->setArmedLoopId(std::nullopt);
        }
        FLOG_DEBUG("ui.score", "loop-disarmed id={} via=checkbox", id);
    }
}

void MainWindow::onLoopModelChanged() {
    // If the armed loop has been removed (Del or Ctrl+Z, or any
    // future scripted mutation), drop the armed state. Edits to
    // start/end/pause keep the same ID so they don't disturb arming.
    if (!armedLoopId_.has_value()) return;
    if (loopModel_ && loopModel_->indexOf(*armedLoopId_).has_value()) {
        return;   // loop still exists, possibly with new range
    }
    const auto droppedId = *armedLoopId_;
    armedLoopId_.reset();
    cancelPendingWrap();
    if (projectViewerDock_) {
        projectViewerDock_->setArmedLoopId(std::nullopt);
    }
    FLOG_DEBUG("ui.score", "loop-disarmed id={} reason=removed-from-model",
               droppedId);
}

void MainWindow::onDockMarkerSelectionChanged(
    std::optional<std::int64_t> id)
{
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (waveform_) waveform_->setSelectedMarkerId(id);
    if (staff_)    staff_->setSelectedMarkerId(id);
    mirroringSelection_ = false;

    if (id.has_value()) {
        FLOG_DEBUG("ui.score", "select-marker id={} via=dock size={}",
                   *id, markerModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select-marker cleared via=dock size={}",
                   markerModel_->size());
    }
}

void MainWindow::onWaveformLoopSelectionChanged(
    std::optional<std::int64_t> id)
{
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (staff_)             staff_->setSelectedLoopId(id);
    if (projectViewerDock_) projectViewerDock_->setSelectedLoopId(id);
    mirroringSelection_ = false;

    if (id.has_value()) {
        FLOG_DEBUG("ui.score", "select-loop id={} via=waveform size={}",
                   *id, loopModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select-loop cleared via=waveform size={}",
                   loopModel_->size());
    }
}

void MainWindow::onStaffLoopSelectionChanged(
    std::optional<std::int64_t> id)
{
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (waveform_)          waveform_->setSelectedLoopId(id);
    if (projectViewerDock_) projectViewerDock_->setSelectedLoopId(id);
    mirroringSelection_ = false;

    if (id.has_value()) {
        FLOG_DEBUG("ui.score", "select-loop id={} via=staff size={}",
                   *id, loopModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select-loop cleared via=staff size={}",
                   loopModel_->size());
    }
}

void MainWindow::onDockLoopSelectionChanged(
    std::optional<std::int64_t> id)
{
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (waveform_) waveform_->setSelectedLoopId(id);
    if (staff_)    staff_->setSelectedLoopId(id);
    mirroringSelection_ = false;

    if (id.has_value()) {
        FLOG_DEBUG("ui.score", "select-loop id={} via=dock size={}",
                   *id, loopModel_->size());
    } else {
        FLOG_DEBUG("ui.score", "select-loop cleared via=dock size={}",
                   loopModel_->size());
    }
}

void MainWindow::onWaveformSecondaryAnchorChanged(
    std::optional<std::int64_t> ms)
{
    // MEMO: secondary anchor mirror — staff visualisation should
    // match the waveform when the user Ctrl+clicks on either widget.
    // Reuses the same single-bool guard the selection mirrors share.
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (staff_) staff_->setSecondaryAnchorMs(ms);
    mirroringSelection_ = false;

    if (ms.has_value()) {
        FLOG_DEBUG("ui.score", "secondary-anchor ms={} via=waveform", *ms);
    } else {
        FLOG_DEBUG("ui.score", "secondary-anchor cleared via=waveform");
    }
}

void MainWindow::onStaffSecondaryAnchorChanged(
    std::optional<std::int64_t> ms)
{
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    if (waveform_) waveform_->setSecondaryAnchorMs(ms);
    mirroringSelection_ = false;

    if (ms.has_value()) {
        FLOG_DEBUG("ui.score", "secondary-anchor ms={} via=staff", *ms);
    } else {
        FLOG_DEBUG("ui.score", "secondary-anchor cleared via=staff");
    }
}

void MainWindow::onDockLoopAnchorAddRequested() {
    // MEMO: the dock doesn't know whether the current primary is a
    // marker or a barline (or even on a different widget) — the
    // canonical answer lives on the waveform via primaryAnchorMs().
    // This handler reads that value at the moment of the Ctrl+click
    // (BEFORE the dock's own selection change has been processed)
    // and pushes it as the secondary. If there's no primary yet it's
    // a no-op — same rule as Ctrl+click on the widget when nothing
    // is selected.
    if (!waveform_) return;
    const auto ms = waveform_->primaryAnchorMs();
    if (!ms.has_value()) return;
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    waveform_->setSecondaryAnchorMs(ms);
    if (staff_) staff_->setSecondaryAnchorMs(ms);
    mirroringSelection_ = false;
    FLOG_DEBUG("ui.score",
               "secondary-anchor ms={} via=dock-ctrl-click", *ms);
}

void MainWindow::onDockLoopAnchorClearRequested() {
    if (!waveform_) return;
    // Skip the no-op case so the log line only fires when the
    // secondary actually clears.
    if (!waveform_->secondaryAnchorMs().has_value()) return;
    if (mirroringSelection_) return;
    mirroringSelection_ = true;
    waveform_->setSecondaryAnchorMs(std::nullopt);
    if (staff_) staff_->setSecondaryAnchorMs(std::nullopt);
    mirroringSelection_ = false;
    FLOG_DEBUG("ui.score", "secondary-anchor cleared via=dock-click");
}

void MainWindow::onBarlineDeleteRequested(std::size_t index) {
    // Either widget can fire this (via its Del-key handler) when it
    // has focus. Both route through the same slot — the model is
    // the single source of truth, and its `changed()` signal will
    // repaint both views. Snapshot the sourceMs BEFORE the remove
    // so undo can re-add the same barline at the same position.
    if (index >= barlineModel_->size()) return;
    const auto sourceMs = barlineModel_->barlines()[index];
    barlineModel_->removeAt(index);
    pushUndoEntry(undo::DeleteBarline{sourceMs});
    FLOG_DEBUG("ui.score", "delete index={} via=widget-key size={}",
               index, barlineModel_->size());
}

void MainWindow::onMarkerDeleteRequested(std::int64_t id) {
    // Same shape as onBarlineDeleteRequested but keyed by the
    // marker's stable ID. Snapshot the marker's full state
    // before removing it so Ctrl+Z can re-add it with the same
    // id and name (via MarkerModel::addWithId).
    const auto idx = markerModel_->indexOf(id);
    if (!idx) return;
    const auto& m = markerModel_->markers()[*idx];
    const auto sourceMs = m.sourceMs;
    const auto name     = m.name;
    markerModel_->remove(id);
    pushUndoEntry(undo::DeleteMarker{id, sourceMs, name});
    FLOG_DEBUG("ui.score", "delete-marker id={} via=widget-key size={}",
               id, markerModel_->size());
}

void MainWindow::onLoopDeleteRequested(std::int64_t id) {
    // Fired by the dock when the user presses Del on a loop row.
    // Snapshot startMs/endMs/name pre-delete so undo restores the
    // exact loop with its original id (selection, armed-state
    // and dock highlight all key off id).
    const auto idx = loopModel_->indexOf(id);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    const auto startMs = l.startMs;
    const auto endMs   = l.endMs;
    const auto name    = l.name;
    loopModel_->remove(id);
    pushUndoEntry(undo::DeleteLoop{id, startMs, endMs, name});
    FLOG_DEBUG("ui.score", "delete-loop id={} via=dock-key size={}",
               id, loopModel_->size());
}

void MainWindow::commitMarkerDrag(std::int64_t id,
                                  std::int64_t fromMs,
                                  std::int64_t toMs,
                                  const char*  via) {
    // Zero-distance drag — release at the press point. Treat as a
    // click: nothing to commit, nothing to undo, just clear the
    // ghost in case the source widget set one before the threshold
    // was crossed (it shouldn't have, but be defensive).
    if (fromMs == toMs) {
        if (waveform_) waveform_->clearDragGhost();
        if (staff_)    staff_->clearDragGhost();
        return;
    }
    if (!markerModel_) return;
    pushUndoEntry(undo::EditMarkerPos{id, fromMs});
    markerModel_->setPosition(id, toMs);
    // Source widget cleared its own ghost in mouseRelease; clear
    // the sister so it stops painting from the now-stale ghost
    // (its paint should return to the model's just-updated value).
    if (waveform_) waveform_->clearDragGhost();
    if (staff_)    staff_->clearDragGhost();
    FLOG_DEBUG("ui.score",
               "marker-drag id={} from={} to={} via={}",
               id, fromMs, toMs, via);
}

void MainWindow::commitLoopDrag(std::int64_t id,
                                bool         isStart,
                                std::int64_t fromMs,
                                std::int64_t toMs,
                                const char*  via) {
    if (fromMs == toMs) {
        if (waveform_) waveform_->clearDragGhost();
        if (staff_)    staff_->clearDragGhost();
        return;
    }
    if (!loopModel_) return;
    const auto idx = loopModel_->indexOf(id);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    // The model's still at the pre-drag values; the unchanged edge
    // is whatever the signal didn't claim was being dragged.
    const std::int64_t newStart = isStart ? toMs   : l.startMs;
    const std::int64_t newEnd   = isStart ? l.endMs : toMs;
    pushUndoEntry(undo::EditLoopRange{id, l.startMs, l.endMs});
    loopModel_->setRange(id, newStart, newEnd);
    if (waveform_) waveform_->clearDragGhost();
    if (staff_)    staff_->clearDragGhost();
    FLOG_DEBUG("ui.score",
               "loop-drag id={} edge={} from={} to={} via={}",
               id, isStart ? "start" : "end", fromMs, toMs, via);
}

void MainWindow::commitLoopMoveDrag(std::int64_t id,
                                    std::int64_t fromStartMs,
                                    std::int64_t fromEndMs,
                                    std::int64_t toStartMs,
                                    std::int64_t toEndMs,
                                    const char*  via) {
    // Zero-distance drag — defensive cleanup; no model write.
    if (fromStartMs == toStartMs && fromEndMs == toEndMs) {
        if (waveform_) waveform_->clearDragGhost();
        if (staff_)    staff_->clearDragGhost();
        return;
    }
    if (!loopModel_) return;
    if (!loopModel_->indexOf(id).has_value()) return;
    pushUndoEntry(undo::EditLoopRange{id, fromStartMs, fromEndMs});
    loopModel_->setRange(id, toStartMs, toEndMs);
    if (waveform_) waveform_->clearDragGhost();
    if (staff_)    staff_->clearDragGhost();
    FLOG_DEBUG("ui.score",
               "loop-move-drag id={} from=[{}..{}] to=[{}..{}] via={}",
               id, fromStartMs, fromEndMs, toStartMs, toEndMs, via);
}

void MainWindow::onLoopCreateCommitted(std::int64_t startMs,
                                       std::int64_t endMs,
                                       const char*  via) {
    if (!loopModel_) return;
    if (endMs <= startMs) {
        FLOG_WARN("ui.score",
                  "loop-create rejected reason=degenerate start={} end={}",
                  startMs, endMs);
        if (waveform_) waveform_->clearDragGhost();
        if (staff_)    staff_->clearDragGhost();
        return;
    }
    const auto id = loopModel_->add(startMs, endMs);
    if (id == 0) {
        FLOG_WARN("ui.score",
                  "loop-create rejected start={} end={}", startMs, endMs);
        if (waveform_) waveform_->clearDragGhost();
        if (staff_)    staff_->clearDragGhost();
        return;
    }
    pushUndoEntry(undo::AddLoop{id});
    // Select the new loop so the dock's property page opens for
    // immediate naming / edge tweak — same UX as note-create.
    if (waveform_) waveform_->setSelectedLoopId(id);
    if (staff_)    staff_->setSelectedLoopId(id);
    if (waveform_) waveform_->clearDragGhost();
    if (staff_)    staff_->clearDragGhost();
    FLOG_DEBUG("ui.score",
               "loop-create id={} start={} end={} via={} size={}",
               id, startMs, endMs, via, loopModel_->size());
}

void MainWindow::onDockMarkerRenameRequested(std::int64_t id,
                                              QString      name) {
    const auto idx = markerModel_->indexOf(id);
    if (!idx) return;
    const auto prevName = markerModel_->markers()[*idx].name;
    if (prevName == name) return;   // no-op edit, no history push
    pushUndoEntry(undo::RenameMarker{id, prevName});
    markerModel_->rename(id, std::move(name));
    FLOG_DEBUG("ui.score",
               "rename-marker id={} via=dock-name", id);
}

void MainWindow::onDockMarkerPositionEditRequested(std::int64_t id,
                                                   std::int64_t newMs) {
    const auto idx = markerModel_->indexOf(id);
    if (!idx) return;
    const auto prevMs = markerModel_->markers()[*idx].sourceMs;
    if (prevMs == newMs) return;
    pushUndoEntry(undo::EditMarkerPos{id, prevMs});
    markerModel_->setPosition(id, newMs);
    FLOG_DEBUG("ui.score",
               "edit-marker-pos id={} from={} to={} via=dock-spinbox",
               id, prevMs, newMs);
}

void MainWindow::onDockLoopRenameRequested(std::int64_t id,
                                           QString      name) {
    const auto idx = loopModel_->indexOf(id);
    if (!idx) return;
    const auto prevName = loopModel_->loops()[*idx].name;
    if (prevName == name) return;
    pushUndoEntry(undo::RenameLoop{id, prevName});
    loopModel_->rename(id, std::move(name));
    FLOG_DEBUG("ui.score",
               "rename-loop id={} via=dock-name", id);
}

void MainWindow::onDockLoopRangeEditRequested(std::int64_t id,
                                              std::int64_t newStartMs,
                                              std::int64_t newEndMs) {
    const auto idx = loopModel_->indexOf(id);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    const auto prevStartMs = l.startMs;
    const auto prevEndMs   = l.endMs;
    if (prevStartMs == newStartMs && prevEndMs == newEndMs) return;
    pushUndoEntry(undo::EditLoopRange{id, prevStartMs, prevEndMs});
    loopModel_->setRange(id, newStartMs, newEndMs);
    FLOG_DEBUG("ui.score",
               "edit-loop-range id={} from=[{},{}] to=[{},{}] via=dock-spinbox",
               id, prevStartMs, prevEndMs, newStartMs, newEndMs);
}

// ---- Note flow (Step 6.1) ------------------------------------------------

void MainWindow::onNoteActivated(std::int64_t id) {
    // Double-click on a note row in the dock: seek to the note's
    // start and start playback. Same "jump-and-play" idiom as
    // markers and loops. 6.2 will additionally fire a reference
    // tone at the note's pitch.
    const auto idx = noteModel_->indexOf(id);
    if (!idx) return;
    const auto& n = noteModel_->notes()[*idx];
    onSeek(static_cast<int>(n.startMs));
    if (player_ && player_->hasAudioOutput()
        && player_->state() != audio::TransportState::Playing) {
        onPlayPause();
    }
    FLOG_DEBUG("ui.score", "note-activated id={} startMs={} midi={}",
               id, n.startMs, n.midi);
}

void MainWindow::onNoteDeleteRequested(std::int64_t id) {
    // Snapshot the full state pre-delete so Ctrl+Z can restore
    // the same id + interval + pitch + name via NoteModel::addWithId.
    const auto idx = noteModel_->indexOf(id);
    if (!idx) return;
    const auto& n = noteModel_->notes()[*idx];
    const auto startMs = n.startMs;
    const auto endMs   = n.endMs;
    const auto midi    = n.midi;
    const auto name    = n.name;
    noteModel_->remove(id);
    pushUndoEntry(undo::DeleteNote{id, startMs, endMs, midi, name});
    FLOG_DEBUG("ui.score", "delete-note id={} size={}",
               id, noteModel_->size());
}

void MainWindow::onDockNoteRenameRequested(std::int64_t id,
                                           QString      name) {
    const auto idx = noteModel_->indexOf(id);
    if (!idx) {
        FLOG_WARN("ui.score",
                  "rename-note id={} via=dock-name skipped reason=stale-id",
                  id);
        return;
    }
    const auto prevName = noteModel_->notes()[*idx].name;
    if (prevName == name) {
        FLOG_DEBUG("ui.score",
                   "rename-note id={} via=dock-name no-op-equal name='{}'",
                   id, name.toStdString());
        return;
    }
    pushUndoEntry(undo::RenameNote{id, prevName});
    noteModel_->rename(id, std::move(name));
    FLOG_DEBUG("ui.score", "rename-note id={} via=dock-name "
               "from='{}' to='{}'",
               id, prevName.toStdString(),
               noteModel_->notes()[*idx].name.toStdString());
}

void MainWindow::onDockNoteIntervalEditRequested(std::int64_t id,
                                                 std::int64_t newStartMs,
                                                 std::int64_t newEndMs) {
    const auto idx = noteModel_->indexOf(id);
    if (!idx) {
        FLOG_WARN("ui.score",
                  "edit-note-interval id={} skipped reason=stale-id", id);
        return;
    }
    const auto& n = noteModel_->notes()[*idx];
    const auto prevStartMs = n.startMs;
    const auto prevEndMs   = n.endMs;
    if (prevStartMs == newStartMs && prevEndMs == newEndMs) {
        FLOG_DEBUG("ui.score",
                   "edit-note-interval id={} via=dock-spinbox "
                   "no-op-equal start={} end={}",
                   id, newStartMs, newEndMs);
        return;
    }
    pushUndoEntry(undo::EditNoteInterval{id, prevStartMs, prevEndMs});
    noteModel_->setInterval(id, newStartMs, newEndMs);
    FLOG_DEBUG("ui.score",
               "edit-note-interval id={} from=[{},{}] to=[{},{}] "
               "via=dock-spinbox",
               id, prevStartMs, prevEndMs, newStartMs, newEndMs);
}

void MainWindow::onDockNotePitchEditRequested(std::int64_t id,
                                              int          newMidi) {
    const auto idx = noteModel_->indexOf(id);
    if (!idx) {
        FLOG_WARN("ui.score",
                  "edit-note-pitch id={} skipped reason=stale-id", id);
        return;
    }
    const auto prevMidi = noteModel_->notes()[*idx].midi;
    if (prevMidi == newMidi) {
        FLOG_DEBUG("ui.score",
                   "edit-note-pitch id={} via=dock-pitch no-op-equal midi={}",
                   id, newMidi);
        return;
    }
    pushUndoEntry(undo::EditNotePitch{id, prevMidi});
    noteModel_->setPitch(id, newMidi);
    FLOG_DEBUG("ui.score",
               "edit-note-pitch id={} from={} to={} via=dock-pitch",
               id, prevMidi, newMidi);
}

void MainWindow::onDockAddNoteRequested() {
    // Dock entered Empty mode and the user clicked "New Note ...".
    // We compute defaults from transport state and ask the dock to
    // enter NewDraft mode with those values. No model touch — the
    // model only changes when the user later clicks "Add Note"
    // (the commit gesture), which routes to
    // onDockNoteCommitNewRequested.
    constexpr std::int64_t kDefaultHalfDurationMs = 200;
    constexpr int          kDefaultMidi           = 69;
    std::int64_t startMs;
    std::int64_t endMs;
    const char*  source;
    if (armedLoopId_.has_value() && loopModel_) {
        const auto idx = loopModel_->indexOf(*armedLoopId_);
        if (idx) {
            const auto& l = loopModel_->loops()[*idx];
            startMs = l.startMs;
            endMs   = l.endMs;
            source  = "armed-loop";
        } else {
            startMs = 0;
            endMs   = kDefaultHalfDurationMs * 2;
            source  = "loop-stale";
        }
    } else {
        const std::int64_t pos = player_ ? player_->position().count()
                                         : std::int64_t{0};
        startMs = std::max<std::int64_t>(0, pos - kDefaultHalfDurationMs);
        endMs   = startMs + kDefaultHalfDurationMs * 2;
        source  = "playback-pos";
    }
    FLOG_DEBUG("ui.score",
               "enter-note-draft via={} start={} end={} midi={}",
               source, startMs, endMs, kDefaultMidi);
    if (projectViewerDock_) {
        projectViewerDock_->enterNoteDraftMode(startMs, endMs, kDefaultMidi);
    }
}

void MainWindow::onDockNoteCommitNewRequested(std::int64_t startMs,
                                              std::int64_t endMs,
                                              int          midi) {
    const auto id = noteModel_->add(startMs, endMs, midi);
    if (id == 0) {
        FLOG_WARN("ui.score",
                  "commit-new-note rejected start={} end={} midi={}",
                  startMs, endMs, midi);
        return;
    }
    pushUndoEntry(undo::AddNote{id});
    FLOG_DEBUG("ui.score",
               "commit-new-note id={} start={} end={} midi={} size={}",
               id, startMs, endMs, midi, noteModel_->size());
}

void MainWindow::onStaffPlaceNoteRequested(std::int64_t ms, int midi) {
    // Step 6.2 piano-roll click-to-place. The interval defaults to
    // the armed loop's range if one is armed (chord-building in a
    // region), otherwise [ms − 200, ms + 200] centred on the click.
    constexpr std::int64_t kDefaultHalfDurationMs = 200;
    std::int64_t startMs;
    std::int64_t endMs;
    const char*  source;
    if (armedLoopId_.has_value() && loopModel_) {
        const auto idx = loopModel_->indexOf(*armedLoopId_);
        if (idx) {
            const auto& l = loopModel_->loops()[*idx];
            startMs = l.startMs;
            endMs   = l.endMs;
            source  = "armed-loop";
        } else {
            startMs = std::max<std::int64_t>(0, ms - kDefaultHalfDurationMs);
            endMs   = startMs + kDefaultHalfDurationMs * 2;
            source  = "loop-stale";
        }
    } else {
        startMs = std::max<std::int64_t>(0, ms - kDefaultHalfDurationMs);
        endMs   = startMs + kDefaultHalfDurationMs * 2;
        source  = "staff-click";
    }
    // MEMO[#step6.2]: avoid the "phantom snap" trap. The hit-test in
    // StaffWidget is pixel-exact, so a click slightly outside an
    // existing bar falls through here. But the default 200 ms half-
    // width is in source-time, so at deep zoom-in the new note's
    // interval still overlaps the existing bar — the freshly-placed
    // bar then sits visually on top of the existing one, and the
    // user perceives it as "the bar got selected when I clicked
    // near it." Detect that case and select the existing note
    // instead of stacking a second bar on the same row.
    for (const auto& n : noteModel_->notes()) {
        if (n.midi != midi) continue;
        if (n.endMs <= startMs || n.startMs >= endMs) continue;
        if (staff_) staff_->setSelectedNoteId(n.id);
        FLOG_DEBUG("ui.score",
                   "place-note coerced-to-select id={} reason=overlap "
                   "click-ms={} midi={} existing=[{}..{}] proposed=[{}..{}]",
                   n.id, ms, midi, n.startMs, n.endMs, startMs, endMs);
        return;
    }
    const auto id = noteModel_->add(startMs, endMs, midi);
    if (id == 0) {
        FLOG_WARN("ui.score",
                  "place-note rejected ms={} midi={} via={} start={} end={}",
                  ms, midi, source, startMs, endMs);
        return;
    }
    pushUndoEntry(undo::AddNote{id});
    // Select the new note so the dock's property page shows it and
    // the user can refine pitch / interval via Apply Changes.
    if (staff_) staff_->setSelectedNoteId(id);
    FLOG_DEBUG("ui.score",
               "place-note id={} ms={} midi={} via={} start={} end={} size={}",
               id, ms, midi, source, startMs, endMs, noteModel_->size());
}

void MainWindow::onStaffNoteDragCommitted(std::int64_t id,
                                          std::int64_t fromStartMs,
                                          std::int64_t fromEndMs,
                                          int          fromMidi,
                                          std::int64_t toStartMs,
                                          std::int64_t toEndMs,
                                          int          toMidi) {
    // MEMO[#60]: single from→to commit on release. The model was
    // untouched mid-drag (paint uses noteDragGhost_ via the staff's
    // effectiveNoteRange / effectiveNoteMidi helpers), so we apply
    // the gesture's net effect here.
    if (!noteModel_) return;
    if (!noteModel_->indexOf(id).has_value()) {
        FLOG_WARN("ui.score",
                  "note-drag id={} skipped reason=stale-id", id);
        return;
    }
    const bool intervalChanged =
        (fromStartMs != toStartMs) || (fromEndMs != toEndMs);
    const bool pitchChanged = (fromMidi != toMidi);
    if (!intervalChanged && !pitchChanged) {
        // Zero-distance drag (threshold crossed but no net delta).
        return;
    }
    // Overlap guard — same rule as click-to-place (#59): a move /
    // resize that would put this note on top of another existing
    // note on the SAME ROW is rejected; the gesture rolls back to
    // the model's untouched state by simply not writing.
    const int finalMidi = pitchChanged ? toMidi : fromMidi;
    for (const auto& n : noteModel_->notes()) {
        if (n.id == id) continue;
        if (n.midi != finalMidi) continue;
        if (n.endMs <= toStartMs || n.startMs >= toEndMs) continue;
        FLOG_DEBUG("ui.score",
                   "note-drag id={} rejected reason=overlap "
                   "would-collide-with-id={} on-midi={} "
                   "proposed=[{}..{}]",
                   id, n.id, finalMidi, toStartMs, toEndMs);
        return;
    }
    // Push undo entries BEFORE writing, so the snapshots reflect
    // the pre-drag state. Two entries (interval + pitch) when both
    // changed — Ctrl+Z reverses them one at a time. QUndoStack
    // grouping is #44.
    if (pitchChanged) {
        pushUndoEntry(undo::EditNotePitch{id, fromMidi});
    }
    if (intervalChanged) {
        pushUndoEntry(undo::EditNoteInterval{id, fromStartMs, fromEndMs});
    }
    if (intervalChanged) {
        noteModel_->setInterval(id, toStartMs, toEndMs);
    }
    if (pitchChanged) {
        noteModel_->setPitch(id, toMidi);
    }
    FLOG_DEBUG("ui.score",
               "note-drag id={} from=[{}..{}]@{} to=[{}..{}]@{}",
               id, fromStartMs, fromEndMs, fromMidi,
               toStartMs, toEndMs, toMidi);
}

void MainWindow::onStaffNoteCreateCommitted(std::int64_t startMs,
                                            std::int64_t endMs,
                                            int          midi) {
    // MEMO[#60]: drag-to-create on the empty grid. Same overlap
    // guard as click-to-place — a drag that would land on top of
    // an existing same-row note selects the existing one instead.
    if (!noteModel_) return;
    if (endMs <= startMs) {
        FLOG_WARN("ui.score",
                  "note-create-drag rejected reason=degenerate "
                  "start={} end={}",
                  startMs, endMs);
        return;
    }
    for (const auto& n : noteModel_->notes()) {
        if (n.midi != midi) continue;
        if (n.endMs <= startMs || n.startMs >= endMs) continue;
        if (staff_) staff_->setSelectedNoteId(n.id);
        FLOG_DEBUG("ui.score",
                   "note-create-drag coerced-to-select id={} reason=overlap "
                   "midi={} existing=[{}..{}] proposed=[{}..{}]",
                   n.id, midi, n.startMs, n.endMs, startMs, endMs);
        return;
    }
    const auto id = noteModel_->add(startMs, endMs, midi);
    if (id == 0) {
        FLOG_WARN("ui.score",
                  "note-create-drag rejected start={} end={} midi={}",
                  startMs, endMs, midi);
        return;
    }
    pushUndoEntry(undo::AddNote{id});
    if (staff_) staff_->setSelectedNoteId(id);
    FLOG_DEBUG("ui.score",
               "note-create-drag id={} start={} end={} midi={} size={}",
               id, startMs, endMs, midi, noteModel_->size());
}

void MainWindow::onDockNoteCommitChangesRequested(std::int64_t id,
                                                  std::int64_t startMs,
                                                  std::int64_t endMs,
                                                  int          midi) {
    const auto idx = noteModel_->indexOf(id);
    if (!idx) {
        FLOG_WARN("ui.score",
                  "commit-note-changes id={} skipped reason=stale-id", id);
        return;
    }
    const auto& n = noteModel_->notes()[*idx];
    const bool intervalChanged = (n.startMs != startMs || n.endMs != endMs);
    const bool pitchChanged    = (n.midi    != midi);
    if (!intervalChanged && !pitchChanged) {
        FLOG_DEBUG("ui.score",
                   "commit-note-changes id={} no-op (buffer == model)", id);
        return;
    }
    // Push one undo entry per changed field so each can be peeled
    // back independently. Order: interval first, then pitch — this
    // matches the order the user typically edits.
    if (intervalChanged) {
        pushUndoEntry(undo::EditNoteInterval{id, n.startMs, n.endMs});
        noteModel_->setInterval(id, startMs, endMs);
        FLOG_DEBUG("ui.score",
                   "commit-note-changes id={} interval from=[{},{}] to=[{},{}]",
                   id, n.startMs, n.endMs, startMs, endMs);
    }
    if (pitchChanged) {
        // Read prevMidi from the model AGAIN — setInterval above may
        // have reordered or otherwise touched the entry.
        const auto idx2 = noteModel_->indexOf(id);
        const int prevMidi = idx2 ? noteModel_->notes()[*idx2].midi : n.midi;
        pushUndoEntry(undo::EditNotePitch{id, prevMidi});
        noteModel_->setPitch(id, midi);
        FLOG_DEBUG("ui.score",
                   "commit-note-changes id={} pitch from={} to={}",
                   id, prevMidi, midi);
    }
}

void MainWindow::onWaveformNoteSelectionChanged(
    std::optional<std::int64_t> id)
{
    // Waveform's plain-seek branch emits this unconditionally with
    // id=nullopt so an empty-space click on the waveform clears the
    // staff's note selection (and via the staff's mirror, the
    // dock's). Without this, the dock's note property page would
    // survive the click and the user would be "stuck" editing a
    // note they've already moved past — the exact stack-on-previous
    // trap. See ScoreOverlayBase::mousePressEvent's plain-seek
    // branch.
    FLOG_DEBUG("ui.score",
               "note-selection mirror from=waveform to=staff id={}",
               id.value_or(-1));
    if (staff_) staff_->setSelectedNoteId(id);
}

void MainWindow::onStaffNoteSelectionChanged(
    std::optional<std::int64_t> id)
{
    // Mirror staff → dock. (Waveform doesn't paint notes in 6.1.)
    FLOG_DEBUG("ui.score",
               "note-selection mirror from=staff to=dock id={}",
               id.value_or(-1));
    if (projectViewerDock_) projectViewerDock_->setSelectedNoteId(id);
}

void MainWindow::onDockNoteSelectionChanged(
    std::optional<std::int64_t> id)
{
    FLOG_DEBUG("ui.score",
               "note-selection mirror from=dock to=staff id={}",
               id.value_or(-1));
    if (staff_) staff_->setSelectedNoteId(id);
}

void MainWindow::onDragStarted() {
    // MEMO[#40]: pre-#40 we stopped the 50 ms position poll here
    // because Player::seek() blocked the GUI for ~140 ms on the
    // shared mutex, and the poll firing during a drag stole mouse
    // events. With the lock-free pipeline seeks are microseconds —
    // the poll is free to keep running, and we *need* it running
    // so the red playback cursor follows the drag. The flag is
    // still observable (tests, future drag-aware logic).
    dragInFlight_ = true;
}

void MainWindow::onDragEnded() {
    dragInFlight_ = false;
}

void MainWindow::onMarkerDragRequested(std::int64_t id,
                                       std::int64_t newMs) {
    // Live drag forwarding (#22). Pre-#22 this slot wrote into
    // the model on every mouse-move, which re-sorted the markers
    // vector and rebuilt the dock tree per move — heavy enough
    // to starve the GUI thread under typical drag speeds. Now
    // the widget paints from a local DragGhost; this slot just
    // mirrors the ghost to the sister score widget so the tick
    // glides on both surfaces. The single model write happens
    // on release via markerDragCommitted.
    if (waveform_) waveform_->setMarkerDragGhost(id, newMs);
    if (staff_)    staff_->setMarkerDragGhost(id, newMs);
}

void MainWindow::onLoopDragRequested(std::int64_t id,
                                     std::int64_t newStartMs,
                                     std::int64_t newEndMs) {
    // Live drag forwarding (#22). Mirror the dragged edge(s) to
    // both widgets so the band glides on both. The source widget
    // already updated its own ghost in mouseMove; the
    // setLoopDragGhost / setLoopMoveDragGhost call here is
    // idempotent on it. The single setRange happens on release.
    if (!loopModel_) return;
    const auto idx = loopModel_->indexOf(id);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    const bool startChanged = (newStartMs != l.startMs);
    const bool endChanged   = (newEndMs   != l.endMs);
    if (startChanged && endChanged) {
        // BOTH edges differ → LoopMove (issue #62). Mirror via the
        // dedicated slot that carries both endpoints.
        if (waveform_) waveform_->setLoopMoveDragGhost(id, newStartMs, newEndMs);
        if (staff_)    staff_->setLoopMoveDragGhost(id, newStartMs, newEndMs);
        return;
    }
    // One edge differs → it's an edge drag (#11). Use the
    // edge-mirror slot.
    const bool isStart = startChanged;
    const std::int64_t ghostMs = isStart ? newStartMs : newEndMs;
    if (waveform_) waveform_->setLoopDragGhost(id, isStart, ghostMs);
    if (staff_)    staff_->setLoopDragGhost(id, isStart, ghostMs);
}

void MainWindow::onDeleteSelectedArtifact() {
    // Window-level Del shortcut. Selection is mutually exclusive
    // across barlines / markers / loops, so we check each in turn
    // and dispatch to whichever has a value. With no selection at
    // all this is a quiet no-op.
    //
    // MEMO: route through onBarlineDeleteRequested /
    // onMarkerDeleteRequested / onLoopDeleteRequested rather than
    // calling models directly, so the undo snapshot lives in one
    // place per kind. The widget-key Del paths land on those same
    // slots; this is just a different gesture entry point.
    if (!waveform_) return;
    if (const auto bar = waveform_->selectedBarline()) {
        const auto idx = *bar;
        onBarlineDeleteRequested(idx);
        FLOG_DEBUG("ui.score",
                   "delete index={} via=window-shortcut size={}",
                   idx, barlineModel_->size());
    } else if (const auto mid = waveform_->selectedMarkerId()) {
        const auto id = *mid;
        onMarkerDeleteRequested(id);
        FLOG_DEBUG("ui.score",
                   "delete-marker id={} via=window-shortcut size={}",
                   id, markerModel_->size());
    } else if (const auto lid = waveform_->selectedLoopId()) {
        const auto id = *lid;
        onLoopDeleteRequested(id);
        FLOG_DEBUG("ui.score",
                   "delete-loop id={} via=window-shortcut size={}",
                   id, loopModel_->size());
    } else if (staff_) {
        // MEMO: notes are not painted on the waveform in 6.1, so
        // the note-selection slot only ever lives on the staff. The
        // mirror plumbing above keeps barline / marker / loop
        // selection on the waveform in sync, but note selection
        // intentionally doesn't reach it. Read from the staff here.
        if (const auto nid = staff_->selectedNoteId()) {
            const auto id = *nid;
            onNoteDeleteRequested(id);
            FLOG_DEBUG("ui.score",
                       "delete-note id={} via=window-shortcut size={}",
                       id, noteModel_->size());
        } else {
            FLOG_DEBUG("ui.score",
                       "delete via=window-shortcut no-selection");
        }
    } else {
        FLOG_DEBUG("ui.score", "delete via=window-shortcut no-selection");
    }
}

void MainWindow::updatePosition() {
    if (!player_) return;
    const auto pos = player_->position();
    {
        QSignalBlocker block(positionSlider_);
        positionSlider_->setValue(static_cast<int>(pos.count()));
    }
    if (waveform_) {
        waveform_->setPositionMs(pos.count());
    }
    if (staff_) {
        staff_->setPositionMs(pos.count());
    }

    // Follow Playback (#49) — page-flip the viewport when the
    // playhead has wandered outside it. Fires only when the
    // playhead actually MOVED since the previous tick; without
    // that guard, a Ctrl+wheel zoom that pushed the viewport away
    // from a stationary playhead would be undone on the very next
    // 50 ms tick (the flip would yank the view back to the
    // playhead, which is the bug the user spotted in smoke).
    // Cheap: an int compare + at most one applyViewport call per
    // tick.
    const auto curMsForFollow = pos.count();
    const bool playheadMoved  = (curMsForFollow != lastFollowPosMs_);
    lastFollowPosMs_ = curMsForFollow;

    if (followPlayback_ && waveform_ && waveform_->isZoomed()
        && playheadMoved)
    {
        const auto curMs   = curMsForFollow;
        const auto vStart  = waveform_->viewportStartMs();
        const auto vEnd    = waveform_->viewportEndMs();
        const auto span    = vEnd - vStart;
        const auto dur     = waveform_->durationMs();
        if (span > 0 && (curMs < vStart || curMs >= vEnd)) {
            // MEMO[#49 smoke]: land the playhead 20 % from the
            // LEFT edge of the new viewport — not flush against
            // it. Flush-at-left worked for natural playback
            // page-flips but turned out unfriendly when the jump
            // was triggered by a double-click on a dock entry
            // for an off-screen artifact: the artifact tick
            // appeared right at the edge and read as "still off
            // screen". 20 % gives a clear lead-in (the marker is
            // visibly INSIDE the viewport) without sacrificing
            // ahead-context for the natural-playback case.
            constexpr double kLeadInFraction = 0.20;
            const auto leadIn =
                static_cast<std::int64_t>(span * kLeadInFraction);
            std::int64_t newStart = curMs - leadIn;
            std::int64_t newEnd   = newStart + span;
            if (newStart < 0) {
                newStart = 0;
                newEnd   = span;
            }
            if (newEnd > dur) {
                newEnd   = dur;
                newStart = std::max<std::int64_t>(0, dur - span);
            }
            applyViewport(newStart, newEnd);
        }
    }

    // Loop wrap-around — armed and we've crossed the loop's endMs.
    // MEMO: GUI-poll-driven (50 ms timer) per the design discussion;
    // wrap jitter of up to ~50 ms is inaudible during practice and
    // simpler than threading the loop region through the audio
    // callback. wrapPending_ blocks re-entry while a pause-between-
    // repeats single-shot timer is in flight.
    //
    // MEMO: only wraps on a NATURAL forward crossing (see
    // wrapShouldFire). User-initiated seeks past endMs are honored
    // as-is — they're "navigate away from the loop", not "skip the
    // current iteration". See issue #13 for the asymmetric-seek
    // bug this rule fixes.
    if (armedLoopId_.has_value() && !wrapPending_ && loopModel_
        && player_->state() == audio::TransportState::Playing)
    {
        const auto idx = loopModel_->indexOf(*armedLoopId_);
        if (idx) {
            const auto& loop = loopModel_->loops()[*idx];
            if (wrapShouldFire(previousPosMs_, pos.count(), loop.endMs)) {
                const auto loopId  = *armedLoopId_;
                const auto startMs = loop.startMs;
                // MEMO: pause-between-repeats now uses the global
                // effective pre-roll (issue #16). Per-loop pauseMs
                // is gone — hand-to-violin time is a property of
                // the player, not the music. When the checkbox is
                // off the effective value is 0 → tight wrap.
                const auto wrapMs  = prerollMs();

                if (wrapMs <= 0) {
                    // Tight wrap: seek back to startMs without
                    // pausing. Keep the player in Playing state.
                    player_->seek(std::chrono::milliseconds{startMs});
                    FLOG_DEBUG("ui.transport",
                               "loop-wrap id={} from={} to={} preroll=0",
                               loopId, pos.count(), startMs);
                } else {
                    wrapPending_       = true;
                    wrapTargetLoopId_  = loopId;   // assigns into optional
                    player_->pause();
                    // Seek now so the visible position slides back
                    // to startMs immediately; the user sees the
                    // pause as silence at the loop's beginning, not
                    // dead air at the end.
                    player_->seek(std::chrono::milliseconds{startMs});
                    // MEMO: deliberately DO NOT change the play
                    // button label here, even though the player is
                    // technically paused. During the wrap-pause
                    // we're conceptually still in playback mode —
                    // a temporary silence between repeats — so the
                    // button stays "Pause". Clicking it during the
                    // pause is the user's "cancel the auto-resume,
                    // stay paused for real" gesture (handled in
                    // onPlayPause). See issue #13.
                    FLOG_DEBUG("ui.transport",
                               "loop-wrap id={} from={} to={} preroll={}",
                               loopId, pos.count(), startMs, wrapMs);
                    if (projectViewerDock_) {
                        projectViewerDock_->startCountdown(wrapMs);
                    }
                    wrapTimer_->start(wrapMs);
                }
                // Don't fall through to the auto-pause-at-end logic
                // below — the loop is what governs the transport now.
                // Update previousPosMs_ before returning so the next
                // tick has a sane reference point.
                previousPosMs_ = pos.count();
                return;
            }
        }
    }

    // Auto-pause when we reach the end (no armed loop).
    if (player_->state() == audio::TransportState::Playing
        && player_->duration().count() > 0
        && pos >= player_->duration()) {
        player_->pause();
        playButton_->setText(tr("Play"));
        FLOG_DEBUG("ui.transport", "auto-pause at-end ms={}", pos.count());
    }

    // Track the position so the wrap rule on the NEXT tick can
    // distinguish a natural forward crossing of endMs (wrap fires)
    // from a user-initiated seek past endMs (no wrap — user is
    // navigating away). See issue #13 + wrapShouldFire.
    previousPosMs_ = pos.count();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Close-with-dirty prompt (#26). Skip in the offscreen Qt
    // platform used by the test suite — there's no user to
    // click the modal, so a dirty-on-close would hang ctest.
    // Production runs the platform plugin Qt picked for the
    // user's desktop (xcb/wayland/cocoa/windows).
    const bool isOffscreen =
        qEnvironmentVariable("QT_QPA_PLATFORM")
            .compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0;
    if (dirty_ && !suppressCloseDirtyPrompt_ && !isOffscreen) {
        const auto pick = QMessageBox::question(
            this, tr("Unsaved changes"),
            tr("This project has unsaved changes. Save before "
               "closing?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (pick == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (pick == QMessageBox::Save) {
            // onSave routes to Save As if no path is bound. If the
            // user cancels the Save As dialog, the project is still
            // dirty — abort the close so they can decide again.
            onSave();
            if (dirty_) {
                event->ignore();
                return;
            }
        }
        // Discard falls through to the normal close path.
    }
    FLOG_DEBUG("ui.file", "close");
    saveLayout();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Force window-level Ctrl+Z to win even when focus is in a
    // QSpinBox / QLineEdit. By default Qt's text-edit widgets
    // accept Qt::ShortcutOverride for the standard editing
    // shortcuts (cut/copy/paste/undo/redo) so they can run their
    // own per-character text-undo. For Fiddler the document-level
    // undo (placement, drag, edit, rename, delete) is what the
    // user means by Ctrl+Z; per-character text-undo inside a
    // dock spinbox is a niche behaviour that just hides the
    // gesture. Reject the override here so the keypress falls
    // through to QShortcut.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->matches(QKeySequence::Undo)) {
            event->ignore();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::saveLayout() const {
    // MEMO: keys are namespaced under "mainwindow/" so future
    // panels / tool windows can share the same QSettings file
    // without colliding. Geometry encodes window position +
    // size; state encodes dock layout (visibility, floating,
    // tabbed grouping, sizes). The pre-roll setting (issue #16)
    // rides along here too — it's a session-level UX preference
    // that benefits from the same survive-relaunch contract.
    QSettings settings;
    settings.setValue("mainwindow/geometry",       saveGeometry());
    settings.setValue("mainwindow/state",          saveState());
    settings.setValue("mainwindow/prerollMs",      prerollMs_);
    settings.setValue("mainwindow/prerollEnabled", prerollEnabled_);
}

void MainWindow::restoreLayout() {
    QSettings settings;
    const auto geometry = settings.value("mainwindow/geometry").toByteArray();
    const auto state    = settings.value("mainwindow/state").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    if (!state.isEmpty())    restoreState(state);
    // Restore pre-roll if a previous session saved it; otherwise
    // keep the default 500 ms. Clamp into the spinbox's range
    // so a corrupted settings value can't push us into a bad UI
    // state.
    if (settings.contains("mainwindow/prerollMs")) {
        const int saved = settings.value("mainwindow/prerollMs",
                                         prerollMs_).toInt();
        prerollMs_ = std::clamp(saved, 0, 5000);
        if (prerollBox_) prerollBox_->setValue(prerollMs_);
    }
    if (settings.contains("mainwindow/prerollEnabled")) {
        const bool saved =
            settings.value("mainwindow/prerollEnabled", prerollEnabled_)
                .toBool();
        // Use setPrerollEnabled to push the value through the same
        // sync paths (checkbox state, spinbox enable, dock
        // visibility) used by the user-driven toggle.
        setPrerollEnabled(saved);
    }
}

void MainWindow::setPrerollMs(int ms) {
    const int clamped = std::clamp(ms, 0, 5000);
    if (prerollMs_ == clamped) return;
    prerollMs_ = clamped;
    if (prerollBox_ && prerollBox_->value() != clamped) {
        QSignalBlocker block(prerollBox_);
        prerollBox_->setValue(clamped);
    }
    // Persist immediately so a crash or force-quit doesn't lose
    // the user's choice. saveLayout would also catch it on close,
    // but redundancy is cheap and avoids surprises.
    QSettings settings;
    settings.setValue("mainwindow/prerollMs", prerollMs_);
    FLOG_DEBUG("ui.transport", "preroll set ms={}", prerollMs_);
}

void MainWindow::setPrerollEnabled(bool enabled) {
    if (prerollEnabled_ == enabled) return;
    prerollEnabled_ = enabled;
    // Sync the checkbox so a programmatic call (restoreLayout, test
    // seam) reflects in the UI without bouncing through the toggled
    // signal.
    if (prerollEnabledBox_ && prerollEnabledBox_->isChecked() != enabled) {
        QSignalBlocker block(prerollEnabledBox_);
        prerollEnabledBox_->setChecked(enabled);
    }
    // Spinbox is only meaningful when pre-roll is enabled — grey
    // it out otherwise so the user understands at a glance that
    // editing has no current effect (but keeps the value).
    if (prerollBox_) prerollBox_->setEnabled(enabled);
    // Show / hide the global countdown widget at the bottom of
    // the dock.
    if (projectViewerDock_) {
        projectViewerDock_->setPrerollEnabled(enabled);
    }
    // If pre-roll just got disabled mid-pause, cancel any active
    // wrap-pause so the player isn't stuck silent waiting for a
    // countdown that's now hidden. The auto-resume timer would
    // still fire, but the user's mental model is "pre-roll is off,
    // nothing should be silent".
    if (!enabled) {
        cancelPendingWrap();
    }
    QSettings settings;
    settings.setValue("mainwindow/prerollEnabled", prerollEnabled_);
    FLOG_DEBUG("ui.transport", "preroll enabled={}", prerollEnabled_);
}

void MainWindow::onPrerollChanged() {
    // editingFinished — the spinbox just committed a new value.
    // Push the previous value onto the undo history so Ctrl+Z
    // reverses the edit. Skip when applyingUndo_ (so the dispatch's
    // own setPrerollMs doesn't push) or when the value didn't
    // actually change (no-op edits aren't worth a history slot).
    if (!prerollBox_) return;
    const int newMs = prerollBox_->value();
    if (newMs == prerollMs_) return;
    pushUndoEntry(undo::EditPrerollMs{prerollMs_});
    setPrerollMs(newMs);
}

void MainWindow::onPrerollEnabledToggled(bool enabled) {
    if (enabled == prerollEnabled_) return;
    pushUndoEntry(undo::EditPrerollEnabled{prerollEnabled_});
    setPrerollEnabled(enabled);
}

// ---- project save / load (#26) -----------------------------------------

void MainWindow::recomputeDirty() {
    // MEMO[#26 smoke]: pre-fix dirty was set sticky-true on the
    // first edit and only cleared on save. The user reported that
    // undoing back to the saved state didn't clear the asterisk —
    // surprising, because their mental model is "* = diverges from
    // saved state". The disjunction below restores that invariant:
    // dirty is true when either the undo history has more (or
    // fewer) entries than at save time, or a non-undoable change
    // (tune-type pick today) has happened since save.
    const bool isDirty =
        (undoHistory_.size() != savedUndoSize_) || nonUndoableDirty_;
    if (isDirty == dirty_) return;
    dirty_ = isDirty;
    updateWindowTitle();
}

void MainWindow::markClean() {
    // Snapshot the current state as the new saved baseline.
    // Subsequent recomputes return false until a real change
    // diverges from this snapshot.
    savedUndoSize_     = undoHistory_.size();
    nonUndoableDirty_  = false;
    recomputeDirty();
}

void MainWindow::pushUndoEntry(undo::Entry entry) {
    if (applyingUndo_) return;
    undoHistory_.push_back(std::move(entry));
    recomputeDirty();
}

void MainWindow::updateWindowTitle() {
    // Three states. No project bound: bare "Fiddler". Bound +
    // clean: "Fiddler — basename". Bound + dirty: prepend "* ".
    if (projectPath_.isEmpty()) {
        setWindowTitle(dirty_ ? tr("* Fiddler") : tr("Fiddler"));
        return;
    }
    const QString base = QFileInfo(projectPath_).fileName();
    setWindowTitle(dirty_
                   ? tr("* Fiddler — %1").arg(base)
                   : tr("Fiddler — %1").arg(base));
}

void MainWindow::onSave() {
    if (projectPath_.isEmpty()) {
        onSaveAs();
        return;
    }
    saveProject(projectPath_);
}

void MainWindow::onSaveAs() {
    // Default suggestion: <audio-without-ext>.fdlp next to the
    // audio file. Mirrors the Logic/Ableton convention of
    // sidecar projects with their own extension.
    QString suggested;
    if (!projectPath_.isEmpty()) {
        suggested = projectPath_;
    } else if (!currentAudioPath_.isEmpty()) {
        const QFileInfo info(currentAudioPath_);
        suggested = info.absolutePath() + "/" + info.completeBaseName() + ".fdlp";
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Fiddler project"),
        suggested,
        tr("Fiddler project (*.fdlp)"));
    if (path.isEmpty()) return;
    saveProject(path);
}

bool MainWindow::saveProject(const QString& path) {
    QJsonObject root;
    root["version"]       = score::kProjectFormatVersion;
    root["audioPath"]     = currentAudioPath_;
    root["timeSignature"] = score::toJson(barlineModel_->timeSignature());
    QJsonObject preroll;
    preroll["enabled"] = prerollEnabled_;
    preroll["ms"]      = prerollMs_;
    root["preroll"]   = preroll;
    root["barlines"]  = score::toJson(*barlineModel_);
    root["markers"]   = score::toJson(*markerModel_);
    root["loops"]     = score::toJson(*loopModel_);
    root["notes"]     = score::toJson(*noteModel_);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        showWarning(this, tr("Save failed"),
            tr("Could not open %1 for writing:\n%2")
                .arg(path).arg(f.errorString()));
        FLOG_WARN("ui.file", "save: failed path={} reason={}",
                  path.toStdString(), f.errorString().toStdString());
        return false;
    }
    const QJsonDocument doc(root);
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();

    projectPath_ = path;
    markClean();
    updateWindowTitle();
    FLOG_DEBUG("ui.file",
               "save: path={} barlines={} markers={} loops={} notes={}",
               path.toStdString(),
               barlineModel_->size(),
               markerModel_->size(),
               loopModel_->size(),
               noteModel_->size());
    return true;
}

bool MainWindow::openProject(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        showWarning(this, tr("Open failed"),
            tr("Could not open %1:\n%2")
                .arg(path).arg(f.errorString()));
        FLOG_WARN("ui.file", "open-project: failed path={} reason={}",
                  path.toStdString(), f.errorString().toStdString());
        return false;
    }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (doc.isNull() || !doc.isObject()) {
        showWarning(this, tr("Open failed"),
            tr("%1 is not a valid Fiddler project (line %2: %3)")
                .arg(path).arg(err.offset).arg(err.errorString()));
        FLOG_WARN("ui.file", "open-project: malformed path={} reason={}",
                  path.toStdString(), err.errorString().toStdString());
        return false;
    }
    const auto root = doc.object();
    const int ver = root.value("version").toInt(0);
    if (ver < 1 || ver > score::kProjectFormatVersion) {
        showWarning(this, tr("Open failed"),
            tr("%1 was saved by a newer Fiddler "
               "(format version %2). Please upgrade.")
                .arg(path).arg(ver));
        return false;
    }

    // Load the audio file first — loadFile clears the models, so
    // anything we put in them BEFORE loadFile would be wiped out.
    const QString audioPath = root.value("audioPath").toString();
    if (!audioPath.isEmpty() && !loadFile(audioPath)) {
        showWarning(this, tr("Audio missing"),
            tr("The audio file referenced by this project "
               "could not be opened:\n%1\n\n"
               "The project's annotations have been loaded; "
               "playback is unavailable.").arg(audioPath));
        // Continue — partial open is more useful than failure.
        // Models are clean (loadFile cleared them) and we'll
        // populate them next.
    }

    // Populate models from JSON.
    if (!score::loadFrom(*barlineModel_,
                         root.value("barlines").toArray())
        || !score::loadFrom(*markerModel_,
                            root.value("markers").toArray())
        || !score::loadFrom(*loopModel_,
                            root.value("loops").toArray())
        || !score::loadFrom(*noteModel_,
                            root.value("notes").toArray())) {
        showWarning(this, tr("Open failed"),
            tr("%1 contains malformed entries; "
               "loading was aborted.").arg(path));
        barlineModel_->clear();
        markerModel_->clear();
        loopModel_->clear();
        noteModel_->clear();
        return false;
    }
    // Time signature + pre-roll.
    barlineModel_->setTimeSignature(
        score::timeSignatureFromJson(
            root.value("timeSignature").toObject()));
    const auto preroll = root.value("preroll").toObject();
    if (preroll.value("ms").isDouble()) {
        setPrerollMs(preroll.value("ms").toInt());
    }
    if (preroll.value("enabled").isBool()) {
        setPrerollEnabled(preroll.value("enabled").toBool());
    }

    // Drop the undo history — Ctrl+Z doesn't reach across a project
    // load. Matches the "fresh start" mental model.
    undoHistory_.clear();

    projectPath_ = path;
    markClean();
    updateWindowTitle();
    FLOG_DEBUG("ui.file",
               "open-project: path={} audio={} "
               "barlines={} markers={} loops={} notes={}",
               path.toStdString(), audioPath.toStdString(),
               barlineModel_->size(),
               markerModel_->size(),
               loopModel_->size(),
               noteModel_->size());
    return true;
}

} // namespace fiddler::ui
