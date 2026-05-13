#include "ui/StaffWidget.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"
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

// ============================================================
// Piano-roll layout (step 6.2)
// ============================================================
//
// The widget has two columns:
//
//   1. A piano keyboard on the left (kKeyboardWidthPx). White keys
//      occupy their row fully; black keys are narrower, right-aligned
//      and darker. C notes carry a small "C{octave}" label so the
//      user can locate themselves on the keyboard.
//   2. A chromatic grid on the right. Each row spans one semitone in
//      midi. The y axis is INVERTED: top of grid = highest pitch
//      (E7), bottom = lowest pitch (G3). White-key rows are tinted
//      slightly lighter than black-key rows so the keyboard's
//      colouration extends across the grid.
//
// Total visible range: G3 (midi 55) to E7 (midi 100) — the violin's
// playable span (kept in lockstep with NoteModel::isAcceptedPitch).

constexpr int kRowMidiLow            = 55;     // G3 — open G string
constexpr int kRowMidiHigh           = 100;    // E7 — top of standard range
constexpr int kSemitoneRowHeightPx   = 8;
constexpr int kRowCount              = kRowMidiHigh - kRowMidiLow + 1;  // 46
constexpr int kGridTopMarginPx       = 18;     // tune-type + marker flag banner
constexpr int kGridBottomMarginPx    = 18;     // loop labels
constexpr int kGridHeightPx          = kRowCount * kSemitoneRowHeightPx; // 368

// MEMO: must stay in lockstep with StaffWidget::kKeyboardWidthPx in
// the header — the constant is duplicated here so the paint code in
// this TU stays a plain `constexpr int`, while MainWindow gets a
// public class member to read.
constexpr int kKeyboardWidthPx       = StaffWidget::kKeyboardWidthPx;
constexpr int kBlackKeyWidthRatio    = 65;     // black key spans 65 % of kbWidth, right-aligned

// Tune-type label sits in the top margin, over the keyboard column.
constexpr int kTuneTypeLabelPointSz  = 10;
constexpr int kTuneTypeLabelLeftPx   = 6;

// Marker label flag — sits in the top margin band above the grid.
constexpr int kMarkerFlagHeightPx     = 12;
constexpr int kMarkerFlagPaddingPx    = 4;
constexpr int kMarkerFlagFontPointSz  = 8;
constexpr int kMarkerFlagMaxWidthPx   = 120;

// Loop band visuals — same palette and rules as WaveformWidget.
constexpr int kLoopBandAlphaUnselected = 35;
constexpr int kLoopBandAlphaSelected   = 90;
constexpr int kLoopLabelHeightPx       = 12;
constexpr int kLoopLabelPaddingPx      = 4;
constexpr int kLoopLabelFontPointSz    = 8;
constexpr int kLoopLabelMaxWidthPx     = 120;

// Note bar — single colour. Each accidental gets its own chromatic
// row, so the bar's row position alone communicates the pitch. The
// tint-vs-natural distinction from step 6.1 retires.
constexpr int kNoteBarHeightPx       = kSemitoneRowHeightPx - 2;   // 6 px
constexpr int kNoteSelectionWidthPx  = 2;

// Black-key chroma classes: C# (1), D# (3), F# (6), G# (8), A# (10).
[[nodiscard]] bool isBlackKey(int midi) noexcept {
    const int chroma = midi % 12;
    return chroma == 1 || chroma == 3 || chroma == 6
        || chroma == 8 || chroma == 10;
}

// True iff this midi value is a "C" — for octave labels on the
// keyboard.
[[nodiscard]] bool isC(int midi) noexcept {
    return (midi % 12) == 0;
}

[[nodiscard]] int octaveOf(int midi) noexcept {
    return midi / 12 - 1;
}

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

