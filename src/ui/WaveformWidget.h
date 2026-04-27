// WaveformWidget — paints a WaveformOverview's peaks plus a playhead
// cursor, and turns user clicks into seek requests.
//
// Decoupled from Player by design: the widget consumes a
// shared_ptr<const WaveformOverview> and emits seekRequested(ms);
// the parent (MainWindow) wires the slot back to Player::seek. This
// shape lets future views (mini-strip, second waveform pane, the
// staff overlay landing in step 5) drop in against the same data
// without touching Player or each other.
//
// Source-time milliseconds is the universal coordinate. xToMs() and
// msToX() are public so step 5's overlay code can map between pixel
// columns and source-time positions without reimplementing the math.

#pragma once

#include "audio/WaveformOverview.h"

#include <QWidget>

#include <cstdint>
#include <memory>

class QMouseEvent;
class QPaintEvent;

namespace fiddler::ui {

class WaveformWidget : public QWidget {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget* parent = nullptr);
    ~WaveformWidget() override;

    void setOverview(std::shared_ptr<const audio::WaveformOverview> ov);
    [[nodiscard]] std::shared_ptr<const audio::WaveformOverview>
        overview() const noexcept { return overview_; }

    [[nodiscard]] std::int64_t positionMs() const noexcept { return positionMs_; }

    // Coordinate transforms — public so overlays can map pixels to
    // source time and back. Both clamp to the widget's current bounds
    // and the overview's duration; calling them when no overview is
    // set returns 0.
    [[nodiscard]] std::int64_t xToMs(int x) const noexcept;
    [[nodiscard]] int          msToX(std::int64_t ms) const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

public slots:
    void setPositionMs(std::int64_t ms);

signals:
    void seekRequested(std::int64_t ms);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    std::shared_ptr<const audio::WaveformOverview> overview_;
    std::int64_t                                   positionMs_ = 0;
};

} // namespace fiddler::ui
