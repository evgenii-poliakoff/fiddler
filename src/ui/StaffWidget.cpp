#include "ui/StaffWidget.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"

#include <QFont>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QString>

#include <algorithm>

namespace fiddler::ui {

namespace {

// Visual layout constants. Pulled out as named constants so the
// numbers in the paint code mean something at a glance.
constexpr int kStaffLineCount      = 5;
constexpr int kStaffSpacingPx      = 8;     // gap between adjacent staff lines
constexpr int kStaffTopMarginPx    = 18;    // room above for tune-type label + marker flags
constexpr int kStaffBottomMarginPx = 12;    // unused for now; reserved for step 6 ledger lines

constexpr int kTimeSigLeftMarginPx = 6;
constexpr int kTimeSigPointSize    = 14;

// Marker label flag — same shape as WaveformWidget's, sits in the
// top margin band above the staff lines.
constexpr int kMarkerFlagHeightPx     = 12;
constexpr int kMarkerFlagPaddingPx    = 4;
constexpr int kMarkerFlagFontPointSz  = 8;
constexpr int kMarkerFlagMaxWidthPx   = 120;

// Loop band visuals — same palette and rules as WaveformWidget.
// Comment intentionally short here; canonical rationale lives in
// WaveformWidget.cpp's matching block.
constexpr int kLoopBandAlphaUnselected = 35;
constexpr int kLoopBandAlphaSelected   = 90;
constexpr int kLoopLabelHeightPx       = 12;
constexpr int kLoopLabelPaddingPx      = 4;
constexpr int kLoopLabelFontPointSz    = 8;
constexpr int kLoopLabelMaxWidthPx     = 120;

} // namespace

// ---- construction --------------------------------------------------------

StaffWidget::StaffWidget(QWidget* parent) : ScoreOverlayBase(parent) {
    // The widget paints its own background opaquely; tell Qt so it
    // skips the default-clear pass before paintEvent.
    setAttribute(Qt::WA_OpaquePaintEvent);
}

StaffWidget::~StaffWidget() = default;

// ---- public setters ------------------------------------------------------

void StaffWidget::setDurationMs(std::int64_t ms) {
    if (durationMs_ == ms) return;
    durationMs_ = ms;
    update();   // schedules a repaint; Qt batches and runs it on the event loop
}

bool StaffWidget::hasContent() const noexcept {
    return durationMs_ > 0;
}

// ---- coordinate transforms ----------------------------------------------

QSize StaffWidget::sizeHint()        const { return QSize(800, 100); }
QSize StaffWidget::minimumSizeHint() const { return QSize(120,  60); }

int StaffWidget::staffTopY()    const noexcept { return kStaffTopMarginPx; }
int StaffWidget::staffBottomY() const noexcept {
    return staffTopY() + (kStaffLineCount - 1) * kStaffSpacingPx;
}

// ---- painting ------------------------------------------------------------

void StaffWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(20, 20, 24));   // matches WaveformWidget

    // One paint helper per visual concern keeps each method short
    // and easy to follow. Order matters: things drawn later sit on
    // top, so the cursor is last. Loops paint first so the
    // translucent bands sit at the lowest z-order.
    paintLoops(painter);
    paintStaffLines(painter);
    paintTimeSignature(painter);
    paintBarlines(painter);
    paintMarkers(painter);
    // Selected loop's edges re-drawn ON TOP so the user can see
    // (and drag) the edge even when a marker tick or barline sits
    // at exactly the same x. Mirrors the hit-test rule in
    // ScoreOverlayBase::mousePressEvent. Issue #11.
    paintSelectedLoopEdges(painter);
    // Cursor must paint BEFORE the secondary anchor's dashed
    // indicator so that, when both are at the same x (e.g. right
    // after a tap-place when the cursor seeks to the new artifact),
    // the dashing pierces through the cursor. Otherwise the cursor
    // would cover the artifact-as-dashed paint and the user would
    // only see the red cursor at that x. See WaveformWidget for the
    // canonical comment.
    paintCursor(painter);
    paintSecondaryAnchor(painter);
    // Zoom-anchor guide (#49) — painted last so it sits on top of
    // every other overlay. No-op when Ctrl isn't held.
    paintZoomAnchorGuide(painter);
    (void)kStaffBottomMarginPx;   // reserved for step 6 ledger lines
}