// MEMO[#step6.2]: height = top margin (18) + grid (46 rows × 8 px =
// 368) + bottom margin (18) = 404 px. The grid fits the full violin
// range (G3 to E7) chromatically — every semitone gets its own row.
QSize StaffWidget::sizeHint()        const {
    return QSize(800, kGridTopMarginPx + kGridHeightPx + kGridBottomMarginPx);
}
QSize StaffWidget::minimumSizeHint() const {
    return QSize(120, kGridTopMarginPx + kGridHeightPx + kGridBottomMarginPx);
}

int StaffWidget::leftMarginPx() const noexcept {
    return kKeyboardWidthPx;
}

int StaffWidget::gridTopY()    const noexcept { return kGridTopMarginPx; }
int StaffWidget::gridBottomY() const noexcept {
    return kGridTopMarginPx + kGridHeightPx;
}

int StaffWidget::rowYForMidi(int midi) const noexcept {
    if (midi < kRowMidiLow || midi > kRowMidiHigh) return INT_MIN;
    // Top of widget = highest pitch (kRowMidiHigh). Each row's
    // CENTRE y lies at gridTopY + rowIndex × rowHeight + rowHeight/2.
    const int rowIndex = kRowMidiHigh - midi;
    return gridTopY()
         + rowIndex * kSemitoneRowHeightPx
         + kSemitoneRowHeightPx / 2;
}

int StaffWidget::midiForRowY(int y) const noexcept {
    const int top = gridTopY();
    if (y < top) return -1;
    const int rowIndex = (y - top) / kSemitoneRowHeightPx;
    if (rowIndex < 0 || rowIndex >= kRowCount) return -1;
    return kRowMidiHigh - rowIndex;
}

// ---- painting ------------------------------------------------------------

void StaffWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(20, 20, 24));   // matches WaveformWidget

    // Paint order (low z-order first):
    //   1. Grid background (chromatic row tint).
    //   2. Piano keyboard column on the left.
    //   3. Tune-type banner in the top margin (over the keyboard column).
    //   4. Loop bands (translucent, full-height across the grid).
    //   5. Barlines (vertical ticks in the grid).
    //   6. Note bars at their chromatic rows.
    //   7. Markers (vertical ticks + label flags in the top margin).
    //   8. Selected loop edges re-drawn so they sit on top of any
    //      coincident markers / barlines (issue #11).
    //   9. Cursor (red playhead).
    //  10. Secondary anchor (dashed) — after the cursor so the dashes
    //      pierce through the cursor when both share an x.
    //  11. Zoom-anchor guide (Ctrl+wheel preview).
    paintGridBackground(painter);
    paintPianoKeyboard(painter);
    paintTuneTypeBanner(painter);
    paintLoops(painter);
    paintBarlines(painter);
    paintNotes(painter);
    paintMarkers(painter);
    paintSelectedLoopEdges(painter);
    paintCursor(painter);
    paintSecondaryAnchor(painter);
    paintZoomAnchorGuide(painter);
}

void StaffWidget::paintGridBackground(QPainter& painter) const {
    // Two-tone row fill — white-key rows are a touch lighter than
    // black-key rows, matching the keyboard's colouration to the
    // right. Octave separators (1-px lines at every C row's top)
    // make octaves countable at a glance.
    const QColor whiteRowCol(34, 34, 38);
    const QColor blackRowCol(26, 26, 30);
    const QColor octaveLineCol(52, 52, 58);

    const int gridLeft  = leftMarginPx();
    const int gridRight = width();
    if (gridRight <= gridLeft) return;
    const int gridW = gridRight - gridLeft;

    for (int midi = kRowMidiLow; midi <= kRowMidiHigh; ++midi) {
        const int yCenter = rowYForMidi(midi);
        if (yCenter == INT_MIN) continue;
        const int rowTop = yCenter - kSemitoneRowHeightPx / 2;
        const QRect rowRect(gridLeft, rowTop, gridW, kSemitoneRowHeightPx);
        painter.fillRect(rowRect,
                         isBlackKey(midi) ? blackRowCol : whiteRowCol);
        // Octave separator drawn at the TOP of each C row (i.e.
        // between B (midi-1) below and C above). Looks like a faint
        // horizontal divider every 12 rows.
        if (isC(midi)) {
            painter.setPen(QPen(octaveLineCol, 1.0));
            painter.drawLine(gridLeft, rowTop, gridRight - 1, rowTop);
        }
    }
}

