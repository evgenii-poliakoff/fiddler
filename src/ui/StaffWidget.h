// StaffWidget — empty 5-line music staff with the current time
// signature, the user-placed barlines, marker overlays, loop
// bands, secondary anchor, and a playhead cursor.
//
// MEMO[refactor]: the artifact-related state, mouse hit-testing,
// and key navigation moved to `ScoreOverlayBase` in #12. StaffWidget
// now contributes:
//
//   * `durationMs_` (the source-time duration; the staff has no
//     waveform overview to read it from)
//   * coord transforms based on `durationMs_`
//   * the staff-specific paint (5 lines, time-sig digits, tune-type
//     label) plus the artifact-overlay paint shared in shape but
//     not in details with WaveformWidget
//
// The widget does not know about Player, the audio engine, or any
// audio decoding. It receives the duration via setDurationMs and
// the playback position via setPositionMs (inherited from base),
// and emits seekRequested(ms) (also inherited) on left-click.
//
// Why source-time-proportional bar widths (and not equal-width bars
// like real engraving): for an empty staff with no notes yet, lining
// the staff up 1:1 with the waveform is more intuitive — clicking a
// bar on the staff lands you at the same x in the waveform, and the
// cursor moves at one rate across both. When notes land in step 6+
// we may switch to a notation-aware layout for the staff alone; we
// can revisit then.

#pragma once

#include "ui/ScoreOverlayBase.h"

#include <cstdint>

class QPaintEvent;
class QPainter;

namespace fiddler::ui {

class StaffWidget : public ScoreOverlayBase {
    Q_OBJECT
public:
    explicit StaffWidget(QWidget* parent = nullptr);
    ~StaffWidget() override;

    // The source-time duration of the loaded file, in milliseconds.
    // Used as the right-hand bound of the source-time → x-pixel
    // mapping. Set to 0 (the default) when no file is loaded.
    void setDurationMs(std::int64_t ms);
    [[nodiscard]] std::int64_t durationMs() const noexcept { return durationMs_; }

    // Coord transforms — overrides ScoreOverlayBase's pure virtuals.
    // Both return 0 when no duration is set.
    [[nodiscard]] std::int64_t xToMs(int x) const noexcept override;
    [[nodiscard]] int          msToX(std::int64_t ms) const noexcept override;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    [[nodiscard]] bool hasContent() const noexcept override;

private:
    // paintEvent's work is split into one helper per visual concern.
    // Each helper assumes `painter` is already targeting this widget
    // and the background has been filled.
    void paintStaffLines(QPainter& painter)    const;
    void paintTimeSignature(QPainter& painter) const;
    void paintLoops(QPainter& painter)         const;
    void paintBarlines(QPainter& painter)      const;
    void paintMarkers(QPainter& painter)       const;
    void paintSelectedLoopEdges(QPainter& painter) const;
    void paintSecondaryAnchor(QPainter& painter) const;
    void paintCursor(QPainter& painter)        const;

    // Pixel y-coordinates of the top and bottom staff lines.
    [[nodiscard]] int staffTopY()    const noexcept;
    [[nodiscard]] int staffBottomY() const noexcept;

    std::int64_t durationMs_ = 0;
};

} // namespace fiddler::ui
