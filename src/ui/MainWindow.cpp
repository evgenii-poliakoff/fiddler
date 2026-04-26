#include "ui/MainWindow.h"

#include "audio/Player.h"

#include <QAction>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QPushButton>
#include <QSlider>
#include <QStatusBar>
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
    fileMenu->addAction(tr("&Quit"), this, &QWidget::close,
                        QKeySequence::Quit);
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

    layout->addStretch();
    setCentralWidget(central);
}

void MainWindow::onOpenFile() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open audio file"),
        {},
        tr("Audio files (*.wav *.flac *.mp3 *.ogg *.m4a *.aac *.opus);;All files (*)"));
    if (path.isEmpty()) return;

    // TODO step 1: hand path to player_->load(path); enable transport on success.
    statusLabel_->setText(tr("Selected: %1 (loading not implemented yet)").arg(path));
}

void MainWindow::onPlayPause() {
    // TODO step 1: toggle player_ transport state.
}

void MainWindow::onStop() {
    // TODO step 1: stop player_ and reset slider.
}

void MainWindow::onSeek(int positionMs) {
    // TODO step 1: player_->seek(std::chrono::milliseconds{positionMs});
    Q_UNUSED(positionMs);
}

} // namespace fiddler::ui