void StaffWidget::paintPianoKeyboard(QPainter& painter) const {
    const int kbLeft  = 0;
    const int kbRight = leftMarginPx();
    if (kbRight <= kbLeft) return;

    const QColor whiteKeyCol(230, 230, 224);
    const QColor whiteKeyBorder(150, 150, 145);
    const QColor blackKeyCol(28, 28, 30);
    const QColor cLabelCol(80, 80, 90);

    // Fill the keyboard background first — same tone as a piano's
    // white-key bed, so any gap between keys reads as white-keyboard.
    painter.fillRect(QRect(kbLeft, gridTopY(),
                           kbRight - kbLeft, kGridHeightPx),
                     whiteKeyCol);

    // Walk every chromatic row. White keys outline the full kbWidth;
    // black keys are narrower rectangles overlaid on the right side.
    QFont labelFont = painter.font();
    labelFont.setPointSize(7);
    labelFont.setBold(false);
    painter.setFont(labelFont);
    const QFontMetrics labelFm(labelFont);

    for (int midi = kRowMidiLow; midi <= kRowMidiHigh; ++midi) {
        const int yCenter = rowYForMidi(midi);
        if (yCenter == INT_MIN) continue;
        const int rowTop = yCenter - kSemitoneRowHeightPx / 2;

        if (isBlackKey(midi)) {
            // Black key: narrower rectangle, right-aligned in the
            // keyboard column. Spans the full row height.
            const int blackW = kKeyboardWidthPx * kBlackKeyWidthRatio / 100;
            const QRect blackRect(kbRight - blackW, rowTop,
                                  blackW, kSemitoneRowHeightPx);
            painter.fillRect(blackRect, blackKeyCol);
        } else {
            // White key: thin grey hairline at the top of each row
            // (separator between adjacent keys). The row's fill
            // already happened (white-key colour above).
            painter.setPen(QPen(whiteKeyBorder, 0.5));
            painter.drawLine(kbLeft, rowTop, kbRight - 1, rowTop);

            // C-note label sits in the leftmost few pixels of the C
            // row. Other naturals stay unlabelled — labelling every
            // white key clutters the column.
            if (isC(midi)) {
                painter.setPen(cLabelCol);
                const QString label = QString("C%1").arg(octaveOf(midi));
                painter.drawText(
                    kbLeft + 3,
                    yCenter + labelFm.ascent() / 2 - 1,
                    label);
            }
        }
    }

    // Right border separating the keyboard from the grid.
    painter.setPen(QPen(whiteKeyBorder, 1.0));
    painter.drawLine(kbRight - 1, gridTopY(),
                     kbRight - 1, gridBottomY());
}

