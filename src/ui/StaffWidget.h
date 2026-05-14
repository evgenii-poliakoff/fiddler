// StaffWidget — chromatic piano-roll editor for the transcribed
// notes plus user-placed barlines, markers, loop bands, secondary
// anchor, and a playhead cursor.
//
// MEMO[refactor — step 6.2]: the widget was a 5-line treble staff
// through step 6.1. The diatonic layout couldn't represent
// accidentals naturally and made click-to-place ambiguous. Step 6.2
// replaces the staff lines with a CHROMATIC GRID: one row per
// semitone across the violin's playable range (G3 = midi 55 up to
// E7 = midi 100), with a piano keyboard column on the left.
//
// MEMO[refactor]: the artifact-related state, mouse hit-testing,
// and key navigation moved to `ScoreOverlayBase` in #12. StaffWidget
// now contributes:
//
//   * `durationMs_` (the source-time duration; the staff has no
//     waveform overview to read it from)
//   * coord transforms based on `durationMs_` (the base reads
//     leftMarginPx() = piano-keyboard width to offset the time
//     axis past the keyboard)
//   * the piano-roll-specific paint (keyboard, chromatic grid,
//     C-note labels) plus the artifact-overlay paint shared in
//     shape but not in details with WaveformWidget
//   * click-to-place — a left-click on the grid emits
//     placeNoteRequested(ms, midi). MainWindow turns that into a
//     model add + selection.
//
// Why source-time-proportional widths (and not equal-width bars
// like classical engraving): the staff lines up 1:1 with the
// waveform above. Clicking a bar on the staff lands you at the
// same x in the waveform, and the cursor moves at one rate across
// both. Engraving-aware layout would need rhythmic interpretation
// (beats, tempo), which the step 6 plan defers to Step 7.

#pragma once

#include "ui/ScoreOverlayBase.h"

#include <cstdint>
#include <optional>

class QPaintEvent;
class QPainter;

namespace fiddler::ui {

class StaffWidget : public ScoreOverlayBase {
    Q_OBJECT
public:
    // Width of the piano-keyboard column on the left of the staff,
    // in pixels. Exposed so MainWindow can indent the waveform /
    // scrollbar / position slider by the same amount, keeping a
    // single continuous cursor line across the waveform AND the
    // staff at the same source-ms. Mirrored as the private layout
    // constant in the .cpp.
    static constexpr int kKeyboardWidthPx = 56;

    explicit StaffWidget(QWidget* parent = nullptr);
    ~StaffWidget() override;

    // The source-time duration of the loaded file, in milliseconds.
    // Used as the right-hand bound of the source-time → x-pixel
    // mapping. Set to 0 (the default) when no file is loaded.
    void setDurationMs(std::int64_t ms);
    [[nodiscard]] std::int64_t durationMs() const noexcept override {
        return durationMs_;
    }

    // Piano keyboard column width. The base class's xToMs/msToX
    // offset the time axis past this so paint and click code work
    // in widget-local coordinates while the math treats the time
    // axis as [kbWidth, width()).
    [[nodiscard]] int leftMarginPx() const noexcept override;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    [[nodiscard]] bool hasContent() const noexcept override;
    // Step 6.3 reference-tone gestures: track hover row + tap key,
    // emit the corresponding signals. mouseMoveEvent and leaveEvent
    // call into the base via QWidget::* so the existing drag /
    // zoom-anchor / selection plumbing keeps working.
    void mouseMoveEvent(QMouseEvent* event)   override;
    void leaveEvent   (QEvent*      event)    override;
    void keyPressEvent(QKeyEvent*   event)    override;
    // Additive hook the base calls inside the plain-seek branch
    // (after seekRequested + emptySpaceClicked fire). The piano
    // roll uses this to emit placeNoteRequested(ms, midi) — that
    // turns a click on an empty grid cell into a note placement
    // alongside the seek. WaveformWidget keeps the base's no-op
    // default.
    void onEmptySpaceClick(int xWidget,
                           int yWidget,
                           std::int64_t ms) override;

    // Note hit-test for click-to-select on the piano roll. Returns
    // the topmost note id whose bar contains (x, y), or nullopt if
    // none. "Topmost" is the last in NoteModel's sorted order at
    // the matching row — the same draw order paintNotes uses.
    [[nodiscard]] std::optional<std::int64_t>
        hitNote(int xWidget, int yWidget) const override;

    // Note-edge hit-test (issue #60): a click within
    // kEdgeTolerancePx of a bar's left / right x AND on the bar's
    // row arms a NoteResize drag. Edge zones may extend slightly
    // outside the bar so very short bars are still resizable.
    [[nodiscard]] std::optional<NoteEdgeHit>
        hitNoteEdge(int xWidget, int yWidget) const override;

