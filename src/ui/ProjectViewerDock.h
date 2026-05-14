// ProjectViewerDock — the right-side artifact viewer / inspector.
//
// Layout:
//
//   ┌─────────────────────────────────┐
//   │ Project                         │ ← QDockWidget title
//   ├─────────────────────────────────┤
//   │ ▾ Markers                       │
//   │   Mark 1     (1000 ms)          │ ← QTreeWidget; one entry per marker
//   │   Mark 2     (1500 ms)          │
//   │ ▾ Loops                         │
//   │   ▶ Loop 1   (1000–3000 ms)     │ ← ▶ glyph marks the armed loop
//   │   Loop 2     (5000–7000 ms)     │
//   ├─────────────────────────────────┤
//   │ Properties                      │
//   │   Name:     [Mark 1____]        │ ← QStackedWidget; per-type property page
//   │   Position: [1000   ↕]          │   (here, the Marker page)
//   ├─────────────────────────────────┤
//   │           ◯ ◯ ◯                 │ ← LoopCountdownWidget; visible only
//   │         ◯       ◯               │   when the global pre-roll setting is
//   │           ◯ ◯ ◯                 │   enabled. Drives the practice-mode
//   │                                 │   ready/active visual.
//   └─────────────────────────────────┘
//
// MEMO: the dock is the home for "all transcription-project
// artifacts". Markers and loops are the first two categories;
// future steps may add notes, sections, etc. The QTreeWidget +
// QStackedWidget pattern is what lets each new category drop in
// alongside without restructuring.
//
// Selection is by stable artifact ID. Across categories the
// selection is mutually exclusive — the score widgets enforce the
// same rule (one of {barline, marker, loop} is selected at a time),
// and the dock mirrors that here so a single property page is
// always showing the right artifact's fields.
//
// Loop arming: this dock raises an `Armed` checkbox on the loop
// property page AND emits loopActivated on double-click. Both
// channels feed the same MainWindow handler so the user can use
// whichever idiom fits the moment (jump-and-play with a
// double-click; explicit toggle from the page).

#pragma once

#include "audio/Oscillator.h"   // for audio::Waveform (step 6.3 reference tone)

#include <QDockWidget>

#include <cstdint>
#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace fiddler::score {
class LoopModel;
class MarkerModel;
class NoteModel;
}

namespace fiddler::ui {

class LoopCountdownWidget;


class ProjectViewerDock : public QDockWidget {
    Q_OBJECT
public:
    explicit ProjectViewerDock(QWidget* parent = nullptr);
    ~ProjectViewerDock() override;

    // Attach (or detach with nullptr) the marker model the dock
    // browses + edits. Writes go through the model's API
    // (rename / setPosition); the dock holds a non-const handle
    // because of those writes.
    void setMarkerModel(std::shared_ptr<score::MarkerModel> model);
    [[nodiscard]] std::shared_ptr<score::MarkerModel>
        markerModel() const noexcept { return markerModel_; }

    // Attach (or detach with nullptr) the loop model. Same lifecycle
    // contract as the marker model — writes happen via the model's
    // public API; the dock holds a non-const handle so it can call
    // setRange / rename.
    void setLoopModel(std::shared_ptr<score::LoopModel> model);
    [[nodiscard]] std::shared_ptr<score::LoopModel>
        loopModel() const noexcept { return loopModel_; }

    // Attach the note model (Step 6.1). Same lifecycle contract as
    // the marker / loop models — writes happen via the model API;
    // dock holds a non-const handle for setInterval / setPitch /
    // rename round-trips.
    void setNoteModel(std::shared_ptr<score::NoteModel> model);
    [[nodiscard]] std::shared_ptr<score::NoteModel>
        noteModel() const noexcept { return noteModel_; }

    [[nodiscard]] std::optional<std::int64_t>
        selectedMarkerId() const noexcept { return selectedMarkerId_; }
    [[nodiscard]] std::optional<std::int64_t>
        selectedLoopId() const noexcept { return selectedLoopId_; }
    [[nodiscard]] std::optional<std::int64_t>
        selectedNoteId() const noexcept { return selectedNoteId_; }
    [[nodiscard]] std::optional<std::int64_t>
        armedLoopId() const noexcept { return armedLoopId_; }

public slots:
    // Programmatic selection setter — used by MainWindow to keep
    // the dock in sync with the score widgets. Mutually exclusive
    // with setSelectedLoopId (and vice versa).
    void setSelectedMarkerId(std::optional<std::int64_t> id);
    void setSelectedLoopId  (std::optional<std::int64_t> id);
    void setSelectedNoteId  (std::optional<std::int64_t> id);

