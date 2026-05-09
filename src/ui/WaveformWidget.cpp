#include "ui/WaveformWidget.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QString>

#include <algorithm>
#include <chrono>
#include <limits>

namespace fiddler::ui {

namespace {

constexpr int kLanePaddingPx        = 2;

// Marker label flag at the top of the widget where names sit.
constexpr int kMarkerFlagHeightPx     = 14;
constexpr int kMarkerFlagPaddingPx    = 4;
constexpr int kMarkerFlagFontPointSz  = 8;
constexpr int kMarkerFlagMaxWidthPx   = 120;

// Loop bands — translucent, full-height. Selected loop renders with
// higher alpha than unselected so a glance at the waveform tells you
// which loop the dock's property page is editing. Colors chosen to
// be visibly distinct from the cyan markers and yellow barlines: a
// soft sage green that doesn't fight peaks-blue.
//
// MEMO: the loop label sits in the BOTTOM of the band on purpose —
// putting it at the top would clash with the marker flag row, since
// loops and markers commonly share start positions when the user
// converts a loop's "starts here" into a marker for orientation.
constexpr int kLoopBandAlphaUnselected = 35;
constexpr int kLoopBandAlphaSelected   = 90;
constexpr int kLoopLabelHeightPx       = 14;
constexpr int kLoopLabelPaddingPx      = 4;
constexpr int kLoopLabelFontPointSz    = 8;
constexpr int kLoopLabelMaxWidthPx     = 120;

} // namespace

WaveformWidget::WaveformWidget(QWidget* parent) : ScoreOverlayBase(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
}

WaveformWidget::~WaveformWidget() = default;

void WaveformWidget::setOverview(
    std::shared_ptr<const audio::WaveformOverview> ov)
{
    overview_ = std::move(ov);
    update();
}

bool WaveformWidget::hasContent() const noexcept {
    return overview_ != nullptr;
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

    // Snapshot the base's selection / artifact state. The paint code
    // is read-only — going through getters keeps the base's members
    // private without growing a friend declaration.
    const auto barModel  = barlineModel();
    const auto markModel = markerModel();
    const auto lpModel   = loopModel();
    const auto selBar    = selectedBarline();
    const auto selMark   = selectedMarkerId();
    const auto selLoop   = selectedLoopId();
    const auto secAnchor = secondaryAnchorMs();
    const auto posMs     = positionMs();

    // Loop bands — drawn first so barlines, markers, and the cursor
    // all paint on top.
    if (lpModel) {
        const auto loops = lpModel->loops();
        QFont labelFont = painter.font();
        labelFont.setPointSize(kLoopLabelFontPointSz);
        labelFont.setBold(true);
        const QFontMetrics fm(labelFont);

        for (const auto& l : loops) {
            const int xStart = msToX(l.startMs);
            const int xEnd   = msToX(l.endMs);
            if (xEnd <= 0 || xStart >= width()) continue;

            const int xLeft  = std::max(0, xStart);
            const int xRight = std::min(width(), xEnd);
            const int bandW  = std::max(1, xRight - xLeft);

            const bool selected = (selLoop == l.id);
            const int  alpha    = selected
                ? kLoopBandAlphaSelected
                : kLoopBandAlphaUnselected;

            const QColor bandCol(120, 200, 140, alpha);
            painter.fillRect(QRect(xLeft, 0, bandW, height()), bandCol);

            const QColor edgeCol(140, 220, 160,
                                 std::min(255, alpha + 60));
            painter.setPen(QPen(edgeCol, selected ? 2.0 : 1.0));
            painter.drawLine(xLeft,      0, xLeft,      height());
            painter.drawLine(xRight - 1, 0, xRight - 1, height());

            painter.setFont(labelFont);
            const int rawTextWidth = fm.horizontalAdvance(l.name);
            const int textWidth =
                std::min(rawTextWidth, kLoopLabelMaxWidthPx
                         - 2 * kLoopLabelPaddingPx);
            const int labelW = std::min(bandW,
                                        textWidth + 2 * kLoopLabelPaddingPx);
            const QRect labelRect(xLeft, height() - kLoopLabelHeightPx,
                                  labelW, kLoopLabelHeightPx);
            painter.fillRect(labelRect, bandCol.darker(180));
            painter.setPen(QColor(220, 240, 220));
            painter.drawText(labelRect.adjusted(kLoopLabelPaddingPx, 0,
                                                -kLoopLabelPaddingPx, 0),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             fm.elidedText(l.name, Qt::ElideRight,
                                           textWidth));
        }
    }

    // MEMO: when the secondary anchor lands on top of an existing
    // artifact (the common case — Ctrl+click on an existing tick),
    // paint the artifact ITSELF dashed below; the standalone tick at
    // the bottom of the paint pipeline is only drawn when there's no
    // overlap. The underlying solid line would otherwise fill the
    // dash gaps.
    bool secondaryOnArtifact = false;
    if (secAnchor.has_value()) {
        if (barModel) {
            for (auto barMs : barModel->barlines()) {
                if (barMs == *secAnchor) { secondaryOnArtifact = true; break; }
            }
        }
        if (!secondaryOnArtifact && markModel) {
            for (const auto& m : markModel->markers()) {
                if (m.sourceMs == *secAnchor) { secondaryOnArtifact = true; break; }
            }
        }
    }

    // Barline ticks — drawn between peaks and the cursor so the
    // playhead always wins z-order.
    if (barModel) {
        const auto bars = barModel->barlines();
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const int x = msToX(bars[i]);
            if (x < 0 || x >= width()) continue;
            const bool selected = (selBar == i);
            const bool isAnchor = secAnchor.has_value()
                              && bars[i] == *secAnchor;
            QPen pen;
            if (isAnchor) {
                pen = QPen(QColor(255, 220, 130), 2.0);
                pen.setStyle(Qt::DashLine);
            } else {
                pen = QPen(
                    selected ? QColor(255, 200, 90) : QColor(210, 170, 60),
                    selected ? 2.0 : 1.0);
            }
            painter.setPen(pen);
            painter.drawLine(x, 0, x, height());
        }
    }

