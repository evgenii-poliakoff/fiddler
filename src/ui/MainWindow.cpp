#include "ui/MainWindow.h"

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

    // Ctrl+Z: peel the most-recently-placed artifact, regardless
    // of kind. Dispatched via the combined placementHistory_ LIFO.
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    undoShortcut_->setContext(Qt::WindowShortcut);
    connect(undoShortcut_, &QShortcut::activated,
            this, &MainWindow::onUndoLastPlacement);

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
    placementHistory_.clear();

    // Disarm any previously-armed loop. loopModel_->clear() above
    // would have invalidated armedLoopId_ via onLoopModelChanged
    // anyway, but resetting wrapPending_ is the explicit reason for
    // doing it here too.
    armedLoopId_.reset();
    wrapPending_ = false;
    if (projectViewerDock_) projectViewerDock_->setArmedLoopId(std::nullopt);

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
                FLOG_DEBUG("ui.transport",
                           "play-armed-seek id={} to={}",
                           *armedLoopId_, loop.startMs);
            }
        }
    }

    player_->play();
    playButton_->setText(tr("Pause"));
    FLOG_DEBUG("ui.transport",
               "play from={} ms audio={}", pos, player_->hasAudioOutput());
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
    wrapPending_ = false;
    if (wasArmed && projectViewerDock_) {
        projectViewerDock_->setArmedLoopId(std::nullopt);
    }

    FLOG_DEBUG("ui.transport", "stop rewind=0 disarmed={}", wasArmed);
}

void MainWindow::onSeek(int positionMs) {
    // MEMO: this is a primitive called by three different UI sources
    // (waveform-click, staff-click, position-slider drag); the
    // *source* is logged at the call site so each source's log line
    // is distinct ("seek via waveform" vs "seek via slider"). Adding
    // a log here would either duplicate or lose source information.
    if (!player_) return;
    player_->seek(std::chrono::milliseconds{positionMs});
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
    // record it. The model handles sorted insertion + duplicate
    // rejection; both widgets receive `changed()` and repaint.
    if (!player_ || !player_->duration().count()) return;
    const auto pos        = player_->position().count();
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
        // Push to the combined LIFO so Ctrl+Z can peel this back as
        // the most-recent placement, regardless of any markers
        // placed in between.
        placementHistory_.push_back(PlacementKind::Barline);
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
    // for barlines, but markers are auto-named and never reject
    // duplicates (two markers can sit at the same ms with
    // different names — see MarkerModel::add).
    if (!player_ || !player_->duration().count()) return;
    const auto pos = player_->position().count();
    const auto id  = markerModel_->add(pos);

    // Auto-select the newly-placed marker. Same pattern as
    // tap-place barline: makes "tap M, press Del" symmetric.
    if (waveform_) waveform_->setSelectedMarkerId(id);

    placementHistory_.push_back(PlacementKind::Marker);
    FLOG_DEBUG("ui.score",
               "tap-marker ms={} id={} size={}",
               pos, id, markerModel_->size());
}

void MainWindow::onUndoLastPlacement() {
    // Ctrl+Z — peel the most-recently-placed artifact regardless
    // of kind. We may have to skip stale entries: if the user
    // manually deleted an artifact via Del, its placementHistory_
    // entry stays put, so undoLastAdd on the relevant model can
    // return false. In that case we keep peeling until something
    // gets removed or the history drains.
    while (!placementHistory_.empty()) {
        const auto kind = placementHistory_.back();
        placementHistory_.pop_back();
        bool removed = false;
        const char* kindStr = "?";
        switch (kind) {
        case PlacementKind::Barline:
            removed = barlineModel_->undoLastAdd();
            kindStr = "barline";
            break;
        case PlacementKind::Marker:
            removed = markerModel_->undoLastAdd();
            kindStr = "marker";
            break;
        case PlacementKind::Loop:
            removed = loopModel_->undoLastAdd();
            kindStr = "loop";
            break;
        }
        if (removed) {
            FLOG_DEBUG("ui.score",
                       "undo-last kind={} bar-size={} marker-size={} loop-size={}",
                       kindStr,
                       barlineModel_->size(), markerModel_->size(),
                       loopModel_->size());
            return;
        }
        // else: stale entry, keep peeling.
    }
    FLOG_DEBUG("ui.score", "undo-last empty (no-op)");
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
    placementHistory_.push_back(PlacementKind::Loop);

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
    // We seek to the marker's exact source-time, then start
    // playback if the player isn't already running.
    if (!markerModel_ || !player_) return;
    const auto idx = markerModel_->indexOf(id);
    if (!idx) return;
    const auto ms =
        static_cast<int>(markerModel_->markers()[*idx].sourceMs);

    // MEMO: route through onSeek so the existing seek log fires
    // and the position slider updates via the timer chain.
    onSeek(ms);

    if (player_->state() != audio::TransportState::Playing) {
        player_->play();
        playButton_->setText(tr("Pause"));
    }

    FLOG_DEBUG("ui.score",
               "marker-activated id={} ms={} via=dock", id, ms);
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
    wrapPending_ = false;
    if (projectViewerDock_) projectViewerDock_->setArmedLoopId(id);

    onSeek(static_cast<int>(loop.startMs));
    if (player_->state() != audio::TransportState::Playing) {
        player_->play();
        playButton_->setText(tr("Pause"));
    }

    FLOG_DEBUG("ui.score",
               "loop-activated id={} start={} end={} pause={} via=dock",
               id, loop.startMs, loop.endMs, loop.pauseMs);
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
        wrapPending_ = false;
        if (projectViewerDock_) projectViewerDock_->setArmedLoopId(id);
        FLOG_DEBUG("ui.score", "loop-armed id={} via=checkbox", id);
    } else {
        if (armedLoopId_ != id) return;
        armedLoopId_.reset();
        wrapPending_ = false;
        if (projectViewerDock_) projectViewerDock_->setArmedLoopId(std::nullopt);
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
    wrapPending_ = false;
    if (projectViewerDock_) projectViewerDock_->setArmedLoopId(std::nullopt);
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
    // repaint both views.
    barlineModel_->removeAt(index);
    FLOG_DEBUG("ui.score", "delete index={} via=widget-key size={}",
               index, barlineModel_->size());
}