    // Programmatic entry into the note property page's draft mode.
    // MainWindow calls this in response to noteAddRequested() with
    // playback-derived defaults (or armed-loop interval). The dock
    // populates its buffer + property page + button label, and any
    // existing note selection is cleared.
    void enterNoteDraftMode(std::int64_t startMs,
                            std::int64_t endMs,
                            int          midi);
    // Programmatic exit — used by tests; in production it happens
    // automatically on commit / click-outside / row-click.
    void exitNoteMode();

    // Pushed by MainWindow when the armed-loop state changes (the
    // user pressed Stop while looping, or armed a different loop).
    // The dock updates the tree row glyph and the Arm checkbox so
    // the UI matches transport state. nullopt = nothing armed.
    void setArmedLoopId(std::optional<std::int64_t> id);

    // Pushed by MainWindow when transport enters / exits a pre-roll
    // (Play press in practice mode) or a pause-between-repeats
    // window. `totalMs > 0` starts the countdown widget at the
    // bottom of the dock; cancel() halts it. The dock just forwards
    // to the widget — countdown animation lives entirely in
    // ui::LoopCountdownWidget.
    void startCountdown(int totalMs);
    void cancelCountdown();

    // Show / hide the global countdown widget. The widget lives at
    // the bottom of the dock and is meaningful only when the user
    // has enabled pre-roll (issue #16). MainWindow drives this in
    // sync with its own `prerollEnabled_` state.
    void setPrerollEnabled(bool enabled);
    [[nodiscard]] bool prerollEnabled() const noexcept { return prerollEnabled_; }

public:
    // ---- Reference tone (issue #step6.3) -----------------------
    //
    // Hover-tone mode controls whether moving the mouse over the
    // staff plays a reference tone, and how. Off = silent (clicks
    // on the piano keyboard still play). Continuous = tone follows
    // the hovered row. OnTap = silent on hover, T fires a pulse.
    //
    // MEMO: plain enum (not enum class) so MOC accepts Q_ENUM here.
    // Enumerators are scoped to the class either way
    // (ProjectViewerDock::OnTap) since they live inside the class
    // body — no global pollution.
    enum HoverToneMode { Off = 0, Continuous = 1, OnTap = 2 };
    Q_ENUM(HoverToneMode)

    [[nodiscard]] HoverToneMode hoverToneMode() const noexcept {
        return hoverToneMode_;
    }
    [[nodiscard]] audio::Waveform toneWaveform() const noexcept {
        return toneWaveform_;
    }

public slots:
    // MainWindow pushes the persisted values back into the dock at
    // startup (after loading QSettings). Drives the combo widgets
    // and emits hoverToneModeChanged for downstream wiring.
    void setHoverToneMode(HoverToneMode mode);
    void setToneWaveform(audio::Waveform waveform);

signals:
    // Selection changed in the dock — by tree click, by programmatic
    // set, or as a side effect of mutual exclusion. MainWindow
    // forwards to the score widgets.
    void markerSelectionChanged(std::optional<std::int64_t> id);
    void loopSelectionChanged  (std::optional<std::int64_t> id);
    void noteSelectionChanged  (std::optional<std::int64_t> id);

    // The user double-clicked a marker entry — "jump and play".
    // MainWindow seeks the player to the marker and starts
    // playback. Different from markerSelectionChanged: that's a
    // passive selection update, this is a request to act.
    void markerActivated(std::int64_t id);

    // Loop double-click — same idiom, but the action is "arm this
    // loop and start playing it from startMs". MainWindow makes
    // that policy decision; we just relay the gesture.
    void loopActivated(std::int64_t id);

    // Note double-click — seek to startMs and start playback. In
    // Step 6.2+, this will also fire a brief reference-tone pulse
    // at the note's pitch.
    void noteActivated(std::int64_t id);

