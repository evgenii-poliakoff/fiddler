// MainWindow — top-level Qt window. Owns the audio engine, drives
// the transport controls, and coordinates the waveform view.

#pragma once

#include "ui/UndoEntry.h"

#include <QMainWindow>
#include <QStringList>
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
class QMenu;
class QShortcut;
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

    // Project save / load (#26). Same shape as loadFile — public
    // so integration tests can drive the round-trip without going
    // through QFileDialog. The slots onSave / onSaveAs end up
    // calling saveProject after picking a path. openProject is
    // called from onOpenFile when the chosen file has a .fdlp
    // suffix.
    bool saveProject(const QString& path);
    bool openProject(const QString& path);

    // Open Recent dispatch (#43). Looks at the path's suffix and
    // routes to openProject (.fdlp) or loadFile (anything else).
    // On success the path is prepended to the MRU list in
    // QSettings (cap 10, deduped). On a missing-file the entry
    // is dropped from the MRU and a warning shown. Public so
    // tests drive recent-file picks the same way the menu does.
    bool openByPath(const QString& path);

    // Read-only view of the persisted MRU list. Test seam.
    [[nodiscard]] QStringList recentFiles() const;
    void clearRecentFiles();

    // Read-only access to the audio engine. Used by integration tests
    // to verify that UI events (clicks, slider moves) propagate
    // through to the player.
    [[nodiscard]] const audio::Player& player() const noexcept { return *player_; }

    // Test seam (#22) — true while a drag is in flight on either
    // score widget. Used by tests to verify that the playback-
    // position poll pauses during the drag.
    [[nodiscard]] bool dragInFlight() const noexcept { return dragInFlight_; }

    // Test seam (#26) — suppress the close-with-dirty prompt so a
    // test that calls window->close() doesn't block on a modal
    // QMessageBox (there's no user to click). Production code
    // leaves this false. Tests that exercise the close path with
    // dirty state should drive openProject / saveProject directly
    // and verify the dirty flag rather than rely on the dialog.
    void setSuppressCloseDirtyPromptForTest(bool suppress) noexcept {
        suppressCloseDirtyPrompt_ = suppress;
    }

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
    // MEMO: per-widget event filter installed on every input that
    // would otherwise consume Ctrl+Z for its own text-undo
    // (QSpinBox / QLineEdit accept Qt::ShortcutOverride for the
    // standard editing shortcuts). The filter rejects the
    // override so our window-level Ctrl+Z always reverses the
    // last document edit, regardless of focus. ProjectViewerDock
    // installs an equivalent filter on its own inputs.
    bool eventFilter(QObject* watched, QEvent* event) override;

    // MEMO: closeEvent is overridden purely to emit a `[ui.file] close`
    // log line — when the user closes the window we want the log to
    // record the boundary of the session, so a later log-replay test
    // knows the scenario ends here.
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onOpenFile();

    // Project save/load (#26). Save / Save As route through
    // saveProject(path); Open with a .fdlp suffix routes through
    // openProject(path). The path is absolute, picked by the
    // user via QFileDialog. Save with no project bound yet
    // falls through to Save As.
    void onSave();
    void onSaveAs();

private:
    // Dirty / window-title helpers for the project flow (#26).
    //
    //   recomputeDirty   — re-derive dirty_ from
    //                      (undoHistory_.size() != savedUndoSize_)
    //                      || nonUndoableDirty_. Called after every
    //                      undo push, every onUndo pop, and from
    //                      slots that change non-undo state (e.g.
    //                      the tune-type picker).
    //   markClean        — snapshot the current state as the new
    //                      saved baseline (savedUndoSize_ = current
    //                      size, nonUndoableDirty_ = false), then
    //                      recompute. Called by save / openProject /
    //                      loadFile to mark "this is the state on
    //                      disk now".
    //   updateWindowTitle — re-compose the title from projectPath_
    //                       and dirty_.
    void recomputeDirty();
    void markClean();
    void updateWindowTitle();

    // Single helper for every "user did a reversible thing" call
    // site. Gates on `applyingUndo_` so the dispatch's own model
    // mutations don't push their own reversal entries, emplaces
    // the typed entry, and recomputes dirty. Push sites become
    //     pushUndoEntry(undo::AddBarline{pos});
    // instead of the four-line if/emplace/recompute block.
    void pushUndoEntry(undo::Entry entry);

    // Recent files (#43). pushRecentFile prepends, dedupes, and
    // truncates to kMaxRecentFiles; called only from successful
    // loadFile / openProject paths. rebuildRecentFilesMenu repopulates
    // the submenu from QSettings + binds a one-per-item action that
    // calls openByPath. Slot so QMenu::aboutToShow can wire to it.
    static constexpr int kMaxRecentFiles = 10;
    void pushRecentFile(const QString& path);

private slots:
    void rebuildRecentFilesMenu();

