// MainWindow — top-level Qt window. Owns the audio engine, drives
// the transport controls, and coordinates the waveform view.

#pragma once

#include <QMainWindow>
#include <atomic>
#include <cstdint>
#include <memory>

class QAction;
class QPushButton;
class QSlider;
class QLabel;
class QString;
class QTimer;

namespace fiddler::audio { class Player; }

namespace fiddler::ui {

class WaveformWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Test seam: drive a file open without going through QFileDialog.
    // The slot onOpenFile() ends up calling this after the dialog
    // returns, so production and tests share the same code path.
    [[nodiscard]] bool loadFile(const QString& path);

    // Read-only access to the audio engine. Used by integration tests
    // to verify that UI events (clicks, slider moves) propagate
    // through to the player.
    [[nodiscard]] const audio::Player& player() const noexcept { return *player_; }

private slots:
    void onOpenFile();
    void onPlayPause();
    void onStop();
    void onSeek(int positionMs);
    void onTempoChanged(int percent);
    void updatePosition();

private:
    void buildMenus();
    void buildCentralWidget();

    // Owned audio engine. Forward-declared so Player.h's PortAudio /
    // FFmpeg fwd-decls don't ripple into Qt-only TUs.
    std::unique_ptr<audio::Player> player_;

    QAction*        openAction_     = nullptr;
    QPushButton*    playButton_     = nullptr;
    QPushButton*    stopButton_     = nullptr;
    QSlider*        positionSlider_ = nullptr;
    QSlider*        tempoSlider_    = nullptr;
    QLabel*         tempoLabel_     = nullptr;
    QLabel*         statusLabel_    = nullptr;
    QTimer*         positionTimer_  = nullptr;
    WaveformWidget* waveform_       = nullptr;

    // Generation counter for async overview builds: rapid loadFile
    // calls invalidate older builds so a slow build for file A can't
    // overwrite a fresh overview for file B if it finishes second.
    // Worker threads are detached; the lambda posted back to the GUI
    // thread checks this counter before installing the result.
    std::atomic<std::uint64_t> overviewGeneration_{0};
};

} // namespace fiddler::ui
