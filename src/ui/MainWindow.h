// MainWindow — top-level Qt window. Owns the audio engine, drives
// the transport controls, and coordinates the waveform view.

#pragma once

#include <QMainWindow>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class QAction;
class QComboBox;
class QPushButton;
class QSlider;
class QLabel;
class QShortcut;
class QString;
class QTimer;

namespace fiddler::audio { class Player; }
namespace fiddler::score { class BarlineModel; }

namespace fiddler::ui {

class StaffWidget;
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

    // Access to the shared score model. Used by integration tests to
    // verify that tap-to-place / Ctrl+Z / time-sig picker mutations
    // actually reach the model.
    [[nodiscard]] const score::BarlineModel& barlineModel() const noexcept;

private slots:
    void onOpenFile();
    void onPlayPause();
    void onStop();
    void onSeek(int positionMs);
    void onTempoChanged(int percent);
    void updatePosition();

    // The 'B' shortcut: place a barline at the player's current
    // source-time position. Tap-to-place is the primary placement
    // gesture for fiddle transcription (memory/project_tap_to_place.md).
    void onTapBarline();

    // The 'Ctrl+Z' shortcut: peel the most-recently-added barline
    // from the model. A degenerate undo, sufficient for the
    // tap-along workflow (Rule 8 — full QUndoStack deferred).
    void onUndoLastBarline();

    // The tune-type picker: looks up the chosen preset and pushes
    // its TimeSignature into the model.
    void onTuneTypePresetChosen(int comboIndex);

    // Selection mirroring: when one of the two views changes its
    // highlight, push it into the other so both stay in sync.
    void onWaveformSelectionChanged(std::optional<std::size_t> index);
    void onStaffSelectionChanged   (std::optional<std::size_t> index);

    // 'Del' on either widget: turn the requested-by-key signal into
    // an actual model mutation.
    void onBarlineDeleteRequested(std::size_t index);

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
    StaffWidget*    staff_          = nullptr;
    QComboBox*      tuneTypeCombo_  = nullptr;
    QShortcut*      tapBarShortcut_ = nullptr;   // 'B'
    QShortcut*      undoShortcut_   = nullptr;   // Ctrl+Z

    // The shared score model — owned here, observed by both widgets
    // as a const view. shared_ptr (not unique_ptr) so the widgets'
    // shared_ptr<const BarlineModel> handles can extend the
    // lifetime if MainWindow tears down before they do (Qt's
    // parent-child destruction order doesn't quite line up with
    // member destruction here).
    std::shared_ptr<score::BarlineModel> barlineModel_;

    // Generation counter for async overview builds: rapid loadFile
    // calls invalidate older builds so a slow build for file A can't
    // overwrite a fresh overview for file B if it finishes second.
    // Worker threads are detached; the lambda posted back to the GUI
    // thread checks this counter before installing the result.
    std::atomic<std::uint64_t> overviewGeneration_{0};
};

} // namespace fiddler::ui