private slots:
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

    // The 'Ctrl+Z' shortcut: reverse the most recent user-driven
    // model mutation, regardless of kind — placement, drag, dock
    // edit, rename, or delete. MEMO: see undoHistory_ — every push
    // point captures enough state to round-trip the mutation
    // exactly (e.g. drag-undo restores the pre-drag position;
    // delete-undo restores the artifact with its original id and
    // name). Per `feedback_simple_first.md` the implementation is
    // a tagged-variant LIFO, not QUndoStack.
    void onUndo();

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

    // Property-page edit slots. The dock emits these (instead of
    // mutating the model directly) so MainWindow can snapshot the
    // pre-edit value into the undo history before applying.
    void onDockMarkerRenameRequested      (std::int64_t id, QString name);
    void onDockMarkerPositionEditRequested(std::int64_t id, std::int64_t newMs);
    void onDockLoopRenameRequested        (std::int64_t id, QString name);
    void onDockLoopRangeEditRequested     (std::int64_t id,
                                           std::int64_t newStartMs,
                                           std::int64_t newEndMs);

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

    // Drag-bracket slots (#22). Pause the playback-position poll
    // for the duration of a drag so its 50 ms repaints don't add
    // to the per-move workload. The poll resumes on dragEnded.
    void onDragStarted();
    void onDragEnded();

    // The window-level 'Del' shortcut: removes the currently-selected
    // artifact (barline OR marker) regardless of which child widget
    // has focus. Selection is mutually exclusive between the two
    // kinds, so we can dispatch by checking which one is set.
    void onDeleteSelectedArtifact();

private:
    // Apply a drag commit to the model — called from the
    // markerDragCommitted / loopDragCommitted slot lambdas. Under
    // the unified ghost flow (#22) the model is unchanged
    // mid-drag, so this is the ONE place that writes for the
    // entire drag: it pushes the pre-drag snapshot onto the undo
    // history (so Ctrl+Z reverses), then runs setPosition /
    // setRange, then clears the sister widget's ghost so paint
    // returns to the model's value.
    //
    // `via` is the widget tag for the log line ("waveform" /
    // "staff") since the source-widget identity is lost by the
    // time we get here.
    void commitMarkerDrag(std::int64_t id,
                          std::int64_t fromMs,
                          std::int64_t toMs,
                          const char*  via);
    void commitLoopDrag  (std::int64_t id,
                          bool         isStart,
                          std::int64_t fromMs,
                          std::int64_t toMs,
                          const char*  via);

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
    QAction*        saveAction_     = nullptr;
    QAction*        saveAsAction_   = nullptr;

    // Open Recent submenu (#43). Populated lazily from
    // QSettings(mainwindow/recentFiles) on every aboutToShow, so
    // a recent-files update via openByPath() is picked up without
    // any explicit rebuild from the open call sites.
    QMenu*          recentFilesMenu_ = nullptr;
    QPushButton*    playButton_     = nullptr;
    QPushButton*    stopButton_     = nullptr;
    QSlider*        positionSlider_ = nullptr;
    QSlider*        tempoSlider_    = nullptr;
    QLabel*         tempoLabel_     = nullptr;
    QLabel*         statusLabel_    = nullptr;
    QSpinBox*       prerollBox_       = nullptr;
    QCheckBox*      prerollEnabledBox_ = nullptr;
    QTimer*         positionTimer_    = nullptr;

    // True while either score widget is mid-drag. The position-
    // poll timer pauses while this is set so its slider/label
    // repaints don't add to the per-move workload — see #22.
    bool            dragInFlight_     = false;

    // Project save/load state (#26).
    //
    //   currentAudioPath_  the absolute path of the most-recently
    //                      loaded audio file, or empty if nothing
    //                      has been loaded yet. Survives into the
    //                      project file so reopening a .fdlp can
    //                      re-load the same audio.
    //   projectPath_       the absolute path of the bound .fdlp
    //                      file, or empty if the session hasn't
    //                      been saved yet (Save As prompts).
    //   dirty_             set true on any model mutation; flipped
    //                      false by save / openProject / loadFile.
    //                      Drives the window title's asterisk and
    //                      the close-with-dirty prompt.
    QString         currentAudioPath_;
    QString         projectPath_;
    bool            dirty_            = false;

    // Snapshot of undoHistory_.size() at the moment we last saved
    // (or opened) the project. dirty_ becomes true when the current
    // size diverges from this, false when we return to it — so a
    // Ctrl+Z that walks the undo history back to the saved
    // checkpoint clears the asterisk, instead of leaving it stuck.
    std::size_t     savedUndoSize_    = 0;

    // Catch-all for state changes that aren't in the undo history
    // (today: tune-type / time-signature picks). Set true on any
    // such change, cleared on save / open. Combined with the
    // history-size compare in recomputeDirty().
    bool            nonUndoableDirty_ = false;

    bool            suppressCloseDirtyPrompt_ = false;
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

    // Combined undo LIFO across every user-driven mutation —
    // placements, drags, dock edits, renames, and deletes. Owned by
    // MainWindow rather than by any model so cross-kind ordering is
    // preserved. Each push captures enough state to reverse that
    // exact mutation; onUndo() pops the back and dispatches via
    // std::visit. See `ui/UndoEntry.h` for the variant arms.
    //
    // MEMO: under this design, deletes are themselves history
    // entries (DeleteMarker / DeleteLoop / DeleteBarline carry the
    // pre-delete snapshot), so there is no stale-after-delete case
    // — onUndo always reverses what's on top, never peels.
    std::vector<undo::Entry> undoHistory_;

    // True while onUndo() is dispatching. Push points consult this
    // flag and skip pushing so the dispatch's own model mutators
    // don't pollute the history (otherwise Ctrl+Z would push its
    // own reversal entry and the next Ctrl+Z would re-do it).
    bool applyingUndo_ = false;

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
