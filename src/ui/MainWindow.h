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
class QCheckBox;
class QComboBox;
class QPushButton;
class QSpinBox;
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

    // Test seams for the wrap-pause state machine (issue #13).
    // Production code never calls enterWrapPauseForTest — the real
    // entry is updatePosition's wrap branch. Exposed so integration
    // tests can verify the cancel paths (Play press / seek during
    // pause) without depending on audio actually advancing position
    // past endMs, which is unreliable on headless CI.
    void enterWrapPauseForTest(std::int64_t loopId, int prerollMs);
    [[nodiscard]] bool wrapPending() const noexcept { return wrapPending_; }

    // Global pre-roll setting (issue #16). Replaces the per-loop
    // pauseMs that used to live on `Loop`. Applies uniformly to
    // every transition to Playing — Play press (with or without
    // armed loop), double-click jump-and-play (markers + loops),
    // Arm-then-Play, cancel-then-resume — AND to the pause-between-
    // repeats window for armed loops. Same value, same feel.
    //
    // Two-mode design:
    //   * **passive listening** (`prerollEnabled = false`): no
    //     pre-roll on Play, no wrap silence. Effective pre-roll =
    //     0 regardless of the spinbox value.
    //   * **practice mode** (`prerollEnabled = true`): every
    //     transition to Playing inserts a countdown for `prerollMs`
    //     so the player has time to prepare (hand from mouse to
    //     instrument, position bow).
    //
    // Default at first launch is **off** — the user starts by
    // listening, then explicitly switches into practice mode.
    // Persisted across launches via QSettings.
    //
    // `prerollMs()` returns the EFFECTIVE value (0 when disabled).
    // `prerollMsValue()` returns the raw spinbox value regardless
    // of enabled state — useful for tests + UI restore.
    [[nodiscard]] int  prerollMs() const noexcept {
        return prerollEnabled_ ? prerollMs_ : 0;
    }
    [[nodiscard]] int  prerollMsValue() const noexcept { return prerollMs_; }
    [[nodiscard]] bool prerollEnabled() const noexcept { return prerollEnabled_; }
    void setPrerollMs(int ms);
    void setPrerollEnabled(bool enabled);

    // Pure helper for the wrap-trigger rule: wrap only when the
    // playhead crossed `endMs` going forward during NATURAL
    // playback, not when the user seeked past it. Detected by
    // requiring `previousPosMs < endMs && currentPosMs >= endMs`.
    // Without this, a click on the waveform to the right of the
    // loop yanks the user back via wrap (UX-asymmetric to clicks
    // on the left, which play forward freely). See issue #13.
    //
    // Static + public so tests can pin the truth table directly,
    // without driving the full updatePosition path which depends
    // on Player::state being Playing (unreliable on headless CI).
    [[nodiscard]] static bool wrapShouldFire(std::int64_t previousPosMs,
                                             std::int64_t currentPosMs,
                                             std::int64_t endMs) noexcept;

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
    // editingFinished on the pre-roll spinbox — commits the new
    // value, persists to QSettings, no immediate transport effect
    // (next Play / wrap will pick it up).
    void onPrerollChanged();
    // toggled on the pre-roll checkbox — flips practice mode on/off.
    void onPrerollEnabledToggled(bool enabled);
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

    // Secondary-anchor mirroring (waveform <-> staff). The dock
    // doesn't track its own secondary anchor — it just emits gesture
    // signals that we translate into setSecondaryAnchorMs calls on
    // the score widgets here.
    void onWaveformSecondaryAnchorChanged(std::optional<std::int64_t> ms);
    void onStaffSecondaryAnchorChanged   (std::optional<std::int64_t> ms);
    // Ctrl+left-click on a marker row in the dock — capture the
    // current primary anchor's ms (read from the waveform widget,
    // which is the canonical primary holder) as the secondary.
    void onDockLoopAnchorAddRequested();
    // Plain left-click elsewhere in the dock tree — clear secondary.
    void onDockLoopAnchorClearRequested();

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

    // Drag-to-nudge requests from the score widgets (issue #11).
    // Fired per mouse-move while the drag is in flight; we just
    // forward to the model. Logging happens on the *Committed slots
    // below — per-move logs would flood the event log.
    void onMarkerDragRequested(std::int64_t id, std::int64_t newMs);
    void onLoopDragRequested  (std::int64_t id,
                               std::int64_t newStartMs,
                               std::int64_t newEndMs);

    // The window-level 'Del' shortcut: removes the currently-selected
    // artifact (barline OR marker) regardless of which child widget
    // has focus. Selection is mutually exclusive between the two
    // kinds, so we can dispatch by checking which one is set.
    void onDeleteSelectedArtifact();

