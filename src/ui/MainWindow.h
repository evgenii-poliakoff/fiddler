// MainWindow — top-level Qt window.
// Step 0: empty shell with File → Open and disabled transport buttons.

#pragma once

#include <QMainWindow>
#include <memory>

class QAction;
class QPushButton;
class QSlider;
class QLabel;
class QTimer;

namespace fiddler::audio { class Player; }

namespace fiddler::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onOpenFile();
    void onPlayPause();
    void onStop();
    void onSeek(int positionMs);
    void updatePosition();

private:
    void buildMenus();
    void buildCentralWidget();

    // Owned audio engine. Forward-declared to keep Qt headers separate
    // from PortAudio/FFmpeg includes.
    std::unique_ptr<audio::Player> player_;

    QAction*     openAction_     = nullptr;
    QPushButton* playButton_     = nullptr;
    QPushButton* stopButton_     = nullptr;
    QSlider*     positionSlider_ = nullptr;
    QLabel*      statusLabel_    = nullptr;
    QTimer*      positionTimer_  = nullptr;
};

} // namespace fiddler::ui
