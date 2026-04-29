#include "ui/MainWindow.h"

#include "audio/Decoder.h"
#include "audio/Player.h"
#include "audio/WaveformOverview.h"
#include "score/BarlineModel.h"
#include "ui/StaffWidget.h"
#include "ui/WaveformWidget.h"
#include "util/Log.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
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
    , barlineModel_(std::make_shared<score::BarlineModel>()) {
    setWindowTitle("Fiddler");
    buildMenus();
    buildCentralWidget();
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

    // Ctrl+Z: peel the most-recently-placed barline (LIFO undo).
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    undoShortcut_->setContext(Qt::WindowShortcut);
    connect(undoShortcut_, &QShortcut::activated,
            this, &MainWindow::onUndoLastBarline);
}

MainWindow::~MainWindow() = default;

const score::BarlineModel& MainWindow::barlineModel() const noexcept {
    return *barlineModel_;
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
    connect(waveform_, &WaveformWidget::seekRequested,
            this, [this](std::int64_t ms) {
                onSeek(static_cast<int>(ms));
            });
    connect(waveform_, &WaveformWidget::barlineSelectionChanged,
            this, &MainWindow::onWaveformSelectionChanged);
    connect(waveform_, &WaveformWidget::barlineDeleteRequested,
            this, &MainWindow::onBarlineDeleteRequested);
    layout->addWidget(waveform_, /*stretch=*/1);

    // Staff: the lower view. Fixed-ish height (its sizeHint), shares
    // the same BarlineModel and source-time axis as the waveform.
    staff_ = new StaffWidget(central);
    staff_->setObjectName("staffWidget");
    staff_->setBarlineModel(barlineModel_);
    connect(staff_, &StaffWidget::seekRequested,
            this, [this](std::int64_t ms) {
                onSeek(static_cast<int>(ms));
            });
    connect(staff_, &StaffWidget::barlineSelectionChanged,
            this, &MainWindow::onStaffSelectionChanged);
    connect(staff_, &StaffWidget::barlineDeleteRequested,
            this, &MainWindow::onBarlineDeleteRequested);
    layout->addWidget(staff_);

    positionSlider_ = new QSlider(Qt::Horizontal, central);
    positionSlider_->setObjectName("positionSlider");
    positionSlider_->setRange(0, 0);
    positionSlider_->setEnabled(false);
    connect(positionSlider_, &QSlider::sliderMoved,
            this, &MainWindow::onSeek);
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
}

void MainWindow::onOpenFile() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open audio file"),
        {},
        tr("Audio files (*.wav *.flac *.mp3 *.ogg *.m4a *.aac *.opus);;All files (*)"));
    if (path.isEmpty()) {
        FLOG_DEBUG("ui", "open dialog cancelled");
        return;
    }

    FLOG_DEBUG("ui", "user selected {}", path.toStdString());
    if (!loadFile(path)) {
        QMessageBox::warning(this, tr("Open failed"),
            tr("Could not open: %1").arg(path));
    }
}

bool MainWindow::loadFile(const QString& path) {
    if (!player_->load(path.toStdString())) {
        statusLabel_->setText(tr("No file loaded."));
        return false;
    }

    const auto duration = player_->duration();
    const bool hasAudio = player_->hasAudioOutput();
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

    // A new file means the previous file's barlines are no longer
    // meaningful — drop them. Both widgets are observing the model
    // and will repaint to empty automatically.
    barlineModel_->clear();

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
        player_->pause();
        playButton_->setText(tr("Play"));
    } else {
        player_->play();
        playButton_->setText(tr("Pause"));
    }
}

void MainWindow::onStop() {
    if (!player_) return;
    player_->stop();
    playButton_->setText(tr("Play"));
    QSignalBlocker block(positionSlider_);
    positionSlider_->setValue(0);
}

void MainWindow::onSeek(int positionMs) {
    if (!player_) return;
    player_->seek(std::chrono::milliseconds{positionMs});
}

void MainWindow::onTempoChanged(int percent) {
    if (!player_) return;
    tempoLabel_->setText(tr("Tempo: %1%").arg(percent));
    player_->setTempoRatio(percent / 100.0);
}

// ---- score-related slots ------------------------------------------------

void MainWindow::onTapBarline() {
    // The 'B' key — primary barline-placement gesture. We capture
    // the player's current source-time position and ask the model to
    // record it. The model handles sorted insertion + duplicate
    // rejection; both widgets receive `changed()` and repaint.
    if (!player_ || !player_->duration().count()) return;
    barlineModel_->add(player_->position().count());
}

void MainWindow::onUndoLastBarline() {
    // Ctrl+Z — peel the most-recently-added barline. The model
    // returns false if the LIFO is empty; we just no-op in that case
    // so an extra Ctrl+Z press doesn't do anything surprising.
    barlineModel_->undoLastAdd();
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
}

void MainWindow::onWaveformSelectionChanged(
    std::optional<std::size_t> index)
{
    // Mirror the waveform's selection onto the staff so both views
    // show the same highlight. setSelectedBarline() is a no-op when
    // the value is unchanged, so the round-trip
    // staff -> onStaffSelectionChanged -> waveform
    // doesn't loop forever.
    if (staff_) staff_->setSelectedBarline(index);
}

void MainWindow::onStaffSelectionChanged(
    std::optional<std::size_t> index)
{
    if (waveform_) waveform_->setSelectedBarline(index);
}

void MainWindow::onBarlineDeleteRequested(std::size_t index) {
    // Either widget can fire this (via its Del-key handler). Both
    // route through the same slot — the model is the single source
    // of truth, and its `changed()` signal will repaint both views.
    barlineModel_->removeAt(index);
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
    // Auto-pause when we reach the end.
    if (player_->state() == audio::TransportState::Playing
        && player_->duration().count() > 0
        && pos >= player_->duration()) {
        player_->pause();
        playButton_->setText(tr("Play"));
    }
}

} // namespace fiddler::ui