private:
    void buildMenus();
    void buildCentralWidget();
    // Save / restore window geometry + dock layout in QSettings.
    // MEMO: the user can close the project viewer dock via its X
    // button; without persistence they'd be greeted by a fresh-
    // looking layout on every launch. QSettings encodes the
    // current dock layout, including which docks are hidden,
    // floating, or resized.
    void saveLayout() const;
    void restoreLayout();

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
    QSpinBox*       prerollBox_       = nullptr;
    QCheckBox*      prerollEnabledBox_ = nullptr;
    QTimer*         positionTimer_    = nullptr;
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
    // the loop is rearmed at startMs, OR by `cancelPendingWrap`
    // when the user interrupts the pause via Play / seek.
    bool wrapPending_ = false;

    // Global pre-roll setting (issue #16). Lives on MainWindow
    // because it's a UI / transport policy, not a domain-model
    // concept. Persisted across launches via QSettings.
    int  prerollMs_      = 500;     // raw spinbox value
    bool prerollEnabled_ = false;   // off by default — see header doc

    // Position observed at the END of the previous updatePosition
    // tick (or after a user seek). The wrap rule fires only on a
    // forward crossing of endMs (`previousPosMs_ < endMs && pos >=
    // endMs`); user-initiated seeks update this so a jump past
    // endMs doesn't look like a fresh crossing on the next tick.
    // See issue #13 + wrapShouldFire().
    //
    // Sentinel -1 means "no previous tick yet" — first tick after
    // file load doesn't fire wrap regardless of position.
    std::int64_t previousPosMs_ = -1;

    // The pause-between-repeats QTimer. Single-shot, started when
    // we enter the wrap-pause window, fires after `pauseMs` to
    // resume playback. Held as a member (rather than the static
    // QTimer::singleShot we used originally) so user actions can
    // explicitly cancel it — pressing Play during the pause should
    // stop the auto-resume; seeking during the pause should leave
    // the player paused at the new position. See issue #13 for
    // the rationale.
    QTimer*      wrapTimer_         = nullptr;
    // The loop id captured at wrap entry, or nullopt for a generic
    // (no-armed-loop) Play pre-roll. The timer's timeout handler
    // checks the optional: if it has a value, it must still match
    // `armedLoopId_` (rejects the case where the user armed a
    // different loop during the pause); if it's nullopt, the timer
    // simply resumes playback wherever the player currently sits.
    std::optional<std::int64_t> wrapTargetLoopId_;

    // Cancel any pending pre-roll / pause-between-repeats. Stops
    // the timer, clears `wrapPending_`, cancels the dock's
    // countdown widget. Idempotent — safe to call when no wrap is
    // pending.
    void cancelPendingWrap();

    // Single source of truth for "transition the player to playing".
    // Routes through a pre-roll wrap-pause when the effective
    // `prerollMs() > 0` (i.e. checkbox enabled and value > 0);
    // otherwise plays immediately. Used by every Play path
    // (onPlayPause, onLoopActivated, onMarkerActivated, the
    // cancel-then-resume case, etc.) so the user gets consistent
    // pre-roll feel regardless of how they entered the play state.
    // MEMO: do NOT call from the wrap timer's lambda (that fires
    // AFTER the wrap-pause that was the pre-roll for the next
    // iteration — calling this would loop forever).
    void startPlayback();

    // Generation counter for async overview builds: rapid loadFile
    // calls invalidate older builds so a slow build for file A can't
    // overwrite a fresh overview for file B if it finishes second.
    // Worker threads are detached; the lambda posted back to the GUI
    // thread checks this counter before installing the result.
    std::atomic<std::uint64_t> overviewGeneration_{0};
};

} // namespace fiddler::ui
