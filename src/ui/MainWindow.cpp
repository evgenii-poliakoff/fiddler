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
#include "ui/ProjectViewerDock.h"
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
#include <QFileDialog>
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
#include <memory>
#include <thread>
#include <utility>

namespace fiddler::ui {

namespace {

// Bucket count is the overview's *internal* peak resolution. The
// widget downsamples to its current pixel width on every paint, so a
// fixed-and-generous 4096 covers any plausible window width without
// rebuilding when the user resizes. 4096 buckets × 2 ch × 8 bytes ≈
// 64 KB — trivial.
constexpr std::size_t kOverviewBuckets = 4096;

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

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , player_(std::make_unique<audio::Player>())
    , barlineModel_(std::make_shared<score::BarlineModel>())
    , markerModel_(std::make_shared<score::MarkerModel>())
    , loopModel_(std::make_shared<score::LoopModel>()) {
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

void MainWindow::buildMenus() {
    openAction_ = new QAction(tr("&Open…"), this);
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &MainWindow::onOpenFile);

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(openAction_);
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
    connect(waveform_, &WaveformWidget::markerDragRequested,
            this, &MainWindow::onMarkerDragRequested);
    connect(waveform_, &WaveformWidget::loopDragRequested,
            this, &MainWindow::onLoopDragRequested);
    connect(waveform_, &WaveformWidget::markerDragCommitted,
            this, [this](std::int64_t id,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                pushMarkerDragCommit(id, fromMs, toMs);
                FLOG_DEBUG("ui.score",
                    "marker-drag id={} from={} to={} via=waveform",
                    id, fromMs, toMs);
            });
    connect(waveform_, &WaveformWidget::loopDragCommitted,
            this, [this](std::int64_t id, bool isStart,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                pushLoopDragCommit(id, isStart, fromMs, toMs);
                FLOG_DEBUG("ui.score",
                    "loop-drag id={} edge={} from={} to={} via=waveform",
                    id, isStart ? "start" : "end", fromMs, toMs);
            });
    layout->addWidget(waveform_, /*stretch=*/1);

    // Staff: the lower view. Fixed-ish height (its sizeHint), shares
    // the same models and source-time axis as the waveform.
    staff_ = new StaffWidget(central);
    staff_->setObjectName("staffWidget");
    staff_->setBarlineModel(barlineModel_);
    staff_->setMarkerModel(markerModel_);
    staff_->setLoopModel(loopModel_);
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
    connect(staff_, &StaffWidget::markerDragRequested,
            this, &MainWindow::onMarkerDragRequested);
    connect(staff_, &StaffWidget::loopDragRequested,
            this, &MainWindow::onLoopDragRequested);
    connect(staff_, &StaffWidget::markerDragCommitted,
            this, [this](std::int64_t id,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                pushMarkerDragCommit(id, fromMs, toMs);
                FLOG_DEBUG("ui.score",
                    "marker-drag id={} from={} to={} via=staff",
                    id, fromMs, toMs);
            });
    connect(staff_, &StaffWidget::loopDragCommitted,
            this, [this](std::int64_t id, bool isStart,
                         std::int64_t fromMs,
                         std::int64_t toMs) {
                pushLoopDragCommit(id, isStart, fromMs, toMs);
                FLOG_DEBUG("ui.score",
                    "loop-drag id={} edge={} from={} to={} via=staff",
                    id, isStart ? "start" : "end", fromMs, toMs);
            });
    layout->addWidget(staff_);

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
    layout->addWidget(positionSlider_);

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
}

void MainWindow::onOpenFile() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open audio file"),
        {},
        tr("Audio files (*.wav *.flac *.mp3 *.ogg *.m4a *.aac *.opus);;All files (*)"));
    if (path.isEmpty()) {
        // MEMO: ui-event log — user dismissed the file dialog.
        // See feedback_logs_drive_tests.md for the contract.
        FLOG_DEBUG("ui.file", "open: cancelled");
        return;
    }

    if (!loadFile(path)) {
        QMessageBox::warning(this, tr("Open failed"),
            tr("Could not open: %1").arg(path));
    }
}

