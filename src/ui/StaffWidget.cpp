#include "ui/StaffWidget.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"
#include "score/Pitch.h"
#include "util/Log.h"

#include <QFont>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QString>

#include <algorithm>
#include <climits>

namespace fiddler::ui {

namespace {

// Visual layout constants. Pulled out as named constants so the
// numbers in the paint code mean something at a glance.
//
// MEMO[#step6.1]: kStaffTopMarginPx grew from 18 → 72 to leave
// vertical room ABOVE the staff for note ledger lines. The violin
// range goes up to E7 (top of range — see NoteModel::isAcceptedPitch).
// E7's diatonic step is 21 above E4 (the bottom staff line), so it
// sits at staffBottomY − 21*(kStaffSpacingPx/2) = staffBottomY − 84
// pixels. With staffBottomY = 72 + 32 = 104, E7 lands at y = 20 —
// safely inside the widget.
constexpr int kStaffLineCount      = 5;
constexpr int kStaffSpacingPx      = 8;     // gap between adjacent staff lines
constexpr int kStaffTopMarginPx    = 72;    // room above for tune-type label + marker flags + ledger lines up to E7
constexpr int kStaffBottomMarginPx = 28;    // room below for ledger lines down to G3 (5 dia steps below E4)

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

// Note bar (piano-roll style — see project_step6_plan.md memory).
// The bar is centred vertically on the diatonic-step y so a note ON
// a line straddles it symmetrically, and a note IN a space sits
// neatly between two lines. Height is one staff spacing minus a
// hair of slack so adjacent diatonic steps don't visually touch.
constexpr int kNoteBarHeightPx       = kStaffSpacingPx - 2;     // 6 px
// Ledger lines: short horizontal marks every diatonic LINE position
// (i.e. every other step) between the note and the staff body. The
// stub extends a hair past the bar on either side so the note head
// reads as ON the ledger, not just near it.
constexpr int kLedgerHalfWidthPx     = 8;
constexpr int kLedgerLineThicknessPx = 1;

// Selection edge highlight — drawn around any selected note so the
// note property page in the dock has visible feedback in the staff.
constexpr int kNoteSelectionWidthPx  = 2;

// Accidental tint — sharps and flats render in a slightly bluer /
// teal-shifted green so they're visually distinct from naturals
// without needing a per-note ♯ glyph. Matches the convention every
// surveyed piano-roll editor follows: no per-note accidental
// glyphs; pitch is communicated by row position + colour. The
// engraving convention of "♯ in front of the note head" assumes
// round note heads and isolated whitespace between notes, neither
// of which fits a contiguous-rectangle layout. See the
// project_step6_plan memory entry for the research notes.
//
// Same green family so the tier is recognisable (Rule: progressive
// visual weight in the memory) — accidentals lean cool, naturals
// lean warm.

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

// MEMO[#step6.1]: height grew 100 → 132 so the violin range
// (G3 to E7, see NoteModel::isAcceptedPitch) fits with ledger
// lines: top margin (72) + 5 staff lines × 8 spacing (32) +
// bottom margin (28) = 132.
QSize StaffWidget::sizeHint()        const { return QSize(800, 132); }
QSize StaffWidget::minimumSizeHint() const { return QSize(120, 132); }

int StaffWidget::staffTopY()    const noexcept { return kStaffTopMarginPx; }
int StaffWidget::staffBottomY() const noexcept {
    return staffTopY() + (kStaffLineCount - 1) * kStaffSpacingPx;
}

int StaffWidget::staffYForPitch(int midi) const noexcept {
    // Bottom staff line E4 (midi 64) anchors the y axis. Each
    // diatonic step is half the staff-line spacing — the same
    // metric a line→space→line→space sequence uses.
    constexpr int kE4DiatonicStep   = 30;
    constexpr int kPxPerDiatonicStep = kStaffSpacingPx / 2;   // 4
    int dia = score::diatonicStep(midi);
    if (dia < 0) {
        // Accidental — sharp spelling pins the note to the natural
        // one semitone BELOW (A#4 sits at A4's line, with a ♯ glyph
        // drawn to the left in paintNotes). Try midi-1 (the natural
        // counterpart for any sharp).
        dia = score::diatonicStep(midi - 1);
    }
    if (dia < 0) return INT_MIN;
    return staffBottomY() - (dia - kE4DiatonicStep) * kPxPerDiatonicStep;
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
    paintNotes(painter);
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

void StaffWidget::paintNotes(QPainter& painter) const {
    const auto noteM = noteModel();
    if (!noteM) return;
    const auto notes = noteM->notes();
    const auto selId = selectedNoteId();

    // MEMO[#step6.1 debug]: log every paint with the model state so
    // we can confirm what the staff thinks it should be drawing.
    // Bumped to DEBUG so it appears under the default
    // `--log-filter='ui.*'` filter without needing --log-level=trace.
    // Each call additionally logs per-note id/midi/x-range below.
    FLOG_DEBUG("ui.staff",
               "paint-notes count={} selected={} duration-ms={} width-px={}",
               notes.size(), selId.value_or(-1),
               durationMs(), width());

    // Visual layers per bar, painted in order:
    //   1. Ledger stubs (under the note so the bar rides ON the
    //      ledger).
    //   2. The bar fill + border.
    //   3. The selection ring (only on the selected note).
    //
    // Naturals lean warm-green; accidentals lean cool-teal. Both in
    // the same hue family for clarity. See the constant block above.
    const QColor fillNaturalCol     (180, 220, 140, 200);
    const QColor borderNaturalCol   (140, 200, 100);
    const QColor fillAccidentalCol  (140, 200, 195, 200);
    const QColor borderAccidentalCol(100, 180, 165);
    const QColor selectionCol       (220, 255, 160);

    // Treble-staff geometry — same constants as staffYForPitch.
    // E4 (dia 30) sits on the bottom line, F5 (dia 38) on top.
    // Ledger LINES live at every even diatonic step outside [30,38]
    // (e.g. C4=28 → middle-C ledger below, A5=40 → first ledger above).
    constexpr int kE4DiatonicStep    = 30;
    constexpr int kF5DiatonicStep    = 38;
    constexpr int kPxPerDiatonicStep = kStaffSpacingPx / 2;     // 4

    const int bottomY = staffBottomY();

    // y of an even-diatonic-step line position. Used for ledgers.
    const auto yForLineDia = [&](int dia) {
        return bottomY - (dia - kE4DiatonicStep) * kPxPerDiatonicStep;
    };

    int paintedCount = 0;
    int skippedAccidental = 0;
    int skippedClipped = 0;
    for (const auto& n : notes) {
        const int y = staffYForPitch(n.midi);
        if (y == INT_MIN) {
            ++skippedAccidental;
            FLOG_TRACE("ui.staff",
                       "paint-note id={} midi={} skipped=accidental",
                       n.id, n.midi);
            continue;   // accidental — model forbids in 6.1
        }

        const int xStart = msToX(n.startMs);
        const int xEnd   = msToX(n.endMs);
        if (xEnd <= 0 || xStart >= width()) {
            ++skippedClipped;
            FLOG_TRACE("ui.staff",
                       "paint-note id={} midi={} startMs={} endMs={} "
                       "xStart={} xEnd={} skipped=clipped (width={})",
                       n.id, n.midi, n.startMs, n.endMs,
                       xStart, xEnd, width());
            continue;
        }
        ++paintedCount;
        FLOG_TRACE("ui.staff",
                   "paint-note id={} midi={} startMs={} endMs={} y={} "
                   "xStart={} xEnd={}",
                   n.id, n.midi, n.startMs, n.endMs, y, xStart, xEnd);

        // Diatonic step for ledger logic. For accidentals (sharp
        // spelling), use the natural-below's step — the bar is
        // already at that y via staffYForPitch. The bar is tinted
        // accordingly, so the accidental is visible without a
        // separate ♯ glyph.
        const bool isAccidental = (score::diatonicStep(n.midi) < 0);
        const int  dia = isAccidental
            ? score::diatonicStep(n.midi - 1)
            : score::diatonicStep(n.midi);

        const QColor& fillCol   = isAccidental
            ? fillAccidentalCol   : fillNaturalCol;
        const QColor& borderCol = isAccidental
            ? borderAccidentalCol : borderNaturalCol;

        const int xLeft  = std::max(0, xStart);
        const int xRight = std::min(width(), xEnd);
        const int barW   = std::max(1, xRight - xLeft);
        const int barTop = y - kNoteBarHeightPx / 2;
        const QRect barRect(xLeft, barTop, barW, kNoteBarHeightPx);
        const int barMidX = (xLeft + xRight) / 2;

        // Ledger stubs — every even-dia line position between the
        // staff body and the note (inclusive of the note's line if
        // it's a line position).
        if (dia > kF5DiatonicStep) {
            painter.setPen(QPen(borderCol, kLedgerLineThicknessPx));
            for (int probeDia = kF5DiatonicStep + 2;
                 probeDia <= dia; probeDia += 2) {
                const int ly = yForLineDia(probeDia);
                painter.drawLine(barMidX - kLedgerHalfWidthPx, ly,
                                 barMidX + kLedgerHalfWidthPx, ly);
            }
        } else if (dia < kE4DiatonicStep) {
            painter.setPen(QPen(borderCol, kLedgerLineThicknessPx));
            for (int probeDia = kE4DiatonicStep - 2;
                 probeDia >= dia; probeDia -= 2) {
                const int ly = yForLineDia(probeDia);
                painter.drawLine(barMidX - kLedgerHalfWidthPx, ly,
                                 barMidX + kLedgerHalfWidthPx, ly);
            }
        }

        painter.fillRect(barRect, fillCol);
        painter.setPen(QPen(borderCol, 1.0));
        painter.drawRect(barRect);

        if (selId == n.id) {
            painter.setPen(QPen(selectionCol, kNoteSelectionWidthPx));
            painter.drawRect(barRect.adjusted(-1, -1, 1, 1));
        }
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
