#include "ui/WaveformWidget.h"

#include <QColor>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <chrono>
#include <limits>

namespace fiddler::ui {

namespace {
constexpr int kLanePaddingPx = 2;
} // namespace

WaveformWidget::WaveformWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
}

WaveformWidget::~WaveformWidget() = default;

void WaveformWidget::setOverview(
    std::shared_ptr<const audio::WaveformOverview> ov)
{
    overview_ = std::move(ov);
    update();
}

void WaveformWidget::setPositionMs(std::int64_t ms) {
    if (positionMs_ == ms) return;
    positionMs_ = ms;
    update();
}

std::int64_t WaveformWidget::xToMs(int x) const noexcept {
    if (!overview_) return 0;
    const int w = width();
    if (w <= 0) return 0;
    const std::int64_t durationMs = overview_->duration().count();
    if (durationMs <= 0) return 0;
    const std::int64_t ms =
        static_cast<std::int64_t>(x) * durationMs / w;
    return std::clamp<std::int64_t>(ms, 0, durationMs);
}

int WaveformWidget::msToX(std::int64_t ms) const noexcept {
    if (!overview_) return 0;
    const int w = width();
    if (w <= 0) return 0;
    const std::int64_t durationMs = overview_->duration().count();
    if (durationMs <= 0) return 0;
    const std::int64_t x = ms * static_cast<std::int64_t>(w) / durationMs;
    return static_cast<int>(std::clamp<std::int64_t>(x, 0, w - 1));
}

QSize WaveformWidget::sizeHint() const        { return QSize(800, 120); }
QSize WaveformWidget::minimumSizeHint() const { return QSize(120, 40);  }

void WaveformWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(20, 20, 24));

    if (!overview_ || overview_->bucketCount() == 0
        || overview_->channels() <= 0)
    {
        painter.setPen(QColor(60, 60, 70));
        painter.drawLine(0, height() / 2, width(), height() / 2);
        painter.setPen(QColor(120, 120, 140));
        painter.drawText(rect(), Qt::AlignCenter, tr("(no audio loaded)"));
        return;
    }

    const int channels   = overview_->channels();
    const int laneHeight = std::max(1, height() / channels);
    const QPen wavePen(QColor(120, 200, 255));
    const QPen separatorPen(QColor(40, 40, 48));

    for (int c = 0; c < channels; ++c) {
        const int laneTop    = c * laneHeight;
        const int laneCenter = laneTop + laneHeight / 2;
        const int laneScale  = std::max(1, laneHeight / 2 - kLanePaddingPx);

        if (c > 0) {
            painter.setPen(separatorPen);
            painter.drawLine(0, laneTop, width(), laneTop);
        }

        painter.setPen(wavePen);
        const auto channelPeaks = overview_->peaks(c);

        for (int x = 0; x < width(); ++x) {
            // Combine peaks across every bucket that maps to this column.
            const auto bStart = overview_->bucketAtMs(
                std::chrono::milliseconds{xToMs(x)});
            const auto bEnd = overview_->bucketAtMs(
                std::chrono::milliseconds{xToMs(x + 1)});
            float pmin =  std::numeric_limits<float>::infinity();
            float pmax = -std::numeric_limits<float>::infinity();
            for (auto b = bStart; b <= bEnd && b < channelPeaks.size(); ++b) {
                pmin = std::min(pmin, channelPeaks[b].min);
                pmax = std::max(pmax, channelPeaks[b].max);
            }
            if (pmin > pmax) continue; // empty range — skip
            const int yTop = laneCenter -
                static_cast<int>(std::clamp(pmax, -1.0f, 1.0f) * laneScale);
            const int yBot = laneCenter -
                static_cast<int>(std::clamp(pmin, -1.0f, 1.0f) * laneScale);
            painter.drawLine(x, yTop, x, yBot);
        }
    }

    // Playhead cursor.
    const int cursorX = msToX(positionMs_);
    if (cursorX >= 0 && cursorX < width()) {
        painter.setPen(QPen(QColor(255, 80, 80), 2));
        painter.drawLine(cursorX, 0, cursorX, height());
    }
}

void WaveformWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && overview_) {
        emit seekRequested(xToMs(event->pos().x()));
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

} // namespace fiddler::ui
