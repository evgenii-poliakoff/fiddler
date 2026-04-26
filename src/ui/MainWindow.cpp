#include "ui/MainWindow.h"

#include "audio/Player.h"
#include "util/Log.h"

#include <QAction>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace fiddler::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , player_(std::make_unique<audio::Player>()) {
    setWindowTitle("Fiddler");
    buildMenus();
    buildCentralWidget();
    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow() = default;

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

    auto* transport = new QHBoxLayout();
    playButton_ = new QPushButton(tr("Play"), central);
    stopButton_ = new QPushButton(tr("Stop"), central);
    playButton_->setEnabled(false);
    stopButton_->setEnabled(false);
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::onPlayPause);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStop);
    transport->addWidget(playButton_);
    transport->addWidget(stopButton_);
    transport->addStretch();
    layout->addLayout(transport);

    positionSlider_ = new QSlider(Qt::Horizontal, central);
    positionSlider_->setRange(0, 0);
    positionSlider_->setEnabled(false);
    connect(positionSlider_, &QSlider::sliderMoved,
            this, &MainWindow::onSeek);
    layout->addWidget(positionSlider_);

    auto* tempoRow = new QHBoxLayout();
    tempoLabel_ = new QLabel(tr("Tempo: 100%"), central);
    tempoSlider_ = new QSlider(Qt::Horizontal, central);
    tempoSlider_->setRange(25, 100);
    tempoSlider_->setValue(100);
    tempoSlider_->setEnabled(false);
    connect(tempoSlider_, &QSlider::valueChanged,
            this, &MainWindow::onTempoChanged);
    tempoRow->addWidget(tempoLabel_);
    tempoRow->addWidget(tempoSlider_, /*stretch=*/1);
    layout->addLayout(tempoRow);

    layout->addStretch();
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
    if (!player_->load(path.toStdString())) {
        QMessageBox::warning(this, tr("Open failed"),
            tr("Could not open: %1").arg(path));
        statusLabel_->setText(tr("No file loaded."));
        return;
    }

    const auto duration = player_->duration();
    {
        QSignalBlocker block(positionSlider_);
        positionSlider_->setRange(0, static_cast<int>(duration.count()));
        positionSlider_->setValue(0);
    }
    positionSlider_->setEnabled(true);
    playButton_->setEnabled(true);
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

    statusLabel_->setText(tr("Loaded: %1  (%2 ms)")
        .arg(path).arg(duration.count()));

    if (!positionTimer_) {
        positionTimer_ = new QTimer(this);
        positionTimer_->setInterval(50);
        connect(positionTimer_, &QTimer::timeout,
                this, &MainWindow::updatePosition);
    }
    positionTimer_->start();
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

void MainWindow::updatePosition() {
    if (!player_) return;
    const auto pos = player_->position();
    {
        QSignalBlocker block(positionSlider_);
        positionSlider_->setValue(static_cast<int>(pos.count()));
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
