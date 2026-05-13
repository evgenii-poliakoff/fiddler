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

signals:
    // Fired when the user clicks an empty cell on the chromatic
    // grid. `ms` is the click's source-time; `midi` is the row's
    // MIDI value. MainWindow turns this into a noteModel_->add()
    // call plus undo push, and selects the new note.
    void placeNoteRequested(std::int64_t ms, int midi);

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
};

} // namespace fiddler::ui