void StaffWidget::paintStaffLines(QPainter& painter) const {
    painter.setPen(QPen(QColor(180, 180, 180), 1.0));
    const int top = staffTopY();
    for (int i = 0; i < kStaffLineCount; ++i) {
        const int y = top + i * kStaffSpacingPx;
        painter.drawLine(0, y, width(), y);
    }
}

void StaffWidget::paintTimeSignature(QPainter& painter) const {
    const auto barModel = barlineModel();
    if (!barModel) return;
    const auto ts = barModel->timeSignature();

    // Numerator + denominator stacked, left of the first barline.
    // A future engraving pass would use proper SMuFL glyphs from a
    // music font; this is good enough for an empty-staff prototype.
    QFont digitFont = painter.font();
    digitFont.setPointSize(kTimeSigPointSize);
    digitFont.setBold(true);
    painter.setFont(digitFont);
    painter.setPen(QColor(230, 230, 230));

    const QFontMetrics fm(digitFont);
    const int top    = staffTopY();
    const int bottom = staffBottomY();
    const int upperCentreY = top    + (bottom - top) / 4;
    const int lowerCentreY = bottom - (bottom - top) / 4;
    const int leftX        = kTimeSigLeftMarginPx;

    painter.drawText(leftX, upperCentreY + fm.ascent() / 2,
                     QString::number(ts.numerator));
    painter.drawText(leftX, lowerCentreY + fm.ascent() / 2,
                     QString::number(ts.denominator));

    // Optional tune-type label above the staff (handbook hint —
    // "Reel", "Jig", etc. — see project_handbook_for_self_taught.md).
    if (!ts.tuneType.isEmpty()) {
        QFont labelFont = painter.font();
        labelFont.setPointSize(10);
        labelFont.setBold(false);
        painter.setFont(labelFont);
        painter.drawText(leftX, top - 4, ts.tuneType);
    }
}

void StaffWidget::paintLoops(QPainter& painter) const {
    const auto lpModel = loopModel();
    if (!lpModel) return;
    const auto loops   = lpModel->loops();
    const auto selLoop = selectedLoopId();

    QFont labelFont = painter.font();
    labelFont.setPointSize(kLoopLabelFontPointSz);
    labelFont.setBold(true);
    const QFontMetrics fm(labelFont);

    for (const auto& l : loops) {
        // MEMO[#22]: paint via effective range so a live edge drag
        // glides here too, not just on the widget being touched.
        const auto [effStart, effEnd] =
            effectiveLoopRange(l.id, l.startMs, l.endMs);
        const int xStart = msToX(effStart);
        const int xEnd   = msToX(effEnd);
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
                         fm.elidedText(l.name, Qt::ElideRight, textWidth));
    }
}

void StaffWidget::paintBarlines(QPainter& painter) const {
    const auto barModel = barlineModel();
    if (!barModel) return;

    const auto bars       = barModel->barlines();
    const auto selBar     = selectedBarline();
    const auto secAnchor  = secondaryAnchorMs();
    const int  topY       = staffTopY();
    const int  bottomY    = staffBottomY();
    const QPen normalPen{ QColor(210, 170, 60), 1.0 };
    const QPen selectedPen{ QColor(255, 200, 90), 2.0 };

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const int x = msToX(bars[i]);
        if (x < 0 || x >= width()) continue;

        const bool selected = (selBar == i);
        const bool isAnchor = secAnchor.has_value()
                          && bars[i] == *secAnchor;
        // MEMO: when this barline IS the secondary anchor, render it
        // dashed instead of drawing a separate dashed tick on top —
        // overlapping a solid line would mask the dash gaps. See
        // WaveformWidget paintEvent for the canonical comment.
        if (isAnchor) {
            QPen pen(QColor(255, 220, 130), 2.0);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
        } else {
            painter.setPen(selected ? selectedPen : normalPen);
        }
        painter.drawLine(x, topY, x, bottomY);
    }
}