    // Returns the midi value of the chromatic row at y, or -1 if
    // y is outside the grid. Used both by the click-to-place
    // gesture (decides whether to arm NoteCreate) and by the
    // NoteMove drag (live pitch follow on mouse-y).
    [[nodiscard]] int pixelToMidi(int yWidget) const override;
    [[nodiscard]] int chromaticRowAt(int xWidget,
                                     int yWidget) const override;

    // Piano roll is the note editor (issue #62) — markers /
    // barlines / loop edges / loop bodies all fall through on
    // click so the user can place / select notes anywhere on the
    // grid, including ON TOP of these artifacts. The artifacts
    // still PAINT for visual context (and serve as snap anchors
    // when notes are dragged), they just don't catch clicks.
    [[nodiscard]] bool artifactsClickable() const override { return false; }

    // Piano keyboard click (step 6.3) — emits keyboardKeyPressed
    // with the midi value of the key under the press, kicks off a
    // visual flash on the pressed key. Routed through this
    // override so the base's mousePressEvent stays surface-agnostic.
    void onLeftMarginPress(int xWidget, int yWidget) override;

signals:
    // Fired when the user clicks an empty cell on the chromatic
    // grid. `ms` is the click's source-time; `midi` is the row's
    // MIDI value. MainWindow turns this into a noteModel_->add()
    // call plus undo push, and selects the new note.
    void placeNoteRequested(std::int64_t ms, int midi);

    // Reference-tone gestures (issue #step6.3). MainWindow routes
    // these into audio::ToneSynth.
    //
    // - Clicking a piano key in the keyboard column → fires a
    //   short pulse at that pitch so the user can A/B against the
    //   recording. Always-on regardless of the hover-tone mode.
    void keyboardKeyPressed(int midi);
    // - Moving the mouse over the chromatic grid produces a row
    //   change: this signal fires ONCE per row crossing (de-duped
    //   by the widget). MainWindow dispatches based on the dock's
    //   hover-tone mode (Off / Continuous / On tap).
    void hoverPitchChanged(int midi);
    // - Mouse leaves the staff (or focus elsewhere): MainWindow
    //   stops any continuous tone in flight.
    void hoverPitchEnded();
    // - User pressed T while hovering: fires a pulse at the
    //   current hovered pitch. MainWindow honours this only when
    //   the hover-tone mode is "On tap".
    void hoverTapRequested(int midi);

private:
    // paintEvent's work is split into one helper per visual concern.
    // Each helper assumes `painter` is already targeting this widget
    // and the background has been filled.
    void paintPianoKeyboard(QPainter& painter) const;
    void paintGridBackground(QPainter& painter) const;
    void paintTuneTypeBanner(QPainter& painter) const;
    void paintLoops(QPainter& painter)         const;
    void paintBarlines(QPainter& painter)      const;
    void paintMarkers(QPainter& painter)       const;
    void paintNotes(QPainter& painter)         const;
    void paintSelectedLoopEdges(QPainter& painter) const;
    void paintSecondaryAnchor(QPainter& painter) const;
    void paintCursor(QPainter& painter)        const;

    // ---- Chromatic geometry (step 6.2) -------------------------
    //
    // The y axis is inverted (top of widget = highest pitch = E7,
    // bottom = lowest pitch = G3). Each chromatic semitone gets a
    // fixed-height row. rowYForMidi returns the row's CENTER y.
    [[nodiscard]] int rowYForMidi(int midi) const noexcept;
    [[nodiscard]] int midiForRowY(int y)    const noexcept;
    // Top of the grid in widget-y coords. Below it sit the chromatic
    // rows; above it sits the marker-flag / barline-label banner
    // (top margin shared with markers in the same x slot).
    [[nodiscard]] int gridTopY()    const noexcept;
    [[nodiscard]] int gridBottomY() const noexcept;

    std::int64_t durationMs_ = 0;

    // ---- Reference-tone state (step 6.3) -----------------------
    //
    // Last midi row the mouse was over the chromatic grid, used to
    // de-duplicate hoverPitchChanged signals so the synth doesn't
    // restart on every sub-row mouse pixel. -1 means "not hovering
    // any row" — the next move into a row fires the signal.
    int                       lastHoverMidi_   = -1;
    // Last midi key the user clicked on the piano keyboard, with a
    // QTimer-driven flash so the user sees what they hit. The flash
    // duration is short (~150 ms). nullopt means no flash active.
    std::optional<int>        pressedKeyMidi_;
    // Hit-test the piano keyboard column. Returns the midi value of
    // the key under (x, y), or nullopt if the click isn't on a key
    // (e.g. outside the grid range, or out-of-row).
    [[nodiscard]] std::optional<int>
        hitKeyboardKey(int xWidget, int yWidget) const noexcept;
};

} // namespace fiddler::ui