void MainWindow::onMarkerDeleteRequested(std::int64_t id) {
    // Same shape as onBarlineDeleteRequested but keyed by the
    // marker's stable ID. Fires from the score widgets' Del
    // handlers OR the dock's tree-row Del filter (in standalone
    // contexts; in MainWindow context the window-level Del
    // shortcut takes precedence).
    markerModel_->remove(id);
    FLOG_DEBUG("ui.score", "delete-marker id={} via=widget-key size={}",
               id, markerModel_->size());
}

void MainWindow::onLoopDeleteRequested(std::int64_t id) {
    // Fired by the dock when the user presses Del on a loop row.
    // (Score widgets don't yet have a loop-Del handler — loops are
    // dock-driven.)
    loopModel_->remove(id);
    FLOG_DEBUG("ui.score", "delete-loop id={} via=dock-key size={}",
               id, loopModel_->size());
}

void MainWindow::onDeleteSelectedArtifact() {
    // Window-level Del shortcut. Selection is mutually exclusive
    // across barlines / markers / loops, so we check each in turn
    // and dispatch to whichever has a value. With no selection at
    // all this is a quiet no-op.
    if (!waveform_) return;
    if (const auto bar = waveform_->selectedBarline()) {
        const auto idx = *bar;
        barlineModel_->removeAt(idx);
        FLOG_DEBUG("ui.score",
                   "delete index={} via=window-shortcut size={}",
                   idx, barlineModel_->size());
    } else if (const auto mid = waveform_->selectedMarkerId()) {
        const auto id = *mid;
        markerModel_->remove(id);
        FLOG_DEBUG("ui.score",
                   "delete-marker id={} via=window-shortcut size={}",
                   id, markerModel_->size());
    } else if (const auto lid = waveform_->selectedLoopId()) {
        const auto id = *lid;
        loopModel_->remove(id);
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
    if (armedLoopId_.has_value() && !wrapPending_ && loopModel_
        && player_->state() == audio::TransportState::Playing)
    {
        const auto idx = loopModel_->indexOf(*armedLoopId_);
        if (idx) {
            const auto& loop = loopModel_->loops()[*idx];
            if (pos.count() >= loop.endMs) {
                const auto loopId  = *armedLoopId_;
                const auto startMs = loop.startMs;
                const auto pauseMs = loop.pauseMs;

                if (pauseMs <= 0) {
                    // Tight wrap: seek back to startMs without
                    // pausing. Keep the player in Playing state.
                    player_->seek(std::chrono::milliseconds{startMs});
                    FLOG_DEBUG("ui.transport",
                               "loop-wrap id={} from={} to={} pause=0",
                               loopId, pos.count(), startMs);
                } else {
                    wrapPending_ = true;
                    player_->pause();
                    // Seek now so the visible position slides back
                    // to startMs immediately; the user sees the
                    // pause as silence at the loop's beginning, not
                    // dead air at the end.
                    player_->seek(std::chrono::milliseconds{startMs});
                    FLOG_DEBUG("ui.transport",
                               "loop-wrap id={} from={} to={} pause={}",
                               loopId, pos.count(), startMs, pauseMs);
                    QPointer<MainWindow> self(this);
                    QTimer::singleShot(pauseMs, this,
                        [self, loopId, startMs]() {
                            if (!self) return;
                            self->wrapPending_ = false;
                            // The user may have disarmed during the
                            // pause (Stop, Del, file-load); the
                            // checks below cover all those exits.
                            if (self->armedLoopId_ != loopId) return;
                            if (!self->player_) return;
                            self->player_->play();
                            self->playButton_->setText(self->tr("Pause"));
                            (void)startMs;
                        });
                }
                // Don't fall through to the auto-pause-at-end logic
                // below — the loop is what governs the transport now.
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
}

void MainWindow::closeEvent(QCloseEvent* event) {
    FLOG_DEBUG("ui.file", "close");
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::saveLayout() const {
    // MEMO: keys are namespaced under "mainwindow/" so future
    // panels / tool windows can share the same QSettings file
    // without colliding. Geometry encodes window position +
    // size; state encodes dock layout (visibility, floating,
    // tabbed grouping, sizes).
    QSettings settings;
    settings.setValue("mainwindow/geometry", saveGeometry());
    settings.setValue("mainwindow/state",    saveState());
}

void MainWindow::restoreLayout() {
    QSettings settings;
    const auto geometry = settings.value("mainwindow/geometry").toByteArray();
    const auto state    = settings.value("mainwindow/state").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    if (!state.isEmpty())    restoreState(state);
}

} // namespace fiddler::ui