void StaffWidget::paintTuneTypeBanner(QPainter& painter) const {
    const auto barModel = barlineModel();
    if (!barModel) return;
    const auto& ts = barModel->timeSignature();
    if (ts.tuneType.isEmpty()) return;

    QFont labelFont = painter.font();
    labelFont.setPointSize(kTuneTypeLabelPointSz);
    labelFont.setBold(false);
    painter.setFont(labelFont);
    painter.setPen(QColor(220, 220, 220));
    const QFontMetrics fm(labelFont);
    // Centred vertically in the top margin band.
    const int y = (gridTopY() + fm.ascent()) / 2 + 1;
    painter.drawText(kTuneTypeLabelLeftPx, y, ts.tuneType);
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
    const int  topY       = gridTopY();
    const int  bottomY    = gridBottomY();
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

    // Single colour for every note bar — the chromatic row's y is
    // what disambiguates pitch (no tint distinction needed since
    // each accidental gets its own row, unlike the 6.1 diatonic
    // staff). Selection ring stays for visible feedback.
    const QColor fillCol    (180, 220, 140, 215);
    const QColor borderCol  (140, 200, 100);
    const QColor selectionCol(220, 255, 160);
    const int    kbWidth = leftMarginPx();

    int paintedCount = 0;
    int skippedOutOfRange = 0;
    int skippedClipped = 0;
    for (const auto& n : notes) {
        // MEMO[#60]: read ghost-overridden values when this note is
        // mid-drag. Move/Resize override range; Move also overrides
        // midi. Otherwise model values pass through unchanged.
        const auto [effStart, effEnd] =
            effectiveNoteRange(n.id, n.startMs, n.endMs);
        const int effMidi = effectiveNoteMidi(n.id, n.midi);
        const int y = rowYForMidi(effMidi);
        if (y == INT_MIN) {
            ++skippedOutOfRange;
            FLOG_TRACE("ui.staff",
                       "paint-note id={} midi={} skipped=out-of-range",
                       n.id, effMidi);
            continue;
        }

        const int xStart = msToX(effStart);
        const int xEnd   = msToX(effEnd);
        if (xEnd <= kbWidth || xStart >= width()) {
            ++skippedClipped;
            FLOG_TRACE("ui.staff",
                       "paint-note id={} midi={} startMs={} endMs={} "
                       "xStart={} xEnd={} skipped=clipped (width={})",
                       n.id, effMidi, effStart, effEnd,
                       xStart, xEnd, width());
            continue;
        }
        ++paintedCount;
        FLOG_TRACE("ui.staff",
                   "paint-note id={} midi={} startMs={} endMs={} y={} "
                   "xStart={} xEnd={}",
                   n.id, effMidi, effStart, effEnd, y, xStart, xEnd);

        const int xLeft  = std::max(kbWidth, xStart);
        const int xRight = std::min(width(), xEnd);
        const int barW   = std::max(1, xRight - xLeft);
        const int barTop = y - kNoteBarHeightPx / 2;
        const QRect barRect(xLeft, barTop, barW, kNoteBarHeightPx);

        painter.fillRect(barRect, fillCol);
        painter.setPen(QPen(borderCol, 1.0));
        painter.drawRect(barRect);

        if (selId == n.id) {
            painter.setPen(QPen(selectionCol, kNoteSelectionWidthPx));
            painter.drawRect(barRect.adjusted(-1, -1, 1, 1));
        }
    }

    // Phantom note for the drag-to-create gesture (issue #60).
    // No model entry yet; we draw a translucent outline so the user
    // sees what's about to land. On release, MainWindow turns this
    // into a real note via noteCreateCommitted.
    if (const auto phantom = phantomNoteGhost()) {
        const int y = rowYForMidi(phantom->midi);
        if (y != INT_MIN) {
            const int xStart = msToX(phantom->startMs);
            const int xEnd   = msToX(phantom->endMs);
            if (xEnd > kbWidth && xStart < width()) {
                const int xLeft  = std::max(kbWidth, xStart);
                const int xRight = std::min(width(), xEnd);
                const int barW   = std::max(1, xRight - xLeft);
                const int barTop = y - kNoteBarHeightPx / 2;
                const QRect r(xLeft, barTop, barW, kNoteBarHeightPx);
                // Lighter fill + dashed outline so the phantom reads
                // as draft vs. the solid committed bars.
                QColor draftFill = fillCol;
                draftFill.setAlpha(120);
                painter.fillRect(r, draftFill);
                QPen draftPen(borderCol, 1.0);
                draftPen.setStyle(Qt::DashLine);
                painter.setPen(draftPen);
                painter.drawRect(r);
            }
        }
    }
}