    // "New Note ..." button pressed while in Empty mode — the user
    // wants to begin drafting a new note at the current seek. The
    // dock doesn't know the playback ms, so it asks MainWindow to
    // compute defaults and call enterNoteDraftMode() back on us.
    void noteAddRequested();

    // "Add Note" button pressed while in NewDraft mode — the user
    // wants to commit the current draft to the model. MainWindow
    // turns this into a noteModel_->add(...) call + undo push.
    void noteCommitNewRequested(std::int64_t startMs,
                                std::int64_t endMs,
                                int          midi);

    // "Apply Changes to Note" button pressed while in Editing mode —
    // the user wants to commit any buffered changes (pitch, interval)
    // to the existing model note. MainWindow computes the diff and
    // applies via existing setPitch / setInterval paths, pushing one
    // undo entry per changed field.
    void noteCommitChangesRequested(std::int64_t id,
                                    std::int64_t startMs,
                                    std::int64_t endMs,
                                    int          midi);

    // The user toggled the Arm checkbox in the loop property page.
    // `armed` is the *requested* new state (the property page already
    // reflects it; MainWindow updates the actual transport). Distinct
    // from loopActivated so MainWindow can disambiguate the gestures
    // in the event log.
    void loopArmToggleRequested(std::int64_t id, bool armed);

    // Reference-tone dock controls (issue #step6.3) — user changed
    // the hover-tone mode or the waveform via the combos. MainWindow
    // persists the new value in QSettings + dispatches incoming
    // hover signals from StaffWidget accordingly.
    //
    // MEMO: signal parameters are `int` (not the typed enums) so
    // they're MOC-friendly. Receivers cast to HoverToneMode /
    // audio::Waveform respectively; the int values are the enum
    // ordinals, kept stable as long as the enum definitions stay
    // in their declared order.
    void hoverToneModeChanged(int mode);
    void toneWaveformChanged(int waveform);

    // Property-page edits. The dock reads the new value off the
    // QSpinBox / QLineEdit and asks MainWindow to apply it; MainWindow
    // is the single place that mutates the models so it can capture
    // the pre-edit snapshot for the undo history before forwarding.
    // MEMO: same pattern as loopArmToggleRequested — the dock sees
    // the user gesture, MainWindow owns the side-effecting policy.
    void markerRenameRequested      (std::int64_t id, QString name);
    void markerPositionEditRequested(std::int64_t id, std::int64_t newMs);
    void loopRenameRequested        (std::int64_t id, QString name);
    void loopRangeEditRequested     (std::int64_t id,
                                     std::int64_t newStartMs,
                                     std::int64_t newEndMs);
    void noteRenameRequested        (std::int64_t id, QString name);
    void noteIntervalEditRequested  (std::int64_t id,
                                     std::int64_t newStartMs,
                                     std::int64_t newEndMs);
    void notePitchEditRequested     (std::int64_t id, int newMidi);

    // The user pressed Del on a focused tree entry. MainWindow turns
    // these into model->remove calls.
    void markerDeleteRequested(std::int64_t id);
    void loopDeleteRequested  (std::int64_t id);
    void noteDeleteRequested  (std::int64_t id);

    // Loop-creation gesture from the dock — Ctrl+left-click on a
    // marker row asks MainWindow to capture the *current* primary
    // anchor's ms as the secondary. The dock doesn't know what's
    // primary on the score widgets (it could be a barline), so the
    // signal is intentionally parameterless: MainWindow reads the
    // primary's ms from the waveform widget. Mirrors the Ctrl+click
    // gesture already on the score widgets.
    void loopAnchorAddRequested();
    // Plain left-click anywhere else in the tree clears any active
    // secondary anchor — same as a plain (non-Ctrl) click on the
    // score widgets.
    void loopAnchorClearRequested();

protected:
    // Forward Del key on the tree to the markerDeleteRequested
    // signal; let everything else fall through.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // Connected to MarkerModel::changed / LoopModel::changed /
    // NoteModel::changed — rebuild the tree section for the
    // changed kind, refresh the property page if it's still
    // showing a valid artifact.
    void onMarkerModelChanged();
    void onLoopModelChanged();
    void onNoteModelChanged();

