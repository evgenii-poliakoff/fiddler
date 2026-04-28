#include "ui/WaveformWidget.h"

#include "score/BarlineModel.h"

#include <QColor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <chrono>
#include <limits>

namespace fiddler::ui {

namespace {
constexpr int kLanePaddingPx        = 2;
// Click-to-select tolerance: if the click x is within this many
// pixels of a barline tick, the barline is selected (and the audio
// seeks to its exact source-ms). Larger than 1 px because hitting
// a single-pixel-wide tick precisely is annoying; small enough not
// to trap clicks meant for plain seeking.
constexpr int kBarlineHitTolerancePx = 5;
} // namespace

WaveformWidget::WaveformWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    // StrongFocus so the widget can receive arrow / Esc / Del keys
    // after a click gives it focus.
    setFocusPolicy(Qt::StrongFocus);
}

WaveformWidget::~WaveformWidget() = default;

void WaveformWidget::setOverview(
    std::shared_ptr<const audio::WaveformOverview> ov)
{
    overview_ = std::move(ov);
    update();
}

void WaveformWidget::setBarlineModel(
    std::shared_ptr<const score::BarlineModel> model)
{
    if (barlineModel_) {
        disconnect(barlineModel_.get(), nullptr, this, nullptr);
    }
    barlineModel_ = std::move(model);
    if (barlineModel_) {
        connect(barlineModel_.get(), &score::BarlineModel::changed,
                this, &WaveformWidget::onBarlineModelChanged);
    }
    if (selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    update();
}

void WaveformWidget::setPositionMs(std::int64_t ms) {
    if (positionMs_ == ms) return;
    positionMs_ = ms;
    update();
}

void WaveformWidget::setSelectedBarline(std::optional<std::size_t> index) {
    if (index.has_value() && barlineModel_
        && *index >= barlineModel_->size()) {
        index = std::nullopt;
    }
    if (selectedBarline_ == index) return;
    selectedBarline_ = index;
    update();
    emit barlineSelectionChanged(selectedBarline_);
}

void WaveformWidget::onBarlineModelChanged() {
    // The selection's index might no longer be valid (entry removed,
    // model cleared). Drop it if so.
    if (selectedBarline_.has_value() && barlineModel_
        && *selectedBarline_ >= barlineModel_->size()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
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

    // Barline ticks — drawn between peaks and the cursor so the
    // playhead always wins z-order.
    if (barlineModel_) {
        const auto bars = barlineModel_->barlines();
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const int x = msToX(bars[i]);
            if (x < 0 || x >= width()) continue;
            const bool selected = (selectedBarline_ == i);
            painter.setPen(QPen(
                selected ? QColor(255, 200, 90) : QColor(210, 170, 60),
                selected ? 2.0 : 1.0));
            painter.drawLine(x, 0, x, height());
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
    if (event->button() != Qt::LeftButton || !overview_) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    const int x  = event->pos().x();
    const auto ms = xToMs(x);

    // Hit-test a nearby barline. Tolerance is in source-ms, scaled
    // from kBarlineHitTolerancePx using the current widget's
    // ms-per-pixel rate.
    std::optional<std::size_t> hit;
    if (barlineModel_ && barlineModel_->size() > 0
        && overview_ && width() > 0)
    {
        const auto durationMs = overview_->duration().count();
        if (durationMs > 0) {
            const auto tolMs = static_cast<std::int64_t>(
                kBarlineHitTolerancePx) * durationMs / width();
            hit = barlineModel_->nearest(ms, tolMs);
        }
    }

    if (hit.has_value()) {
        // Update selection (if it changed) and seek to the barline's
        // exact ms — not the click ms — so the cursor lands on the
        // tick, not just nearby.
        if (selectedBarline_ != hit) {
            selectedBarline_ = hit;
            update();
            emit barlineSelectionChanged(selectedBarline_);
        }
        emit seekRequested(barlineModel_->barlines()[*hit]);
    } else {
        if (selectedBarline_.has_value()) {
            selectedBarline_.reset();
            update();
            emit barlineSelectionChanged(selectedBarline_);
        }
        emit seekRequested(ms);
    }
    event->accept();
}

void WaveformWidget::keyPressEvent(QKeyEvent* event) {
    if (!barlineModel_) {
        QWidget::keyPressEvent(event);
        return;
    }
    const auto count = barlineModel_->size();

    switch (event->key()) {
    case Qt::Key_Left:
        if (selectedBarline_ && *selectedBarline_ > 0) {
            setSelectedBarline(*selectedBarline_ - 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        }
        event->accept();
        return;
    case Qt::Key_Right:
        if (selectedBarline_ && *selectedBarline_ + 1 < count) {
            setSelectedBarline(*selectedBarline_ + 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        }
        event->accept();
        return;
    case Qt::Key_Escape:
        if (selectedBarline_.has_value()) {
            setSelectedBarline(std::nullopt);
        }
        event->accept();
        return;
    case Qt::Key_Delete:
        if (selectedBarline_.has_value()) {
            emit barlineDeleteRequested(*selectedBarline_);
            // Don't reset selection here — onBarlineModelChanged()
            // will handle it once the model actually drops the entry.
        }
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

} // namespace fiddler::ui
