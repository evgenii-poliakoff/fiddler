#include "ui/LoopCountdownWidget.h"

#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QTimer>
#include <QtMath>

#include <algorithm>

namespace fiddler::ui {

namespace {

// Visual proportions. Outer / inner radius are fractions of the
// shorter widget dimension so the ring scales smoothly with the
// containing layout.
constexpr double kOuterRadiusFrac = 0.45;
constexpr double kInnerRadiusFrac = 0.30;

// Tick width in degrees (the angular extent of each tick's
// rectangle). Smaller values look more digital; larger values
// look chunkier. 18° leaves comfortable gaps between ticks at
// 16 ticks (22.5° apart).
constexpr double kTickAngularWidthDeg = 12.0;

// Glow halo: a second pen drawn underneath the bright tick line
// at greater width and lower opacity. Cheap approximation of a
// real bloom filter.
constexpr double kGlowWidthMul = 3.0;
constexpr int    kGlowAlpha    = 90;

// Three-tier tick palette — see
// memory/feedback_progressive_visual_weight.md.
//
// Cyan throughout (same hue family as the marker convention used
// elsewhere in the UI; see WaveformWidget / StaffWidget). The
// three tiers track readiness:
//   * counting          — full bright + glow halo (highest weight)
//   * armed-but-idle    — mid-brightness, no glow
//   * disabled (idle)   — dim & desaturated, no glow
const QColor kTickActiveColor   (140, 230, 250);   // counting
const QColor kTickReadyColor    (100, 160, 180);   // armed-idle
const QColor kTickDisabledColor ( 60,  90, 100);   // not armed
const QColor kBackgroundColor   ( 20,  20,  24);

} // namespace

LoopCountdownWidget::LoopCountdownWidget(QWidget* parent)
    : QWidget(parent)
{
    // MEMO: opaque paint event — we own the background. Without
    // this, Qt draws the parent's background first which can show
    // through during partial repaints.
    setAttribute(Qt::WA_OpaquePaintEvent);

    tickTimer_ = new QTimer(this);
    tickTimer_->setSingleShot(false);
    connect(tickTimer_, &QTimer::timeout,
            this, &LoopCountdownWidget::onTickTimer);
}

LoopCountdownWidget::~LoopCountdownWidget() = default;

void LoopCountdownWidget::start(int totalMs) {
    // No-op for non-positive totals. The "no pause" path
    // (pauseMs == 0) shouldn't trigger a countdown at all;
    // MainWindow gates on pauseMs > 0 but we guard here too.
    if (totalMs <= 0) {
        cancel();
        return;
    }
    // Each tick consumes totalMs / kTickCount. Round to a
    // minimum interval so very fast pauses (pauseMs < 16) still
    // produce a visible cadence; the depletion will outrun the
    // pause but the widget won't crash.
    const int interval = std::max(1, totalMs / kTickCount);
    remainingTicks_    = kTickCount;
    countingDown_      = true;
    tickTimer_->start(interval);
    update();
}

void LoopCountdownWidget::cancel() {
    if (!countingDown_) return;
    tickTimer_->stop();
    countingDown_   = false;
    remainingTicks_ = kTickCount;   // ring snaps back to full
    update();
}

void LoopCountdownWidget::setIdleVisible(bool visible) {
    if (idleVisible_ == visible) return;
    idleVisible_ = visible;
    update();
}

void LoopCountdownWidget::setArmed(bool armed) {
    if (armed_ == armed) return;
    armed_ = armed;
    update();
}

void LoopCountdownWidget::onTickTimer() {
    if (remainingTicks_ <= 0) {
        // Defensive — should be stopped already.
        cancel();
        return;
    }
    --remainingTicks_;
    emit tickConsumed(remainingTicks_);
    update();
    if (remainingTicks_ == 0) {
        tickTimer_->stop();
        countingDown_ = false;
        emit finished();
    }
}

QSize LoopCountdownWidget::sizeHint()        const { return QSize(96, 96); }
QSize LoopCountdownWidget::minimumSizeHint() const { return QSize(48, 48); }

void LoopCountdownWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackgroundColor);

    // Skip the ring entirely when idle visibility is off and we
    // aren't counting down — just leave a blank dark square so
    // the dock's property page doesn't show an empty affordance
    // when no loop is selected.
    if (!countingDown_ && !idleVisible_) return;

    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPointF centre(width() / 2.0, height() / 2.0);
    const double  shorter = std::min(width(), height());
    const double  rOuter  = shorter * kOuterRadiusFrac;
    const double  rInner  = shorter * kInnerRadiusFrac;

    // Three-tier palette:
    //   counting        → kTickActiveColor + glow
    //   armed, idle     → kTickReadyColor, no glow
    //   not armed, idle → kTickDisabledColor, no glow
    QColor tickCol;
    bool   drawGlow;
    if (countingDown_) {
        tickCol  = kTickActiveColor;
        drawGlow = true;
    } else if (armed_) {
        tickCol  = kTickReadyColor;
        drawGlow = false;
    } else {
        tickCol  = kTickDisabledColor;
        drawGlow = false;
    }

    const double tickPenWidth = std::max(1.5,
        (rOuter - rInner) * 0.18);
    const double glowPenWidth = tickPenWidth * kGlowWidthMul;

    QColor glowCol = tickCol;
    glowCol.setAlpha(kGlowAlpha);

    for (int i = 0; i < kTickCount; ++i) {
        // MEMO: clockwise from 12 o'clock means tick i sits at
        // angle (-90 + i * step) degrees in screen coordinates
        // (Qt's y axis grows downward). i=0 is at the top.
        const double step = 360.0 / kTickCount;
        const double angleDeg = -90.0 + i * step;
        const double angleRad = qDegreesToRadians(angleDeg);

        // Visibility: counting down → only the first
        // remainingTicks_ ticks (still in the future) are bright;
        // the rest are gone. Idle full ring → all visible.
        const bool visible = countingDown_
            ? (i < remainingTicks_)
            : true;
        if (!visible) continue;

        const double cosA = std::cos(angleRad);
        const double sinA = std::sin(angleRad);
        const QPointF inner(centre.x() + rInner * cosA,
                            centre.y() + rInner * sinA);
        const QPointF outer(centre.x() + rOuter * cosA,
                            centre.y() + rOuter * sinA);

        // Glow under-pass — only in the active (counting) tier.
        // MEMO: per the progressive-visual-weight rule, idle tiers
        // are static and unhaloed; the glow is reserved for the
        // "something is happening NOW" state.
        if (drawGlow) {
            QPen glowPen(glowCol);
            glowPen.setWidthF(glowPenWidth);
            glowPen.setCapStyle(Qt::RoundCap);
            painter.setPen(glowPen);
            painter.drawLine(inner, outer);
        }

        // Tick.
        QPen tickPen(tickCol);
        tickPen.setWidthF(tickPenWidth);
        tickPen.setCapStyle(Qt::RoundCap);
        painter.setPen(tickPen);
        painter.drawLine(inner, outer);

        // Suppress -Wunused for the tick angular width constant
        // when the user later switches to drawing tick *bars*
        // (filled QRectF rotated about centre) instead of lines.
        // For now, line + RoundCap visually covers the angular
        // width sufficiently.
        (void)kTickAngularWidthDeg;
    }
}

} // namespace fiddler::ui
