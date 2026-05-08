// MainWindow — top-level Qt window. Owns the audio engine, drives
// the transport controls, and coordinates the waveform view.

#pragma once

#include <QMainWindow>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QComboBox;
class QPushButton;
class QSlider;
class QLabel;
class QShortcut;
class QString;
class QTimer;

namespace fiddler::audio { class Player; }
namespace fiddler::score {
class BarlineModel;
class LoopModel;
class MarkerModel;
}

namespace fiddler::ui {

class ProjectViewerDock;
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

    // Access to the shared score models. Used by integration tests
    // to verify that tap-to-place / Ctrl+Z / time-sig picker /
    // marker-related mutations actually reach the models.
    [[nodiscard]] const score::BarlineModel& barlineModel() const noexcept;
    [[nodiscard]] const score::MarkerModel&  markerModel()  const noexcept;
    [[nodiscard]] const score::LoopModel&    loopModel()    const noexcept;

protected:
    // MEMO: closeEvent is overridden purely to emit a `[ui.file] close`
    // log line — when the user closes the window we want the log to
    // record the boundary of the session, so a later log-replay test
    // knows the scenario ends here.
    void closeEvent(QCloseEvent* event) override;

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

    // The 'M' shortcut: place a marker at the player's current
    // source-time position. Auto-named "Mark N" by the model;
    // user can rename via the project viewer dock.
    void onTapMarker();

    // The 'Ctrl+Z' shortcut: peel the most-recently-added artifact
    // (barline, marker, or loop — whichever was most recent) from
    // its model. MEMO: see placementHistory_ — this is the combined
    // LIFO that makes "Z = undo last placement" feel right
    // regardless of kind.
    void onUndoLastPlacement();

    // The 'L' shortcut: turn a primary + secondary anchor pair into
    // a new loop. Reads both anchors off the waveform widget; the
    // staff and dock keep them in sync via the same mirror plumbing
    // that already covers single selections. Lower of the two ms
    // values becomes startMs, higher becomes endMs.
    void onCreateLoop();

    // The tune-type picker: looks up the chosen preset and pushes
    // its TimeSignature into the model.
    void onTuneTypePresetChosen(int comboIndex);

    // Barline selection mirroring between waveform and staff.
    void onWaveformBarlineSelectionChanged(std::optional<std::size_t> index);
    void onStaffBarlineSelectionChanged   (std::optional<std::size_t> index);

    // Marker selection mirroring across waveform / staff / dock —
    // three-way mirror because the dock also shows the selection.
    void onWaveformMarkerSelectionChanged(std::optional<std::int64_t> id);
    void onStaffMarkerSelectionChanged   (std::optional<std::int64_t> id);
    void onDockMarkerSelectionChanged    (std::optional<std::int64_t> id);

    // Loop selection mirroring — same three-way shape as markers.
    void onWaveformLoopSelectionChanged(std::optional<std::int64_t> id);
    void onStaffLoopSelectionChanged   (std::optional<std::int64_t> id);
    void onDockLoopSelectionChanged    (std::optional<std::int64_t> id);

    // Secondary-anchor mirroring (waveform <-> staff). The dock has
    // no secondary anchor — the L gesture only originates from the
    // score widgets, so two participants are sufficient.
    void onWaveformSecondaryAnchorChanged(std::optional<std::int64_t> ms);
    void onStaffSecondaryAnchorChanged   (std::optional<std::int64_t> ms);

    // The user double-clicked a marker in the dock — seek the
    // player to the marker's position and start playback.
    // "Jump-and-play" is the standard DAW idiom for an
    // activate-on-double-click marker list.
    void onMarkerActivated(std::int64_t id);

    // The user double-clicked a loop in the dock — arm it,
    // seek to its startMs, and start playback. Same jump-and-play
    // idiom as markers; the difference is that arming switches the
    // transport into wrap-around mode (see updatePosition).
    void onLoopActivated(std::int64_t id);

    // The Arm checkbox on the loop property page was toggled.
    // `armed=true` arms the loop *without* seeking or auto-playing
    // (the user might be mid-listen and just want wrap-around to
    // start happening at endMs); `armed=false` disarms.
    void onLoopArmToggleRequested(std::int64_t id, bool armed);