    // Connected to QTreeWidget::currentItemChanged — translate the
    // newly-current item (or null) into an artifact selection.
    void onTreeCurrentItemChanged(QTreeWidgetItem* current,
                                  QTreeWidgetItem* previous);

    // Connected to QTreeWidget::itemDoubleClicked — emit
    // markerActivated for marker rows, loopActivated for loop rows.
    void onTreeItemDoubleClicked(QTreeWidgetItem* item, int column);

    // Property page edits round-trip into the model. The
    // updatingPropertyPage_ guard suppresses re-emit when *we* set
    // the field values from the model (not when the user types).
    void onMarkerNameEdited();
    // MEMO: takes no `newMs` because we read the spinbox's value()
    // directly. We bind to editingFinished (not valueChanged) so
    // the user can type leading zeros while editing without each
    // keystroke round-tripping through the model and stripping
    // them. See the slot impl for the full reasoning.
    void onMarkerPositionEdited();

    // Loop property page slots. Range edits enforce the
    // start < end invariant by widening the partner box's range
    // bound; if a user types start > current end, end auto-bumps
    // to start + 1. Same editingFinished discipline as markers.
    void onLoopNameEdited();
    void onLoopStartEdited();
    void onLoopEndEdited();
    void onLoopArmedToggled(bool checked);

    // Note property page slots. Pitch is entered as SPN ("A4",
    // "F#5") in a QLineEdit; on editingFinished we parse via
    // score::spnToMidi and validate via NoteModel::isAcceptedPitch.
    // Invalid input reverts the line edit to the current value.
    // In NewDraft / Editing modes the value updates the in-memory
    // buffer; the noteModel is only touched on commit.
    void onNotePitchEdited();
    void onNoteStartEdited();
    void onNoteEndEdited();

    // Single button at the bottom of the note property panel. Its
    // label changes with the mode ("New Note ..." / "Add Note" /
    // "Apply Changes to Note"); a click does whatever the current
    // mode says. See the state-machine MEMO on noteBuffer_.
    void onAddNoteClicked();

private:
    void buildUi();
    void rebuildMarkerSection();
    void rebuildLoopSection();
    void refreshPropertyPage();

    // Switch the property stack to the given page AND update the
    // caption label above it ("Marker properties:" / "Loop
    // properties:" / "Note properties:" / hidden). Single
    // entry-point so the caption and visible page never drift apart.
    void setPropertyPage(int pageIndex);

    // Update the single Add-Note button to match the current
    // note-property-page mode. Empty → "New Note ...";
    // NewDraft → "Add Note"; Editing → button is hidden (live-commit
    // via per-field edits — same inspector pattern as markers/loops).
    void updateAddNoteButtonLabel();

    // Populate noteBuffer_ from an existing note's current model
    // values. Used when entering Editing mode (note row click, or
    // setSelectedNoteId).
    void populateBufferFromModel(std::int64_t id);
    // Decide which loop-anchor signal to fire for a tree mouse-press.
    // Pulled out as a helper because eventFilter() should stay
    // narrowly focused on dispatch.
    void handleTreeMousePress(class QMouseEvent* me);

    // Find the QTreeWidgetItem (under the appropriate category) whose
    // user-data ID matches `id`. Returns nullptr if not found.
    [[nodiscard]] QTreeWidgetItem*
        findMarkerItem(std::int64_t id) const;
    [[nodiscard]] QTreeWidgetItem*
        findLoopItem(std::int64_t id) const;

    std::shared_ptr<score::MarkerModel> markerModel_;
    std::shared_ptr<score::LoopModel>   loopModel_;
    std::shared_ptr<score::NoteModel>   noteModel_;

    // Mutually exclusive — at most one of these has a value.
    std::optional<std::int64_t>         selectedMarkerId_;
    std::optional<std::int64_t>         selectedLoopId_;
    std::optional<std::int64_t>         selectedNoteId_;

    // Independent of selection — a loop can be armed without being
    // selected (the user could click another artifact while the loop
    // is still wrapping). nullopt means "nothing armed".
    std::optional<std::int64_t>         armedLoopId_;