std::optional<std::int64_t>
StaffWidget::hitNote(int xWidget, int yWidget) const {
    // Hit-test the click against every note bar. A bar covers
    //   x ∈ [msToX(startMs), msToX(endMs))
    //   y ∈ [rowYForMidi(midi) - barHeight/2, +barHeight/2)
    // Walk the notes in REVERSE so the visually-topmost bar wins
    // on overlap — the loop in paintNotes paints earlier entries
    // first, so later entries draw over them.
    const auto noteM = noteModel();
    if (!noteM) return std::nullopt;
    if (xWidget < leftMarginPx()) return std::nullopt;

    const auto notes = noteM->notes();
    for (std::size_t i = notes.size(); i-- > 0;) {
        const auto& n = notes[i];
        const int rowY = rowYForMidi(n.midi);
        if (rowY == INT_MIN) continue;
        const int barTop    = rowY - kNoteBarHeightPx / 2;
        const int barBottom = barTop + kNoteBarHeightPx;
        if (yWidget < barTop || yWidget >= barBottom) continue;
        const int xStart = msToX(n.startMs);
        const int xEnd   = msToX(n.endMs);
        if (xWidget < xStart || xWidget >= xEnd) continue;
        FLOG_TRACE("ui.staff",
                   "hit-note id={} midi={} startMs={} endMs={} "
                   "click=({},{})",
                   n.id, n.midi, n.startMs, n.endMs, xWidget, yWidget);
        return n.id;
    }
    return std::nullopt;
}

std::optional<ScoreOverlayBase::NoteEdgeHit>
StaffWidget::hitNoteEdge(int xWidget, int yWidget) const {
    // Same y-row test as hitNote, but instead of checking the bar's
    // body, check whether x is within kEdgeTolerancePx of either
    // edge. The tolerance zone may extend slightly outside the bar,
    // which is what makes very short bars still resizable. If both
    // edges are within tolerance (a tiny bar), the start edge wins
    // arbitrarily — same convention as loop edges (issue #11).
    const auto noteM = noteModel();
    if (!noteM) return std::nullopt;
    if (xWidget < leftMarginPx()) return std::nullopt;

    const auto notes = noteM->notes();
    for (std::size_t i = notes.size(); i-- > 0;) {
        const auto& n = notes[i];
        const int rowY = rowYForMidi(n.midi);
        if (rowY == INT_MIN) continue;
        const int barTop    = rowY - kNoteBarHeightPx / 2;
        const int barBottom = barTop + kNoteBarHeightPx;
        if (yWidget < barTop || yWidget >= barBottom) continue;
        const int xStart = msToX(n.startMs);
        const int xEnd   = msToX(n.endMs);
        if (std::abs(xWidget - xStart) <= kEdgeTolerancePx) {
            return NoteEdgeHit{n.id, /*isStart=*/true};
        }
        if (std::abs(xWidget - xEnd) <= kEdgeTolerancePx) {
            return NoteEdgeHit{n.id, /*isStart=*/false};
        }
    }
    return std::nullopt;
}

int StaffWidget::pixelToMidi(int yWidget) const {
    return midiForRowY(yWidget);
}

int StaffWidget::chromaticRowAt(int xWidget, int yWidget) const {
    if (xWidget < leftMarginPx()) return -1;
    return midiForRowY(yWidget);
}

// MEMO[#step6.2]: extends the base's plain-seek branch — when the
// click lands on a valid chromatic row, emit placeNoteRequested so
// MainWindow can add a note at (ms, midi). Clicks above the grid
// (top margin) or below it just stay as a seek.
void StaffWidget::onEmptySpaceClick(int /*xWidget*/,
                                    int yWidget,
                                    std::int64_t ms) {
    const int midi = midiForRowY(yWidget);
    if (midi < 0) {
        FLOG_TRACE("ui.staff",
                   "empty-space-click y={} ms={} no-row hit", yWidget, ms);
        return;
    }
    FLOG_DEBUG("ui.staff",
               "empty-space-click ms={} midi={} emit=place-note",
               ms, midi);
    emit placeNoteRequested(ms, midi);
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