void StaffWidget::paintMarkers(QPainter& painter) const {
    const auto markModel = markerModel();
    if (!markModel) return;
    const auto markers   = markModel->markers();
    const auto selMark   = selectedMarkerId();
    const auto secAnchor = secondaryAnchorMs();

    QFont flagFont = painter.font();
    flagFont.setPointSize(kMarkerFlagFontPointSz);
    flagFont.setBold(true);
    painter.setFont(flagFont);
    const QFontMetrics fm(flagFont);

    for (const auto& m : markers) {
        // MEMO[#22]: paint via effective ms so a live drag glides.
        const auto effMs = effectiveMarkerMs(m.id, m.sourceMs);
        const int x = msToX(effMs);
        if (x < 0 || x >= width()) continue;
        const bool selected = (selMark == m.id);
        const bool isAnchor = secAnchor.has_value()
                          && effMs == *secAnchor;
        const QColor lineCol = selected
            ? QColor(140, 230, 250)
            : QColor(100, 200, 220);

        // Marker-as-secondary gets a brighter, dashed, thicker tick;
        // the label flag stays solid so the marker's name remains
        // legible.
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
                         fm.elidedText(m.name, Qt::ElideRight, textWidth));
    }
}

void StaffWidget::paintSecondaryAnchor(QPainter& painter) const {
    const auto secAnchor = secondaryAnchorMs();
    if (!secAnchor.has_value()) return;
    const int sx = msToX(*secAnchor);
    if (sx < 0 || sx >= width()) return;

    // Detect whether an artifact already sits at the secondary's ms
    // (in which case paintBarlines / paintMarkers already painted
    // the artifact's tick in dashed style — a second tick would
    // overdraw with the same pattern, harmless but wasteful) and
    // whether the cursor is at the same x (in which case we MUST
    // paint here so the dashes pierce through the cursor's red).
    const bool cursorOverlap = (msToX(positionMs()) == sx);
    bool secondaryOnArtifact = false;
    QColor col(255, 200, 90);
    const auto barModel  = barlineModel();
    const auto markModel = markerModel();
    if (barModel) {
        for (auto barMs : barModel->barlines()) {
            if (barMs == *secAnchor) {
                secondaryOnArtifact = true;
                break;
            }
        }
    }
    if (markModel) {
        for (const auto& m : markModel->markers()) {
            if (m.sourceMs == *secAnchor) {
                secondaryOnArtifact = true;
                col = QColor(160, 240, 255);   // marker hue
                break;
            }
        }
    }
    if (secondaryOnArtifact && !cursorOverlap) return;

    QPen pen(col, 2.0);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.drawLine(sx, 0, sx, height());
}

void StaffWidget::paintCursor(QPainter& painter) const {
    const int cursorX = msToX(positionMs());
    if (cursorX < 0 || cursorX >= width()) return;
    painter.setPen(QPen(QColor(255, 80, 80), 2.0));
    painter.drawLine(cursorX, 0, cursorX, height());
}

void StaffWidget::paintSelectedLoopEdges(QPainter& painter) const {
    const auto lpModel = loopModel();
    const auto selLoop = selectedLoopId();
    if (!lpModel || !selLoop.has_value()) return;
    const auto idx = lpModel->indexOf(*selLoop);
    if (!idx) return;
    const auto& sel = lpModel->loops()[*idx];
    const auto [effStart, effEnd] =
        effectiveLoopRange(sel.id, sel.startMs, sel.endMs);
    const int xStart = msToX(effStart);
    const int xEnd   = msToX(effEnd);
    const int xLeft  = std::max(0, xStart);
    const int xRight = std::min(width(), xEnd);
    const QColor edgeCol(180, 240, 200, 255);
    painter.setPen(QPen(edgeCol, 2.0));
    if (xStart == xLeft) {
        painter.drawLine(xLeft, 0, xLeft, height());
    }
    if (xEnd == xRight) {
        painter.drawLine(xRight - 1, 0, xRight - 1, height());
    }
}

} // namespace fiddler::ui