    // Top-level items in the tree.
    QTreeWidget*     tree_              = nullptr;
    QTreeWidgetItem* markersCategory_   = nullptr;
    QTreeWidgetItem* loopsCategory_     = nullptr;

    // "Add Note" button — sits below the property stack, always
    // visible, enabled only when a NoteModel is attached.
    QPushButton*     addNoteButton_     = nullptr;

    // Reference-tone controls (issue #step6.3). Live below the
    // Add-Note button, above the countdown widget.
    QComboBox*       hoverToneCombo_    = nullptr;
    QComboBox*       toneWaveformCombo_ = nullptr;
    HoverToneMode    hoverToneMode_     = HoverToneMode::OnTap;
    audio::Waveform  toneWaveform_      = audio::Waveform::Triangle;

    // Property-page stack. Page indices live in the .cpp.
    QStackedWidget*  propertyStack_     = nullptr;

    // Caption above the property stack — "Marker properties:",
    // "Loop properties:", "Note properties:", or hidden when no
    // artifact is selected. Updated together with the stack's
    // current page via setPropertyPage().
    QLabel*          propertyCaption_   = nullptr;

    // Marker page widgets.
    QLineEdit*       markerNameEdit_    = nullptr;
    QSpinBox*        markerPositionBox_ = nullptr;

    // Loop page widgets.
    QLineEdit*           loopNameEdit_      = nullptr;
    QSpinBox*            loopStartBox_      = nullptr;
    QSpinBox*            loopEndBox_        = nullptr;
    QCheckBox*           loopArmedCheck_    = nullptr;

    // Note page widgets. Pitch is entered as SPN in a QLineEdit
    // (e.g. "A4", "F#5"); the read-only QLabel beside it shows
    // the corresponding MIDI number. End is lower-clamped to
    // start+1 the same way LoopEnd is.
    //
    // MEMO[#step6.1]: there's NO per-note Name field. Top-level
    // notation apps (MuseScore, Sibelius, MusicXML, ABC) don't have
    // one either — notes are anonymous; lyrics / fingerings /
    // annotations get their own slots. NoteModel::name still
    // exists for future annotation use, but it's not user-editable
    // from the dock.
    QLineEdit*       notePitchEdit_     = nullptr;
    QLabel*          notePitchMidiLabel_= nullptr;
    QSpinBox*        noteStartBox_      = nullptr;
    QSpinBox*        noteEndBox_        = nullptr;
    QLabel*          noteDurationLabel_ = nullptr;

    // ---- Note property-page state machine (step 6.1) ------------
    //
    // The property page has three modes, derived from noteBuffer_:
    //   Empty           — noteBuffer_ not set; button = "New Note …"
    //   NewDraft        — noteBuffer_ set, id == 0; button = "Add Note"
    //   Editing         — noteBuffer_ set, id  > 0; button = "Apply
    //                     Changes to Note"
    //
    // Field edits in NewDraft / Editing modes mutate the buffer only;
    // the noteModel is touched at commit time (button click) via
    // noteCommitNewRequested / noteCommitChangesRequested signals to
    // MainWindow. This keeps the model side intact (and undo-able
    // atomically) while making "is the form a draft or is it already
    // saved?" obvious to the user.
    struct NoteBuffer {
        std::int64_t id      = 0;    // 0 ⇒ draft; >0 ⇒ existing note id
        std::int64_t startMs = 0;
        std::int64_t endMs   = 0;
        int          midi    = 69;
        QString      name;            // forward-compat; not in UI
    };
    std::optional<NoteBuffer> noteBuffer_;

    // Global countdown widget — sits at the bottom of the dock,
    // visible only when prerollEnabled_ is true. Decoupled from
    // any specific loop because pre-roll is a global setting now
    // (issue #16) and fires on every Play press, not just armed
    // loops.
    LoopCountdownWidget* loopCountdown_     = nullptr;
    bool                 prerollEnabled_    = false;

    // MEMO: invariant — set true while we're populating the
    // property-page widgets from the model, so their valueChanged
    // / editingFinished / toggled slots don't loop back into model
    // writes or re-emit user-intent signals. Reset before returning
    // from refreshPropertyPage() / setArmedLoopId().
    bool updatingPropertyPage_ = false;
};

} // namespace fiddler::ui
