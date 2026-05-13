// WaveformWidget — paints a WaveformOverview's peaks plus a playhead
// cursor, the artifact overlays (barlines / markers / loops), and
// the secondary-anchor indicator. Decoupled from Player by design:
// the widget consumes a `shared_ptr<const WaveformOverview>` and
// emits `seekRequested(ms)` on left-click; the parent (MainWindow)
// wires the slot back to `Player::seek`.
//
// MEMO[refactor]: most of what used to live here moved to
// `ScoreOverlayBase` in #12 — selection state, mutual exclusion,
// secondary anchor, mouse press hit-testing, keyboard navigation,
// model wiring. WaveformWidget now contributes just the bits that
// genuinely differ from StaffWidget: the overview pointer, the
// peaks-based paint, and coord transforms based on the overview's
// duration.
//
// Source-time milliseconds is the universal coordinate. xToMs()
// and msToX() are the public coord transforms; future overlays
// drop in against the same axis without reimplementing the math.

#pragma once

#include "audio/WaveformOverview.h"
#include "ui/ScoreOverlayBase.h"

#include <cstdint>
#include <memory>

class QPaintEvent;

namespace fiddler::ui {

class WaveformWidget : public ScoreOverlayBase {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget* parent = nullptr);
    ~WaveformWidget() override;

    void setOverview(std::shared_ptr<const audio::WaveformOverview> ov);
    [[nodiscard]] std::shared_ptr<const audio::WaveformOverview>
        overview() const noexcept { return overview_; }

    // Source-time duration in milliseconds — overrides
    // ScoreOverlayBase's pure virtual. Read from the attached
    // WaveformOverview; returns 0 when no overview is set. The
    // base class's xToMs/msToX use this together with the viewport
    // state to do the coord math.
    [[nodiscard]] std::int64_t durationMs() const noexcept override;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    [[nodiscard]] bool hasContent() const noexcept override;

    // Loop-create drag is a waveform-only gesture (issue #62). On
    // the staff the empty-space press arms NoteCreate instead.
    [[nodiscard]] bool armsLoopCreate(int xWidget,
                                      int yWidget) const override;

private:
    std::shared_ptr<const audio::WaveformOverview> overview_;
};

} // namespace fiddler::ui