    // Connected to LoopModel::changed — drops the armed state if
    // the armed loop has been removed from the model.
    void onLoopModelChanged();

    // 'Del' on a widget: turn the requested-by-key signal into an
    // actual model mutation.
    void onBarlineDeleteRequested(std::size_t index);
    void onMarkerDeleteRequested (std::int64_t id);
    void onLoopDeleteRequested   (std::int64_t id);

    // The window-level 'Del' shortcut: removes the currently-selected
    // artifact (barline OR marker) regardless of which child widget
    // has focus. Selection is mutually exclusive between the two
    // kinds, so we can dispatch by checking which one is set.
    void onDeleteSelectedArtifact();

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
    ProjectViewerDock* projectViewerDock_ = nullptr;
    QShortcut*      tapBarShortcut_    = nullptr;   // 'B'
    QShortcut*      tapMarkerShortcut_ = nullptr;   // 'M'
    QShortcut*      createLoopShortcut_ = nullptr;  // 'L'
    QShortcut*      undoShortcut_      = nullptr;   // Ctrl+Z
    QShortcut*      deleteBarShortcut_ = nullptr;   // 'Del' (artifact-agnostic)

    // The shared score models — owned here, observed by widgets +
    // dock. shared_ptr (not unique_ptr) so widget handles can
    // extend the lifetime if MainWindow tears down before they do
    // (Qt's parent-child destruction order doesn't quite line up
    // with member destruction here).
    std::shared_ptr<score::BarlineModel> barlineModel_;
    std::shared_ptr<score::MarkerModel>  markerModel_;
    std::shared_ptr<score::LoopModel>    loopModel_;

    // MEMO: invariant — selection mirrors are one-shot, not loops.
    // Each widget (and now the dock) emits *SelectionChanged when
    // its highlight changes; MainWindow forwards each event to the
    // siblings. Without a guard, "user clicks waveform" would
    // bounce: waveform→staff→waveform→staff…  setSelectedBarline /
    // setSelectedMarkerId are no-op-on-equal so the bounce
    // terminates anyway, but we still see the echo emissions and
    // would log them as user actions in the wrong direction.
    // mirroringSelection_ is set true while we're propagating from
    // an originating slot to its siblings; receiving slots return
    // early when they see the flag, so only the originating slot
    // logs and forwards. Reset after the forwarding calls return.
    bool mirroringSelection_ = false;

    // Combined undo LIFO across barlines + markers, owned by
    // MainWindow rather than by either model. Each tap pushes its
    // kind; Ctrl+Z pops the back and dispatches to the matching
    // model's undoLastAdd. MEMO: invariant — manual deletion of an
    // artifact via Del does *not* update this stack, so a stale
    // entry can sit at the top after a delete; onUndoLastPlacement
    // peels through stale entries until either an undoLastAdd
    // succeeds or the history drains. See feedback_simple_first.md
    // — this is the simple route; a full QUndoStack would be richer
    // but we don't need redo or cross-action undo yet.
    enum class PlacementKind { Barline, Marker, Loop };
    std::vector<PlacementKind> placementHistory_;

    // MEMO: armedLoopId_ is the canonical "transport is wrapping
    // around this loop" state. The dock mirrors it via setArmedLoopId
    // so the Armed checkbox + tree glyph match. Stop disarms; file
    // load disarms; removing the armed loop from the model disarms.
    std::optional<std::int64_t> armedLoopId_;

    // Suppress re-entering the wrap path while a pause-between-
    // repeats timer is in flight. Without this, the GUI poll could
    // see pos > endMs on subsequent ticks (the player paused but
    // the position didn't fully reset until the seek lands) and
    // schedule duplicate wraps. Reset by the timer's lambda when
    // the loop is rearmed at startMs.
    bool wrapPending_ = false;

    // Generation counter for async overview builds: rapid loadFile
    // calls invalidate older builds so a slow build for file A can't
    // overwrite a fresh overview for file B if it finishes second.
    // Worker threads are detached; the lambda posted back to the GUI
    // thread checks this counter before installing the result.
    std::atomic<std::uint64_t> overviewGeneration_{0};
};

} // namespace fiddler::ui