bool MainWindow::loadFile(const QString& path) {
    if (!player_->load(path.toStdString())) {
        FLOG_DEBUG("ui.file", "open: failed path={}", path.toStdString());
        statusLabel_->setText(tr("No file loaded."));
        return false;
    }

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
        if (!applyingUndo_) {
            undoHistory_.emplace_back(undo::AddBarline{pos});
        }
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

    if (!applyingUndo_) {
        undoHistory_.emplace_back(undo::AddMarker{id});
    }
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
        } else if constexpr (std::is_same_v<E, undo::EditMarkerPos>) {
            markerModel_->setPosition(e.id, e.prevSourceMs);
            kind = "edit-marker-pos";
        } else if constexpr (std::is_same_v<E, undo::EditLoopRange>) {
            loopModel_->setRange(e.id, e.prevStartMs, e.prevEndMs);
            kind = "edit-loop-range";
        } else if constexpr (std::is_same_v<E, undo::RenameMarker>) {
            markerModel_->rename(e.id, e.prevName);
            kind = "rename-marker";
        } else if constexpr (std::is_same_v<E, undo::RenameLoop>) {
            loopModel_->rename(e.id, e.prevName);
            kind = "rename-loop";
        } else if constexpr (std::is_same_v<E, undo::DeleteBarline>) {
            barlineModel_->add(e.sourceMs);
            kind = "delete-barline";
        } else if constexpr (std::is_same_v<E, undo::DeleteMarker>) {
            markerModel_->addWithId(e.id, e.sourceMs, e.name);
            kind = "delete-marker";
        } else if constexpr (std::is_same_v<E, undo::DeleteLoop>) {
            loopModel_->addWithId(e.id, e.startMs, e.endMs, e.name);
            kind = "delete-loop";
        } else if constexpr (std::is_same_v<E, undo::EditPrerollMs>) {
            setPrerollMs(e.prevMs);
            kind = "edit-preroll-ms";
        } else if constexpr (std::is_same_v<E, undo::EditPrerollEnabled>) {
            setPrerollEnabled(e.prevEnabled);
            kind = "edit-preroll-enabled";
        }
    }, entry);
    applyingUndo_ = false;

    FLOG_DEBUG("ui.score",
               "undo kind={} bar-size={} marker-size={} loop-size={}",
               kind,
               barlineModel_->size(), markerModel_->size(),
               loopModel_->size());
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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(undo::AddLoop{id});
    }

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
    // marker) also gives the player time to switch from mouse to
    // instrument.
    //
    // MEMO: do NOT call from the wrap timer's lambda (timeout) —
    // that fires AFTER the wrap-pause that was the pre-roll for
    // the next iteration. Calling this would loop forever.
    if (!player_) return;

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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(undo::DeleteBarline{sourceMs});
    }
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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(
            undo::DeleteMarker{id, sourceMs, name});
    }
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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(
            undo::DeleteLoop{id, startMs, endMs, name});
    }
    FLOG_DEBUG("ui.score", "delete-loop id={} via=dock-key size={}",
               id, loopModel_->size());
}

void MainWindow::pushMarkerDragCommit(std::int64_t id,
                                      std::int64_t fromMs,
                                      std::int64_t toMs) {
    // No-op when we're applying an undo. Also bail when the drag
    // was a zero-distance no-op so Ctrl+Z doesn't pop a phantom
    // entry that does nothing observable.
    if (applyingUndo_) return;
    if (fromMs == toMs)  return;
    undoHistory_.emplace_back(undo::EditMarkerPos{id, fromMs});
}

void MainWindow::pushLoopDragCommit(std::int64_t id,
                                    bool         isStart,
                                    std::int64_t fromMs,
                                    std::int64_t toMs) {
    if (applyingUndo_) return;
    if (fromMs == toMs)  return;
    // Reconstruct the full pre-drag range from the post-drag model
    // state and the signal's edge identification. The unchanged
    // edge is whichever the signal *didn't* carry; read it from
    // the model now.
    const auto idx = loopModel_->indexOf(id);
    if (!idx) return;
    const auto& l = loopModel_->loops()[*idx];
    const std::int64_t prevStart = isStart ? fromMs : l.startMs;
    const std::int64_t prevEnd   = isStart ? l.endMs : fromMs;
    undoHistory_.emplace_back(undo::EditLoopRange{id, prevStart, prevEnd});
}

void MainWindow::onDockMarkerRenameRequested(std::int64_t id,
                                              QString      name) {
    const auto idx = markerModel_->indexOf(id);
    if (!idx) return;
    const auto prevName = markerModel_->markers()[*idx].name;
    if (prevName == name) return;   // no-op edit, no history push
    if (!applyingUndo_) {
        undoHistory_.emplace_back(undo::RenameMarker{id, prevName});
    }
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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(undo::EditMarkerPos{id, prevMs});
    }
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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(undo::RenameLoop{id, prevName});
    }
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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(
            undo::EditLoopRange{id, prevStartMs, prevEndMs});
    }
    loopModel_->setRange(id, newStartMs, newEndMs);
    FLOG_DEBUG("ui.score",
               "edit-loop-range id={} from=[{},{}] to=[{},{}] via=dock-spinbox",
               id, prevStartMs, prevEndMs, newStartMs, newEndMs);
}

void MainWindow::onMarkerDragRequested(std::int64_t id,
                                       std::int64_t newMs) {
    // Live drag (issue #11). Fires per mouse-move while the user is
    // dragging a marker tick on either score widget. We just push
    // the new position into the model — every connected view +
    // the dock's property page repaints automatically through the
    // model's changed() signal. No log here: the per-move rate
    // would flood the event log; the widget emits a separate
    // *DragCommitted signal on release that we DO log.
    if (!markerModel_) return;
    markerModel_->setPosition(id, newMs);
}

void MainWindow::onLoopDragRequested(std::int64_t id,
                                     std::int64_t newStartMs,
                                     std::int64_t newEndMs) {
    // Live drag for a loop boundary. setRange validates the
    // start < end invariant and rejects invalid ranges; the widget
    // already filters those out before emitting, but the model is
    // the authoritative gate. No log per move (see
    // onMarkerDragRequested).
    if (!loopModel_) return;
    loopModel_->setRange(id, newStartMs, newEndMs);
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
    if (!applyingUndo_) {
        undoHistory_.emplace_back(undo::EditPrerollMs{prerollMs_});
    }
    setPrerollMs(newMs);
}

void MainWindow::onPrerollEnabledToggled(bool enabled) {
    if (enabled == prerollEnabled_) return;
    if (!applyingUndo_) {
        undoHistory_.emplace_back(
            undo::EditPrerollEnabled{prerollEnabled_});
    }
    setPrerollEnabled(enabled);
}

} // namespace fiddler::ui
