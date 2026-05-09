#include "ui/WaveformWidget.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
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
// Click-to-select tolerance: if the click x is within this many
// pixels of a barline / marker tick, the entry is selected (and
// the audio seeks to its exact source-ms). Larger than 1 px because
// hitting a single-pixel-wide tick precisely is annoying; small
// enough not to trap clicks meant for plain seeking.
constexpr int kHitTolerancePx = 5;

// Label flag band at the top of the widget where marker names sit.
// Reserved height; peaks and barline ticks paint over the rest.
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
// MEMO: the loop label sits in the BOTTOM of the band (the bottom
// kLoopLabelHeightPx pixels) on purpose — putting it at the top
// would clash with the marker flag row, since loops and markers
// commonly share start positions when the user converts a loop's
// "starts here" into a marker for orientation.
constexpr int kLoopBandAlphaUnselected = 35;
constexpr int kLoopBandAlphaSelected   = 90;
constexpr int kLoopLabelHeightPx       = 14;
constexpr int kLoopLabelPaddingPx      = 4;
constexpr int kLoopLabelFontPointSz    = 8;
constexpr int kLoopLabelMaxWidthPx     = 120;
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

void WaveformWidget::setMarkerModel(
    std::shared_ptr<const score::MarkerModel> model)
{
    if (markerModel_) {
        disconnect(markerModel_.get(), nullptr, this, nullptr);
    }
    markerModel_ = std::move(model);
    if (markerModel_) {
        connect(markerModel_.get(), &score::MarkerModel::changed,
                this, &WaveformWidget::onMarkerModelChanged);
    }
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

void WaveformWidget::setLoopModel(
    std::shared_ptr<const score::LoopModel> model)
{
    if (loopModel_) {
        disconnect(loopModel_.get(), nullptr, this, nullptr);
    }
    loopModel_ = std::move(model);
    if (loopModel_) {
        connect(loopModel_.get(), &score::LoopModel::changed,
                this, &WaveformWidget::onLoopModelChanged);
    }
    if (selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
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
    // MEMO: mutual exclusion — setting a barline selection clears any
    // active marker AND loop selection. The user wanted "the selected
    // artifact" to be a single concept (the project viewer shows its
    // properties), so even though all three selection slots exist
    // they're never simultaneously populated.
    if (index.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (index.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (selectedBarline_ == index) {
        // The marker/loop-clear above may still have caused a repaint
        // need (the cleared selection was highlighted). Repaint just
        // in case; update() is a cheap no-op when nothing moved.
        update();
        return;
    }
    selectedBarline_ = index;
    update();
    emit barlineSelectionChanged(selectedBarline_);
}

void WaveformWidget::setSelectedMarkerId(std::optional<std::int64_t> id) {
    // Validate against current model — if the ID isn't there
    // anymore, coerce to nullopt rather than holding a dangling ID.
    if (id.has_value() && markerModel_
        && !markerModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    // Mirror of setSelectedBarline: setting a marker selection clears
    // barline AND loop selections.
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (id.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (selectedMarkerId_ == id) {
        update();
        return;
    }
    selectedMarkerId_ = id;
    update();
    emit markerSelectionChanged(selectedMarkerId_);
}

std::optional<std::int64_t>
WaveformWidget::primaryAnchorMs() const noexcept {
    if (selectedBarline_.has_value() && barlineModel_
        && *selectedBarline_ < barlineModel_->size())
    {
        return barlineModel_->barlines()[*selectedBarline_];
    }
    if (selectedMarkerId_.has_value() && markerModel_) {
        if (const auto idx = markerModel_->indexOf(*selectedMarkerId_)) {
            return markerModel_->markers()[*idx].sourceMs;
        }
    }
    return std::nullopt;
}

void WaveformWidget::setSecondaryAnchorMs(
    std::optional<std::int64_t> ms)
{
    if (secondaryAnchorMs_ == ms) return;
    secondaryAnchorMs_ = ms;
    update();
    emit secondaryAnchorChanged(secondaryAnchorMs_);
}

void WaveformWidget::setSelectedLoopId(std::optional<std::int64_t> id) {
    // Validate against current model — drop dangling IDs.
    if (id.has_value() && loopModel_
        && !loopModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    // Mirror of the others: setting a loop selection clears
    // barline AND marker selections.
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (id.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (selectedLoopId_ == id) {
        update();
        return;
    }
    selectedLoopId_ = id;
    update();
    emit loopSelectionChanged(selectedLoopId_);
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

void WaveformWidget::onMarkerModelChanged() {
    // The selected marker might have been removed, or its position
    // edited (in which case its ID is still valid). Either way we
    // need to repaint. We only clear the selection if the ID has
    // genuinely gone away — moves shouldn't deselect.
    if (selectedMarkerId_.has_value() && markerModel_
        && !markerModel_->indexOf(*selectedMarkerId_).has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

void WaveformWidget::onLoopModelChanged() {
    // Mirror of onMarkerModelChanged: range edits keep the same ID
    // (so selection survives), but a remove genuinely drops the ID
    // and we have to clear the selection slot to match.
    if (selectedLoopId_.has_value() && loopModel_
        && !loopModel_->indexOf(*selectedLoopId_).has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
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

    // Loop bands — drawn first so barlines, markers, and the cursor
    // all paint on top (otherwise the band's translucent fill would
    // smudge over a tick that the user wants to read clearly).
    if (loopModel_) {
        const auto loops = loopModel_->loops();
        QFont labelFont = painter.font();
        labelFont.setPointSize(kLoopLabelFontPointSz);
        labelFont.setBold(true);
        const QFontMetrics fm(labelFont);

        for (const auto& l : loops) {
            const int xStart = msToX(l.startMs);
            const int xEnd   = msToX(l.endMs);
            // Skip degenerate or off-screen bands. msToX() clamps to
            // [0, w-1] so we still get a one-pixel-wide rect at the
            // edge for loops that start before x=0; that's fine.
            if (xEnd <= 0 || xStart >= width()) continue;

            const int xLeft  = std::max(0, xStart);
            const int xRight = std::min(width(), xEnd);
            const int bandW  = std::max(1, xRight - xLeft);

            const bool selected = (selectedLoopId_ == l.id);
            const int  alpha    = selected
                ? kLoopBandAlphaSelected
                : kLoopBandAlphaUnselected;

            // Soft sage green — chosen to be visibly distinct from
            // the cyan markers and yellow barlines, and not to fight
            // the peaks-blue waveform.
            const QColor bandCol(120, 200, 140, alpha);
            painter.fillRect(QRect(xLeft, 0, bandW, height()), bandCol);

            // Vertical edges of the band, drawn slightly more opaque
            // so the boundary is legible even when alpha is low.
            const QColor edgeCol(140, 220, 160,
                                 std::min(255, alpha + 60));
            painter.setPen(QPen(edgeCol, selected ? 2.0 : 1.0));
            painter.drawLine(xLeft,      0, xLeft,      height());
            painter.drawLine(xRight - 1, 0, xRight - 1, height());

            // Loop name label in the BOTTOM strip of the band
            // (avoids overlapping the marker flag row at the top
            // when a loop and a marker share a start position).
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
    // artifact (the common case — the user Ctrl+clicked an existing
    // tick), DON'T draw a separate dashed tick over the artifact's
    // solid tick. The underlying solid line would fill the gaps in
    // the dash pattern and the dashing wouldn't be visible. Instead,
    // detect the overlap here, paint the artifact ITSELF with a
    // dashed pen below, and skip the standalone tick later.
    bool secondaryOnArtifact = false;
    if (secondaryAnchorMs_.has_value()) {
        if (barlineModel_) {
            for (auto barMs : barlineModel_->barlines()) {
                if (barMs == *secondaryAnchorMs_) {
                    secondaryOnArtifact = true;
                    break;
                }
            }
        }
        if (!secondaryOnArtifact && markerModel_) {
            for (const auto& m : markerModel_->markers()) {
                if (m.sourceMs == *secondaryAnchorMs_) {
                    secondaryOnArtifact = true;
                    break;
                }
            }
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
            const bool isAnchor = secondaryAnchorMs_.has_value()
                              && bars[i] == *secondaryAnchorMs_;
            QPen pen;
            if (isAnchor) {
                // Bright yellow + dashed + thick: visibly distinct
                // from both the regular and selected barlines.
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
    if (markerModel_) {
        const auto markers = markerModel_->markers();
        QFont flagFont = painter.font();
        flagFont.setPointSize(kMarkerFlagFontPointSz);
        flagFont.setBold(true);
        painter.setFont(flagFont);
        const QFontMetrics fm(flagFont);

        for (const auto& m : markers) {
            const int x = msToX(m.sourceMs);
            if (x < 0 || x >= width()) continue;
            const bool selected = (selectedMarkerId_ == m.id);
            const bool isAnchor = secondaryAnchorMs_.has_value()
                              && m.sourceMs == *secondaryAnchorMs_;
            const QColor lineCol = selected
                ? QColor(140, 230, 250)
                : QColor(100, 200, 220);

            // Vertical tick line — full height. Marker-as-secondary
            // gets a brighter, dashed, thicker line; the label flag
            // stays solid so the marker's name remains legible.
            QPen tickPen;
            if (isAnchor) {
                tickPen = QPen(QColor(160, 240, 255), 2.0);
                tickPen.setStyle(Qt::DashLine);
            } else {
                tickPen = QPen(lineCol, selected ? 2.0 : 1.0);
            }
            painter.setPen(tickPen);
            painter.drawLine(x, 0, x, height());

            // Label flag at the top: filled rectangle with the name.
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

    // Standalone secondary anchor tick — only drawn when the
    // secondary doesn't sit on top of an existing artifact. The
    // overlap case is handled above by replacing the artifact's
    // solid tick with a dashed one. MEMO: dashed style is the
    // "armed for combination, not yet committed" convention from
    // DAW ghost cursors.
    if (secondaryAnchorMs_.has_value() && !secondaryOnArtifact) {
        const int x = msToX(*secondaryAnchorMs_);
        if (x >= 0 && x < width()) {
            QPen pen(QColor(255, 200, 90), 2);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
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
    const bool ctrlHeld =
        (event->modifiers() & Qt::ControlModifier) != 0;

    // MEMO: hit-test priority — markers FIRST (labelled and visually
    // atop barlines, so a click on a flag should select the marker),
    // then barlines, then plain seek.

    std::int64_t tolMs = 0;
    if (overview_ && width() > 0) {
        const auto durationMs = overview_->duration().count();
        if (durationMs > 0) {
            tolMs = static_cast<std::int64_t>(kHitTolerancePx)
                    * durationMs / width();
        }
    }

    // MEMO: Ctrl+click semantics — "add as second anchor for loop
    // creation". The current primary's ms is captured into the
    // secondary slot before the new selection is installed; the
    // dashed tick at that ms persists until cleared. Plain click
    // clears the secondary slot as a side effect (the user is
    // starting a fresh selection). See commit 4 for the reason this
    // is widget-level (rather than MainWindow-level) state.
    auto prepareClickStateChange = [&]() {
        if (ctrlHeld) {
            if (const auto primMs = primaryAnchorMs()) {
                setSecondaryAnchorMs(*primMs);
            }
        } else {
            setSecondaryAnchorMs(std::nullopt);
        }
    };

    // 1. Marker hit?
    if (markerModel_ && markerModel_->size() > 0 && tolMs >= 0) {
        if (const auto markerHit = markerModel_->nearest(ms, tolMs)) {
            const auto idx = markerModel_->indexOf(*markerHit);
            if (idx) {
                prepareClickStateChange();
                setSelectedMarkerId(*markerHit);
                emit seekRequested(
                    markerModel_->markers()[*idx].sourceMs);
                event->accept();
                return;
            }
        }
    }

    // 2. Barline hit?
    if (barlineModel_ && barlineModel_->size() > 0 && tolMs >= 0) {
        if (const auto barHit = barlineModel_->nearest(ms, tolMs)) {
            prepareClickStateChange();
            setSelectedBarline(*barHit);
            emit seekRequested(barlineModel_->barlines()[*barHit]);
            event->accept();
            return;
        }
    }

    // 3. No artifact hit. Ctrl+click on empty space is a no-op so
    // the user can't accidentally lose their second anchor by
    // missing a tick. Plain click clears everything and seeks.
    if (ctrlHeld) {
        event->accept();
        return;
    }
    if (selectedBarline_.has_value()) {
        selectedBarline_.reset();
        update();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        update();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (secondaryAnchorMs_.has_value()) {
        setSecondaryAnchorMs(std::nullopt);
    }
    emit seekRequested(ms);
    event->accept();
}

void WaveformWidget::keyPressEvent(QKeyEvent* event) {
    // MEMO: arrow nav, Esc, and Del all operate on whichever
    // artifact kind is currently selected. A barline-selection
    // arrow moves between barlines; a marker-selection arrow moves
    // between markers. Esc clears whichever is set; Del fires the
    // matching delete signal.

    const bool haveBarline = static_cast<bool>(barlineModel_);
    const bool haveMarker  = static_cast<bool>(markerModel_);
    if (!haveBarline && !haveMarker) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left:
        if (selectedBarline_ && *selectedBarline_ > 0) {
            setSelectedBarline(*selectedBarline_ - 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        } else if (selectedMarkerId_ && markerModel_) {
            const auto idx = markerModel_->indexOf(*selectedMarkerId_);
            if (idx && *idx > 0) {
                const auto prevId = markerModel_->idAt(*idx - 1);
                if (prevId) {
                    setSelectedMarkerId(*prevId);
                    const auto newIdx = markerModel_->indexOf(*prevId);
                    if (newIdx) {
                        emit seekRequested(
                            markerModel_->markers()[*newIdx].sourceMs);
                    }
                }
            }
        }
        event->accept();
        return;
    case Qt::Key_Right:
        if (selectedBarline_ && barlineModel_
            && *selectedBarline_ + 1 < barlineModel_->size())
        {
            setSelectedBarline(*selectedBarline_ + 1);
            emit seekRequested(
                barlineModel_->barlines()[*selectedBarline_]);
        } else if (selectedMarkerId_ && markerModel_) {
            const auto idx = markerModel_->indexOf(*selectedMarkerId_);
            if (idx && *idx + 1 < markerModel_->size()) {
                const auto nextId = markerModel_->idAt(*idx + 1);
                if (nextId) {
                    setSelectedMarkerId(*nextId);
                    const auto newIdx = markerModel_->indexOf(*nextId);
                    if (newIdx) {
                        emit seekRequested(
                            markerModel_->markers()[*newIdx].sourceMs);
                    }
                }
            }
        }
        event->accept();
        return;
    case Qt::Key_Escape:
        if (selectedBarline_.has_value()) {
            setSelectedBarline(std::nullopt);
        } else if (selectedMarkerId_.has_value()) {
            setSelectedMarkerId(std::nullopt);
        }
        // Also drop any secondary anchor — Esc means "I'm done with
        // the in-flight selection state, including loop anchors".
        if (secondaryAnchorMs_.has_value()) {
            setSecondaryAnchorMs(std::nullopt);
        }
        event->accept();
        return;
    case Qt::Key_Delete:
        if (selectedBarline_.has_value()) {
            emit barlineDeleteRequested(*selectedBarline_);
            // Don't reset selection here — onBarlineModelChanged()
            // handles it once the model drops the entry.
        } else if (selectedMarkerId_.has_value()) {
            emit markerDeleteRequested(*selectedMarkerId_);
        }
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

} // namespace fiddler::ui