    // Marker ticks + label flags. Drawn above barlines so the
    // labelled annotations sit visually on top, but below the cursor
    // so the playhead still wins z-order.
    if (markModel) {
        const auto markers = markModel->markers();
        QFont flagFont = painter.font();
        flagFont.setPointSize(kMarkerFlagFontPointSz);
        flagFont.setBold(true);
        painter.setFont(flagFont);
        const QFontMetrics fm(flagFont);

        for (const auto& m : markers) {
            const int x = msToX(m.sourceMs);
            if (x < 0 || x >= width()) continue;
            const bool selected = (selMark == m.id);
            const bool isAnchor = secAnchor.has_value()
                              && m.sourceMs == *secAnchor;
            const QColor lineCol = selected
                ? QColor(140, 230, 250)
                : QColor(100, 200, 220);

            // Marker-as-secondary gets a brighter, dashed, thicker
            // tick; the label flag stays solid so the marker's name
            // remains legible.
            QPen tickPen;
            if (isAnchor) {
                tickPen = QPen(QColor(160, 240, 255), 2.0);
                tickPen.setStyle(Qt::DashLine);
            } else {
                tickPen = QPen(lineCol, selected ? 2.0 : 1.0);
            }
            painter.setPen(tickPen);
            painter.drawLine(x, 0, x, height());

            const int rawTextWidth = fm.horizontalAdvance(m.name);
            const int textWidth =
                std::min(rawTextWidth, kMarkerFlagMaxWidthPx
                         - 2 * kMarkerFlagPaddingPx);
            const int flagWidth = textWidth + 2 * kMarkerFlagPaddingPx;
            const QRect flagRect(x, 0, flagWidth, kMarkerFlagHeightPx);
            painter.fillRect(flagRect, lineCol.darker(140));
            painter.setPen(QColor(20, 20, 24));
            painter.drawText(flagRect.adjusted(kMarkerFlagPaddingPx, 0,
                                              -kMarkerFlagPaddingPx, 0),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             fm.elidedText(m.name, Qt::ElideRight,
                                           textWidth));
        }
    }

    // Playhead cursor — drawn before the secondary anchor tick so
    // the dashed indicator can pierce through it when both share an
    // x (which happens immediately after a tap-place because the
    // cursor seeks to the new artifact).
    const int cursorX = msToX(posMs);
    if (cursorX >= 0 && cursorX < width()) {
        painter.setPen(QPen(QColor(255, 80, 80), 2));
        painter.drawLine(cursorX, 0, cursorX, height());
    }

    // Secondary anchor indicator — painted LAST so it pierces the
    // cursor's red when they overlap. Skip when an artifact already
    // shows the dashing AND the cursor isn't on top (otherwise the
    // standalone tick would just overdraw an identical dash).
    if (secAnchor.has_value()) {
        const int sx = msToX(*secAnchor);
        if (sx >= 0 && sx < width()) {
            const bool cursorOverlap = (sx == cursorX) && cursorX >= 0
                                    && cursorX < width();
            if (!secondaryOnArtifact || cursorOverlap) {
                QColor col(255, 200, 90);
                if (markModel) {
                    for (const auto& m : markModel->markers()) {
                        if (m.sourceMs == *secAnchor) {
                            col = QColor(160, 240, 255);
                            break;
                        }
                    }
                }
                QPen pen(col, 2.0);
                pen.setStyle(Qt::DashLine);
                painter.setPen(pen);
                painter.drawLine(sx, 0, sx, height());
            }
        }
    }
}

} // namespace fiddler::ui
