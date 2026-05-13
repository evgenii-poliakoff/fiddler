#include "ui/ScoreOverlayBase.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"
#include "util/Log.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace fiddler::ui {

namespace {

// Click-to-select tolerance in pixels — used by mousePressEvent's
// hit test for barlines and markers. Same value the widgets used
// before the refactor (#12) so user feel is unchanged.
constexpr int kHitTolerancePx = 5;

// Loop-edge hit-test tolerance: ScoreOverlayBase::kEdgeTolerancePx
// (public so subclass hit-tests can match). The literal lives on
// the header — issue #11.

// Snap-to-anchor tolerance for loop-edge dragging — same value as
// the edge hit radius so the affordances match: if you can grab the
// edge from N px, you can also park it on an anchor from N px.
constexpr int kSnapTolerancePx = 6;

} // namespace

ScoreOverlayBase::ScoreOverlayBase(QWidget* parent) : QWidget(parent) {
    // StrongFocus so the widget can receive arrow / Esc / Del keys
    // after a click gives it focus.
    setFocusPolicy(Qt::StrongFocus);
    // MEMO[#49]: mouse tracking enabled so we can paint the zoom
    // anchor guide as the user hovers with Ctrl held — without
    // tracking, mouseMoveEvent only fires while a button is down.
    setMouseTracking(true);
    // MEMO[#49 smoke]: install a global key-event filter so the
    // zoom-anchor guide reacts to Ctrl press/release even when
    // the widget doesn't have keyboard focus — which is the
    // common case while the user is just hovering with the mouse
    // (no click yet, focus is on some other widget).
    if (auto* app = QCoreApplication::instance()) {
        app->installEventFilter(this);
    }
}

ScoreOverlayBase::~ScoreOverlayBase() = default;

// ---- model attachment ----------------------------------------------------

void ScoreOverlayBase::setBarlineModel(
    std::shared_ptr<const score::BarlineModel> model)
{
    if (barlineModel_) {
        disconnect(barlineModel_.get(), nullptr, this, nullptr);
    }
    barlineModel_ = std::move(model);
    if (barlineModel_) {
        connect(barlineModel_.get(), &score::BarlineModel::changed,
                this, &ScoreOverlayBase::onBarlineModelChanged);
    }
    if (selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    update();
}

void ScoreOverlayBase::setMarkerModel(
    std::shared_ptr<const score::MarkerModel> model)
{
    if (markerModel_) {
        disconnect(markerModel_.get(), nullptr, this, nullptr);
    }
    markerModel_ = std::move(model);
    if (markerModel_) {
        connect(markerModel_.get(), &score::MarkerModel::changed,
                this, &ScoreOverlayBase::onMarkerModelChanged);
    }
    if (selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

void ScoreOverlayBase::setLoopModel(
    std::shared_ptr<const score::LoopModel> model)
{
    if (loopModel_) {
        disconnect(loopModel_.get(), nullptr, this, nullptr);
    }
    loopModel_ = std::move(model);
    if (loopModel_) {
        connect(loopModel_.get(), &score::LoopModel::changed,
                this, &ScoreOverlayBase::onLoopModelChanged);
    }
    if (selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    update();
}

void ScoreOverlayBase::setNoteModel(
    std::shared_ptr<const score::NoteModel> model)
{
    if (noteModel_) {
        disconnect(noteModel_.get(), nullptr, this, nullptr);
    }
    noteModel_ = std::move(model);
    if (noteModel_) {
        connect(noteModel_.get(), &score::NoteModel::changed,
                this, &ScoreOverlayBase::onNoteModelChanged);
    }
    if (selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    update();
}

// ---- read-only state -----------------------------------------------------

std::optional<std::int64_t>
ScoreOverlayBase::primaryAnchorMs() const noexcept {
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

std::int64_t ScoreOverlayBase::pixelsToMs(int px) const noexcept {
    // Pixel→ms scale by sampling xToMs at two points INSIDE the
    // time axis (i.e. past leftMarginPx()), so the piano-roll
    // keyboard column doesn't collapse the answer to zero. Picks up
    // the viewport zoom automatically: at deep zoom-in a given
    // pixel count maps to a smaller ms span, so hit-tests tighten
    // in lockstep with what the user sees.
    const int lm = leftMarginPx();
    return xToMs(lm + px) - xToMs(lm);
}

// ---- viewport / coord transforms (#49) -----------------------------------

namespace {

// Returns the effective [start, end) range to use for coord
// transforms — either the explicitly-set viewport or [0, duration]
// when no viewport is active.
struct EffectiveRange { std::int64_t start; std::int64_t end; };

EffectiveRange effectiveRange(std::int64_t viewportStart,
                              std::int64_t viewportEnd,
                              std::int64_t duration) noexcept {
    if (viewportEnd > viewportStart) {
        return { viewportStart, viewportEnd };
    }
    return { 0, duration };
}

} // namespace

std::int64_t ScoreOverlayBase::xToMs(int x) const noexcept {
    // MEMO[#step6.2]: leftMarginPx() reserves the leftmost pixels
    // for a non-time region (the piano keyboard column on the
    // StaffWidget). The time axis maps onto [lm, width()).
    const int lm = leftMarginPx();
    const int xInGrid = x - lm;
    const std::int64_t dur = durationMs();
    const int w = width() - lm;
    if (dur <= 0 || w <= 0) return 0;
    const auto [vStart, vEnd] =
        effectiveRange(viewportStartMs_, viewportEndMs_, dur);
    const std::int64_t span = vEnd - vStart;
    if (span <= 0) return 0;
    const std::int64_t ms =
        vStart + static_cast<std::int64_t>(xInGrid) * span / w;
    return std::clamp<std::int64_t>(ms, 0, dur);
}

int ScoreOverlayBase::msToX(std::int64_t ms) const noexcept {
    const int lm = leftMarginPx();
    const std::int64_t dur = durationMs();
    const int w = width() - lm;
    if (dur <= 0 || w <= 0) return 0;
    const auto [vStart, vEnd] =
        effectiveRange(viewportStartMs_, viewportEndMs_, dur);
    const std::int64_t span = vEnd - vStart;
    if (span <= 0) return 0;
    const std::int64_t xInGrid =
        (ms - vStart) * static_cast<std::int64_t>(w) / span;
    // MEMO[#49 smoke]: pre-fix this clamped to [0, w-1], so an
    // off-screen artifact rendered against the leftmost or
    // rightmost column instead of being culled. Every paint
    // site already has an `if (x<0 || x>=width()) continue`
    // bounds check that was being short-circuited by the clamp.
    // We only nudge the exact right-edge case (ms == vEnd gives
    // x == w on integer math) to w-1 so the rightmost column
    // can still paint a marker landing exactly at the boundary.
    if (xInGrid == w) return lm + static_cast<int>(w - 1);
    return lm + static_cast<int>(xInGrid);
}

bool ScoreOverlayBase::isZoomed() const noexcept {
    const std::int64_t dur = durationMs();
    if (dur <= 0) return false;
    const std::int64_t span = viewportSpanMs();
    return span > 0 && span < dur;
}

void ScoreOverlayBase::setViewport(std::int64_t startMs, std::int64_t endMs) {
    const std::int64_t dur = durationMs();

    // (0, 0) — and any inverted / empty pair — clears the viewport
    // back to fit-to-window. This is the convention MainWindow's
    // Ctrl+0 uses.
    if (endMs <= startMs) {
        if (viewportStartMs_ == 0 && viewportEndMs_ == 0) return;
        viewportStartMs_ = 0;
        viewportEndMs_   = 0;
        update();
        emit viewportChanged(viewportStartMs_, viewportEndMs_);
        return;
    }

    // Clamp the pair into [0, dur]. If duration isn't known yet
    // (nothing loaded), pass the values through unclamped — the
    // caller (MainWindow) won't drive zoom on an empty widget
    // anyway.
    std::int64_t s = startMs;
    std::int64_t e = endMs;
    if (dur > 0) {
        s = std::clamp<std::int64_t>(s, 0, dur);
        e = std::clamp<std::int64_t>(e, 0, dur);
    }

    // Enforce the minimum span by extending the END if there's
    // headroom, otherwise the START. Without this, deep zoom-in
    // could collapse the visible range to a single ms — useless
    // for the user and pathological for the divisions above.
    if (e - s < kMinViewportSpanMs) {
        e = s + kMinViewportSpanMs;
        if (dur > 0 && e > dur) {
            e = dur;
            s = std::max<std::int64_t>(0, e - kMinViewportSpanMs);
        }
    }

    if (s == viewportStartMs_ && e == viewportEndMs_) return;
    viewportStartMs_ = s;
    viewportEndMs_   = e;
    update();
    emit viewportChanged(viewportStartMs_, viewportEndMs_);
}

void ScoreOverlayBase::zoomBy(double factor, std::int64_t anchorMs) {
    const std::int64_t dur = durationMs();
    if (dur <= 0 || factor <= 0.0) return;

    // Current visible span: use the viewport when set, otherwise
    // the full duration (fit-to-window). The anchor's pixel
    // fraction within the span determines how the new range
    // straddles `anchorMs`.
    const std::int64_t curStart = viewportEndMs_ > viewportStartMs_
        ? viewportStartMs_ : 0;
    const std::int64_t curEnd   = viewportEndMs_ > viewportStartMs_
        ? viewportEndMs_   : dur;
    const std::int64_t curSpan  = curEnd - curStart;
    if (curSpan <= 0) return;

    const std::int64_t clampedAnchor =
        std::clamp<std::int64_t>(anchorMs, curStart, curEnd);
    const double anchorFrac = curSpan > 0
        ? static_cast<double>(clampedAnchor - curStart) / curSpan
        : 0.5;

    // Compute the new span, clamped between kMinViewportSpanMs and
    // the full duration. Zoom-out beyond fit-to-window is a no-op.
    double newSpanF = static_cast<double>(curSpan) * factor;
    if (newSpanF > static_cast<double>(dur)) {
        // Caller asked to zoom further out than the duration —
        // collapse to fit-to-window (the explicit "default" state).
        setViewport(0, 0);
        return;
    }
    if (newSpanF < static_cast<double>(kMinViewportSpanMs)) {
        newSpanF = static_cast<double>(kMinViewportSpanMs);
    }
    const std::int64_t newSpan = static_cast<std::int64_t>(newSpanF);

    // Place the new span so the anchor stays at the same pixel
    // fraction across the widget. Clamp into [0, dur] without
    // changing the span (slide left or right at the edges).
    std::int64_t newStart = clampedAnchor -
        static_cast<std::int64_t>(anchorFrac * newSpan);
    std::int64_t newEnd   = newStart + newSpan;
    if (newStart < 0) {
        newStart = 0;
        newEnd   = newSpan;
    }
    if (newEnd > dur) {
        newEnd   = dur;
        newStart = std::max<std::int64_t>(0, dur - newSpan);
    }
    setViewport(newStart, newEnd);
}

void ScoreOverlayBase::panBy(std::int64_t deltaMs) {
    if (!isZoomed()) return;
    const std::int64_t dur = durationMs();
    const std::int64_t span = viewportSpanMs();
    std::int64_t newStart = viewportStartMs_ + deltaMs;
    newStart = std::clamp<std::int64_t>(newStart, 0, dur - span);
    setViewport(newStart, newStart + span);
}

// ---- selection setters ---------------------------------------------------

void ScoreOverlayBase::setPositionMs(std::int64_t ms) {
    if (positionMs_ == ms) return;
    positionMs_ = ms;
    update();
}

void ScoreOverlayBase::setSelectedBarline(std::optional<std::size_t> index) {
    if (index.has_value() && barlineModel_
        && *index >= barlineModel_->size()) {
        index = std::nullopt;
    }
    // MEMO: cross-kind mutual exclusion — setting a barline selection
    // clears any active marker AND loop selection. The user wanted
    // "the selected artifact" to be a single concept (the project
    // viewer shows its properties), so even though all three
    // selection slots exist they're never simultaneously populated.
    if (index.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (index.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (index.has_value() && selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    if (selectedBarline_ == index) {
        // Repaint just in case the other-kind clears above changed
        // visible state. update() is a no-op when nothing moved.
        update();
        return;
    }
    selectedBarline_ = index;
    update();
    emit barlineSelectionChanged(selectedBarline_);
}

void ScoreOverlayBase::setSelectedMarkerId(std::optional<std::int64_t> id) {
    if (id.has_value() && markerModel_
        && !markerModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (id.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (id.has_value() && selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    if (selectedMarkerId_ == id) {
        update();
        return;
    }
    selectedMarkerId_ = id;
    update();
    emit markerSelectionChanged(selectedMarkerId_);
}

void ScoreOverlayBase::setSelectedLoopId(std::optional<std::int64_t> id) {
    if (id.has_value() && loopModel_
        && !loopModel_->indexOf(*id).has_value()) {
        id = std::nullopt;
    }
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (id.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (id.has_value() && selectedNoteId_.has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    if (selectedLoopId_ == id) {
        update();
        return;
    }
    selectedLoopId_ = id;
    update();
    emit loopSelectionChanged(selectedLoopId_);
}

void ScoreOverlayBase::setSelectedNoteId(std::optional<std::int64_t> id) {
    const std::int64_t requestedRaw = id.value_or(-1);
    if (id.has_value() && noteModel_
        && !noteModel_->indexOf(*id).has_value()) {
        FLOG_DEBUG("ui.score",
                   "overlay set-selected-note-id={} coerced=stale-id",
                   requestedRaw);
        id = std::nullopt;
    }
    if (id.has_value() && selectedBarline_.has_value()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    if (id.has_value() && selectedMarkerId_.has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    if (id.has_value() && selectedLoopId_.has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    if (selectedNoteId_ == id) {
        update();
        return;
    }
    FLOG_DEBUG("ui.score",
               "overlay set-selected-note-id new={} prev={}",
               id.value_or(-1), selectedNoteId_.value_or(-1));
    selectedNoteId_ = id;
    update();
    emit noteSelectionChanged(selectedNoteId_);
}

void ScoreOverlayBase::setSecondaryAnchorMs(std::optional<std::int64_t> ms) {
    if (secondaryAnchorMs_ == ms) return;
    secondaryAnchorMs_ = ms;
    update();
    emit secondaryAnchorChanged(secondaryAnchorMs_);
}

// ---- model-changed slots -------------------------------------------------

void ScoreOverlayBase::onBarlineModelChanged() {
    if (selectedBarline_.has_value() && barlineModel_
        && *selectedBarline_ >= barlineModel_->size()) {
        selectedBarline_.reset();
        emit barlineSelectionChanged(selectedBarline_);
    }
    update();
}

void ScoreOverlayBase::onMarkerModelChanged() {
    // Position edits keep the same ID, so a selected marker
    // survives a re-sort; only an outright remove drops the
    // selection.
    if (selectedMarkerId_.has_value() && markerModel_
        && !markerModel_->indexOf(*selectedMarkerId_).has_value()) {
        selectedMarkerId_.reset();
        emit markerSelectionChanged(selectedMarkerId_);
    }
    update();
}

void ScoreOverlayBase::onLoopModelChanged() {
    if (selectedLoopId_.has_value() && loopModel_
        && !loopModel_->indexOf(*selectedLoopId_).has_value()) {
        selectedLoopId_.reset();
        emit loopSelectionChanged(selectedLoopId_);
    }
    update();
}

void ScoreOverlayBase::onNoteModelChanged() {
    // setInterval / setPitch keep the same ID, so a selected note
    // survives a re-sort or a wheel-pitch-adjust; only an outright
    // remove drops the selection. Same shape as the other models.
    FLOG_DEBUG("ui.score",
               "overlay note-model-changed widget={} size={} "
               "selected-id={} duration-ms={} width-px={} "
               "viewport=[{},{}]",
               objectName().toStdString(),
               noteModel_ ? noteModel_->size() : 0,
               selectedNoteId_.value_or(-1),
               durationMs(), width(),
               viewportStartMs_, viewportEndMs_);
    if (selectedNoteId_.has_value() && noteModel_
        && !noteModel_->indexOf(*selectedNoteId_).has_value()) {
        selectedNoteId_.reset();
        emit noteSelectionChanged(selectedNoteId_);
    }
    update();
}

// ---- mouse + key handling ------------------------------------------------

void ScoreOverlayBase::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !hasContent()) {
        FLOG_TRACE("ui.score",
                   "overlay mouse-press widget={} button={} ignored "
                   "reason={}",
                   objectName().toStdString(),
                   static_cast<int>(event->button()),
                   hasContent() ? "non-left-button" : "no-content");
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    const int x = event->pos().x();
    // MEMO[#step6.2]: ignore clicks in the leftMarginPx() region
    // (the piano-keyboard column on the staff). The keyboard is a
    // visual / future-tone-preview surface, not a click target for
    // seek or placement.
    if (x < leftMarginPx()) {
        FLOG_TRACE("ui.score",
                   "overlay mouse-press widget={} x={} ignored "
                   "reason=left-margin (kbWidth={})",
                   objectName().toStdString(), x, leftMarginPx());
        event->accept();
        return;
    }
    const auto ms = xToMs(x);
    const bool ctrlHeld =
        (event->modifiers() & Qt::ControlModifier) != 0;
    const auto tolMs = pixelsToMs(kHitTolerancePx);
    FLOG_TRACE("ui.score",
               "overlay mouse-press widget={} x={} y={} ms={} ctrl={} "
               "sel-bar={} sel-marker={} sel-loop={} sel-note={}",
               objectName().toStdString(),
               x, event->pos().y(), ms, ctrlHeld,
               selectedBarline_.value_or(static_cast<std::size_t>(-1)),
               selectedMarkerId_.value_or(-1),
               selectedLoopId_.value_or(-1),
               selectedNoteId_.value_or(-1));

    // MEMO: hit-test priority — by default markers FIRST (labelled
    // and visually atop barlines, so a click on a flag should select
    // the marker), then barlines, then loop edges, then plain seek.
    //
    // Exception (#11): when a loop is currently SELECTED, that
    // loop's edges take priority over markers / barlines that sit
    // at the same x. Reason: a loop created from markers has its
    // edges pinned to those markers' x-coordinates, so without this
    // rule the user could never drag the edges to fine-tune the
    // loop — every press would hit the anchor marker first. The
    // selection is effectively "edit mode" for that loop. To drag
    // a coincident marker, the user clicks the marker in the dock
    // first (which switches selection via the cross-kind mutex).
    // Paint order in WaveformWidget / StaffWidget mirrors this so
    // the selected loop's edges visually sit on top of the marker
    // tick — see "Drag-to-nudge" in docs/architecture.md.

    // MEMO: Ctrl+click semantics — "add as second anchor for loop
    // creation". The current primary's ms is captured into the
    // secondary slot before the new selection is installed; the
    // dashed tick at that ms persists until cleared. Plain click
    // clears the secondary slot as a side effect (the user is
    // starting a fresh selection).
    auto prepareClickStateChange = [&]() {
        if (ctrlHeld) {
            if (const auto primMs = primaryAnchorMs()) {
                setSecondaryAnchorMs(*primMs);
            }
        } else {
            setSecondaryAnchorMs(std::nullopt);
        }
    };

    // 0. Selected loop's edges win first (see exception above).
    if (selectedLoopId_.has_value() && loopModel_) {
        const auto idx = loopModel_->indexOf(*selectedLoopId_);
        if (idx) {
            const auto& loop = loopModel_->loops()[*idx];
            const int xStart = msToX(loop.startMs);
            const int xEnd   = msToX(loop.endMs);
            const int dStart = std::abs(x - xStart);
            const int dEnd   = std::abs(x - xEnd);
            const bool startCloser = (dStart <= dEnd);
            const int  dBest = startCloser ? dStart : dEnd;
            if (dBest <= kEdgeTolerancePx) {
                prepareClickStateChange();
                // Already selected — no setSelectedLoopId call needed
                // (would be a no-op anyway). But the call would also
                // reset secondary-anchor logic via mutual exclusion;
                // prepareClickStateChange() above has already done that.
                if (!ctrlHeld) {
                    dragKind_     = startCloser
                                    ? DragKind::LoopStart
                                    : DragKind::LoopEnd;
                    dragId_       = loop.id;
                    dragPressPos_ = event->pos();
                    dragActive_   = false;
                    dragOriginalMs_ = startCloser
                                      ? loop.startMs : loop.endMs;
                }
                event->accept();
                return;
            }
        }
    }

    // 1. Marker hit?
    if (markerModel_ && markerModel_->size() > 0 && tolMs >= 0) {
        if (const auto markerHit = markerModel_->nearest(ms, tolMs)) {
            const auto idx = markerModel_->indexOf(*markerHit);
            if (idx) {
                prepareClickStateChange();
                setSelectedMarkerId(*markerHit);
                emit seekRequested(
                    markerModel_->markers()[*idx].sourceMs);
                // Drag candidate (#11): plain (non-Ctrl) press on a
                // marker tick arms the drag state machine. If the
                // user releases without crossing the threshold this
                // stays a click; if they drag past it, mouseMove
                // takes over.
                if (!ctrlHeld) {
                    dragKind_       = DragKind::Marker;
                    dragId_         = *markerHit;
                    dragPressPos_   = event->pos();
                    dragActive_     = false;
                    dragOriginalMs_ =
                        markerModel_->markers()[*idx].sourceMs;
                }
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
            // No drag candidate for barlines — issue #11 explicitly
            // defers barline drag to step 6 when the staff widget
            // grows note content.
            event->accept();
            return;
        }
    }

    // 3. Loop edge hit? (issue #11) Markers + barlines win first by
    // priority — a marker sitting at a loop's startMs should be the
    // hit target, not the edge. Only check loop edges if neither
    // anchor kind matched.
    if (const auto edgeHit = hitLoopEdge(x)) {
        prepareClickStateChange();
        setSelectedLoopId(edgeHit->id);
        // Loop-edge selection deliberately does NOT seek — the user
        // is editing region geometry, not navigating playback.
        // Loops never had a hit-test before this PR, so there's no
        // prior behaviour to preserve here.
        if (!ctrlHeld && loopModel_) {
            const auto idx = loopModel_->indexOf(edgeHit->id);
            if (idx) {
                const auto& loop = loopModel_->loops()[*idx];
                dragKind_     = edgeHit->isStart
                                ? DragKind::LoopStart
                                : DragKind::LoopEnd;
                dragId_       = edgeHit->id;
                dragPressPos_ = event->pos();
                dragActive_   = false;
                dragOriginalMs_ = edgeHit->isStart
                                  ? loop.startMs : loop.endMs;
            }
        }
        event->accept();
        return;
    }

    // 4a. Note bar EDGE hit? (issue #60 — drag-to-resize). Edges
    // win before bodies so the user can grab a thin edge zone of
    // a wide bar to resize it; a press in the middle is a move
    // candidate, handled by 4b.
    if (noteModel_ && noteModel_->size() > 0) {
        if (const auto edgeHit = hitNoteEdge(x, event->pos().y())) {
            const auto idx = noteModel_->indexOf(edgeHit->id);
            if (idx) {
                const auto& n = noteModel_->notes()[*idx];
                prepareClickStateChange();
                setSelectedNoteId(edgeHit->id);
                emit seekRequested(n.startMs);
                if (!ctrlHeld) {
                    dragKind_ = edgeHit->isStart
                                ? DragKind::NoteResizeStart
                                : DragKind::NoteResizeEnd;
                    dragId_         = edgeHit->id;
                    dragPressPos_   = event->pos();
                    dragActive_     = false;
                    dragOriginalMs_ = edgeHit->isStart ? n.startMs : n.endMs;
                    noteDragOrigin_ = NoteDragOrigin{n.startMs, n.endMs, n.midi};
                }
                event->accept();
                return;
            }
        }
    }

    // 4b. Note bar BODY hit? (issue #60 — drag-to-move; existing
    // behaviour from #59: also select + seek). On non-Ctrl press,
    // arm a NoteMove drag candidate; on release without a drag
    // it stays a plain selection click.
    if (noteModel_ && noteModel_->size() > 0) {
        if (const auto noteId = hitNote(x, event->pos().y())) {
            const auto idx = noteModel_->indexOf(*noteId);
            if (idx) {
                const auto& n = noteModel_->notes()[*idx];
                prepareClickStateChange();
                setSelectedNoteId(*noteId);
                emit seekRequested(n.startMs);
                if (!ctrlHeld) {
                    dragKind_       = DragKind::NoteMove;
                    dragId_         = *noteId;
                    dragPressPos_   = event->pos();
                    dragActive_     = false;
                    // MEMO[#60]: NoteMove uses press-cursor-ms as the
                    // anchor so the per-move delta (cursorMs − ms)
                    // gives the user's drag distance, NOT the bar's
                    // start position. Grabbing the middle of a bar
                    // and dragging 100 ms right shifts the whole
                    // interval 100 ms; without this we'd snap the
                    // start to the cursor's x instead.
                    dragOriginalMs_ = ms;
                    noteDragOrigin_ = NoteDragOrigin{n.startMs, n.endMs, n.midi};
                }
                event->accept();
                return;
            }
        }
    }

    // 5. No artifact hit. Ctrl+click on empty space is a no-op so
    // the user can't accidentally lose their second anchor by
    // missing a tick. Plain click clears barline + marker
    // selections (and the secondary anchor) and seeks.
    //
    // MEMO: empty-space click deliberately does NOT clear the loop
    // selection. Loops are usually being edited via the dock or
    // via edge-drag, and the user might want to scrub-seek with
    // the loop still selected so they can come back and nudge an
    // edge. To clear a loop selection from the waveform, click on
    // a different artifact (the cross-kind mutex switches it) or
    // clear from the dock. See the smoke test in #11 — the user
    // confirmed this preservation is the intended workflow.
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
    // MEMO[#step6.1]: empty-space click on the score widgets also
    // clears any active note selection — including across widgets.
    // Asymmetry: notes paint only on the staff (in 6.1), so the
    // waveform's own selectedNoteId_ is always empty. If we gated
    // the emit on `if (selectedNoteId_.has_value())`, a click on
    // the waveform would never tell the staff to deselect, leaving
    // the dock's note property page stuck on the previous note —
    // the exact bug surfaced in the user's transcription log.
    //
    // Fix: clear + emit UNCONDITIONALLY in the plain-seek branch.
    // The application's mirror plumbing (MainWindow's
    // onStaffNoteSelectionChanged / onWaveformNoteSelectionChanged)
    // sees the emit and propagates the deselection to the sister
    // widget and the dock. setSelectedNoteId is idempotent on
    // already-empty, so the chain terminates cleanly.
    selectedNoteId_.reset();
    update();
    emit noteSelectionChanged(selectedNoteId_);
    if (secondaryAnchorMs_.has_value()) {
        setSecondaryAnchorMs(std::nullopt);
    }
    emit seekRequested(ms);
    // MEMO[#step6.1]: fire AFTER seekRequested so MainWindow's seek
    // handler has updated playback first. Receivers (the dock's
    // note property page) treat this as the user's "I'm done with
    // the previous focus — give me a clean slate" cue and discard
    // any pending draft. setSelectedNoteId(nullopt) won't reach the
    // dock from here (the selection chain short-circuits when both
    // sides are already null), so this dedicated signal is the only
    // way to propagate the "discard draft" intent on plain seeks.
    emit emptySpaceClicked();
    // MEMO[#step6.2/#60]: additive subclass hook. On the staff,
    // chromaticRowAt(x, y) returns the midi of the row under the
    // press — non-negative means "this empty-grid press could
    // become either a click-to-place OR a drag-to-create". We arm
    // a NoteCreate candidate and defer onEmptySpaceClick to
    // mouseReleaseEvent so the user can drag past the click-vs-
    // drag threshold to specify an explicit length, or not drag
    // and get the default-span placement on release.
    //
    // Other widgets (WaveformWidget) return -1 here, so the call
    // falls through to onEmptySpaceClick immediately — the
    // pre-#60 behaviour.
    const int rowMidi = chromaticRowAt(x, event->pos().y());
    if (rowMidi >= 0) {
        dragKind_       = DragKind::NoteCreate;
        dragId_         = 0;
        dragPressPos_   = event->pos();
        dragActive_     = false;
        dragOriginalMs_ = ms;
        noteDragOrigin_ = NoteDragOrigin{ms, ms, rowMidi};
        noteCreateDeferred_ = true;
    } else {
        onEmptySpaceClick(x, event->pos().y(), ms);
    }
    event->accept();
}

void ScoreOverlayBase::mouseMoveEvent(QMouseEvent* event) {
    // MEMO[#49]: setMouseTracking(true) is on so this fires on
    // unbuttoned motion too. Update the zoom-anchor guide whenever
    // Ctrl is held; clear it otherwise. Cheap: just an optional<int>
    // compare + at most one update() call.
    const bool ctrlHeld =
        QApplication::keyboardModifiers() & Qt::ControlModifier;
    const std::optional<int> newGuide = (ctrlHeld && hasContent())
        ? std::make_optional<int>(static_cast<int>(event->position().x()))
        : std::nullopt;
    updateZoomAnchorGuide(newGuide);

    // We only react when a drag candidate was armed during
    // mousePressEvent — otherwise pass through. (The base no longer
    // depends on mouse-tracking being off for this check.)
    if (dragKind_ == DragKind::None || !hasContent()) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    // Click-vs-drag disambiguation (#11). Until the cursor leaves
    // the start-drag radius around the press point, treat the press
    // as a still-undecided click. Once it crosses, we commit to drag
    // mode for the rest of this gesture.
    if (!dragActive_) {
        const int delta =
            (event->pos() - dragPressPos_).manhattanLength();
        if (delta < QApplication::startDragDistance()) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        dragActive_ = true;
        // MEMO[#22]: ghost is initialised at the artifact's
        // press-time position, then updated per move. Paint code
        // (effectiveMarkerMs / effectiveLoopRange) consults the
        // ghost instead of the model so the dragged tick glides
        // without the model→dock→repaint round-trip per move.
        dragGhost_ = DragGhost{ dragKind_, dragId_, dragOriginalMs_ };
        lastSeekMs_.reset();   // fresh squelch state per drag
        emit dragStarted();
    }

    const auto cursorMs = xToMs(event->pos().x());

    // Defensive: the dragged artifact may have been deleted
    // (Ctrl+Z, Del on a sibling widget, model rebuild). If so the
    // drag has nothing to commit to — drop quietly.
    if (dragKind_ == DragKind::Marker) {
        if (!markerModel_ || !markerModel_->indexOf(dragId_)) {
            dragKind_   = DragKind::None;
            dragActive_ = false;
            dragGhost_.reset();
            emit dragEnded();
            event->accept();
            return;
        }
        if (dragGhost_) dragGhost_->ms = cursorMs;
        emit markerDragRequested(dragId_, cursorMs);
        // Cursor follows the marker live so the user visually
        // tracks where the marker is going. Squelch identical
        // values — Qt can deliver mouse-moves in same-pixel bursts
        // under load, and a duplicate seek is wasted player work
        // plus log noise.
        if (!lastSeekMs_ || *lastSeekMs_ != cursorMs) {
            lastSeekMs_ = cursorMs;
            emit seekRequested(cursorMs);
        }
        update();
        event->accept();
        return;
    }

    // Note drags (issue #60). All four kinds (Move / ResizeStart /
    // ResizeEnd / Create) share the same shape: update the local
    // ghost, repaint, no model write. Commit fires exactly once
    // on release.
    if (dragKind_ == DragKind::NoteMove
        || dragKind_ == DragKind::NoteResizeStart
        || dragKind_ == DragKind::NoteResizeEnd
        || dragKind_ == DragKind::NoteCreate)
    {
        const std::int64_t dur = durationMs();

        if (dragKind_ == DragKind::NoteMove) {
            // Bail if the note vanished mid-drag.
            if (!noteModel_ || !noteModel_->indexOf(dragId_).has_value()) {
                dragKind_   = DragKind::None;
                dragActive_ = false;
                noteDragGhost_.reset();
                emit dragEnded();
                event->accept();
                return;
            }
            // dms is the time-axis offset from press-x.
            const std::int64_t dms = cursorMs - dragOriginalMs_;
            std::int64_t newStart =
                noteDragOrigin_.startMs + dms;
            std::int64_t newEnd =
                noteDragOrigin_.endMs + dms;
            // Clamp by SHIFTING (don't crush the note's length).
            if (newStart < 0) {
                newEnd -= newStart;
                newStart = 0;
            }
            if (dur > 0 && newEnd > dur) {
                newStart -= (newEnd - dur);
                newEnd = dur;
            }
            // Shift held → lock to the original row (time-only).
            // Else follow the cursor's y to a chromatic row.
            const bool shiftHeld =
                QApplication::keyboardModifiers() & Qt::ShiftModifier;
            int newMidi = noteDragOrigin_.midi;
            if (!shiftHeld) {
                const int rowMidi = pixelToMidi(event->pos().y());
                if (rowMidi >= 0) newMidi = rowMidi;
            }
            noteDragGhost_ = NoteDragGhost{
                DragKind::NoteMove, dragId_, newStart, newEnd, newMidi};
        }
        else if (dragKind_ == DragKind::NoteResizeStart) {
            if (!noteModel_ || !noteModel_->indexOf(dragId_).has_value()) {
                dragKind_   = DragKind::None;
                dragActive_ = false;
                noteDragGhost_.reset();
                emit dragEnded();
                event->accept();
                return;
            }
            std::int64_t snapped = cursorMs;
            if (const auto snap = findSnapAnchor(cursorMs)) snapped = *snap;
            if (snapped < 0) snapped = 0;
            // Pin strictly below the partner edge so the note keeps
            // a positive duration; the kept value is the cursor's
            // intent, just clamped one ms inside.
            if (snapped >= noteDragOrigin_.endMs) {
                snapped = noteDragOrigin_.endMs - 1;
            }
            noteDragGhost_ = NoteDragGhost{
                DragKind::NoteResizeStart, dragId_,
                snapped, noteDragOrigin_.endMs, noteDragOrigin_.midi};
        }
        else if (dragKind_ == DragKind::NoteResizeEnd) {
            if (!noteModel_ || !noteModel_->indexOf(dragId_).has_value()) {
                dragKind_   = DragKind::None;
                dragActive_ = false;
                noteDragGhost_.reset();
                emit dragEnded();
                event->accept();
                return;
            }
            std::int64_t snapped = cursorMs;
            if (const auto snap = findSnapAnchor(cursorMs)) snapped = *snap;
            if (dur > 0 && snapped > dur) snapped = dur;
            if (snapped <= noteDragOrigin_.startMs) {
                snapped = noteDragOrigin_.startMs + 1;
            }
            noteDragGhost_ = NoteDragGhost{
                DragKind::NoteResizeEnd, dragId_,
                noteDragOrigin_.startMs, snapped, noteDragOrigin_.midi};
        }
        else {  // NoteCreate
            // Press defined startMs (= dragOriginalMs_) on the
            // press-row. End follows the cursor — but never goes
            // before the start (we reject reverse drags by pinning
            // end one ms past start). Pitch stays on the press row.
            std::int64_t end = cursorMs;
            if (end <= dragOriginalMs_) end = dragOriginalMs_ + 1;
            if (dur > 0 && end > dur) end = dur;
            noteDragGhost_ = NoteDragGhost{
                DragKind::NoteCreate, 0,
                dragOriginalMs_, end, noteDragOrigin_.midi};
        }
        update();
        event->accept();
        return;
    }

    // Loop edge drag — pull the loop's CURRENT range from the
    // model so the partner edge stays put even after a previous
    // mouse-move in this gesture mutated the loop.
    if (!loopModel_) { event->accept(); return; }
    const auto idx = loopModel_->indexOf(dragId_);
    if (!idx) {
        dragKind_   = DragKind::None;
        dragActive_ = false;
        dragGhost_.reset();
        emit dragEnded();
        event->accept();
        return;
    }
    const auto& loop = loopModel_->loops()[*idx];

    // Snap to nearby barline / marker before clamping. The snap
    // result already reflects the user's intent ("magnet to this
    // anchor"), so the clamp below operates on the post-snap value.
    auto snappedMs = cursorMs;
    if (const auto snap = findSnapAnchor(cursorMs)) {
        snappedMs = *snap;
    }

    if (dragKind_ == DragKind::LoopStart) {
        // Clamp newStart strictly less than current endMs. Refusing
        // to commit (rather than pinning to endMs - 1) keeps the
        // model's `start < end` invariant clean and avoids a 1-ms
        // sliver loop that the user almost certainly didn't want.
        if (snappedMs >= loop.endMs) {
            event->accept();
            return;
        }
        if (dragGhost_) dragGhost_->ms = snappedMs;
        emit loopDragRequested(dragId_, snappedMs, loop.endMs);
    } else { // LoopEnd
        if (snappedMs <= loop.startMs) {
            event->accept();
            return;
        }
        if (dragGhost_) dragGhost_->ms = snappedMs;
        emit loopDragRequested(dragId_, loop.startMs, snappedMs);
    }
    update();
    event->accept();
}

void ScoreOverlayBase::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton
        || dragKind_ == DragKind::None)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    const bool wasNoteCreate = (dragKind_ == DragKind::NoteCreate);
    if (dragActive_) {
        // MEMO[#22]: read the artifact's final ms from the GHOST,
        // not the model. Under the unified ghost flow the model
        // is unchanged mid-drag — MainWindow commits exactly once
        // here, on release, so the model's value would still
        // reflect the press-time position when this slot reads it.
        // `dragOriginalMs_` provides the from-side of the
        // from→to log line and the snapshot for undo.
        const auto toMs = dragGhost_ ? dragGhost_->ms : dragOriginalMs_;
        if (dragKind_ == DragKind::Marker
            && markerModel_
            && markerModel_->indexOf(dragId_).has_value())
        {
            emit markerDragCommitted(dragId_, dragOriginalMs_, toMs);
        } else if ((dragKind_ == DragKind::LoopStart
                    || dragKind_ == DragKind::LoopEnd)
                   && loopModel_
                   && loopModel_->indexOf(dragId_).has_value()) {
            const bool isStart = (dragKind_ == DragKind::LoopStart);
            emit loopDragCommitted(dragId_, isStart,
                                   dragOriginalMs_, toMs);
        } else if (noteDragGhost_
                   && (dragKind_ == DragKind::NoteMove
                       || dragKind_ == DragKind::NoteResizeStart
                       || dragKind_ == DragKind::NoteResizeEnd)
                   && noteModel_
                   && noteModel_->indexOf(dragId_).has_value())
        {
            // Single from→to commit for the whole gesture. MainWindow
            // applies it as one setInterval + (maybe) setPitch + one
            // undo entry. The ghost holds the post-clamp values.
            emit noteDragCommitted(
                dragId_,
                noteDragOrigin_.startMs, noteDragOrigin_.endMs,
                noteDragOrigin_.midi,
                noteDragGhost_->startMs, noteDragGhost_->endMs,
                noteDragGhost_->midi);
        } else if (noteDragGhost_
                   && dragKind_ == DragKind::NoteCreate
                   && noteDragGhost_->endMs > noteDragGhost_->startMs)
        {
            emit noteCreateCommitted(noteDragGhost_->startMs,
                                     noteDragGhost_->endMs,
                                     noteDragGhost_->midi);
        }
    } else if (wasNoteCreate && noteCreateDeferred_) {
        // Plain click on empty grid (no drag threshold crossed) —
        // fall through to the deferred default-span placement.
        // Mirror what mousePressEvent used to do before #60 deferred
        // the call. We use the current cursor's ms (release point);
        // press point would also work since no drag occurred, but
        // release-point matches QTest::mouseClick semantics.
        const std::int64_t ms = xToMs(event->pos().x());
        onEmptySpaceClick(event->pos().x(), event->pos().y(), ms);
    }

    const bool wasActive = dragActive_;
    dragKind_       = DragKind::None;
    dragActive_     = false;
    dragId_         = 0;
    dragOriginalMs_ = 0;
    dragGhost_.reset();
    noteDragGhost_.reset();
    noteCreateDeferred_ = false;
    lastSeekMs_.reset();
    if (wasActive) {
        emit dragEnded();
        update();   // repaint without ghost — final position from model
    }
    event->accept();
}

// ---- drag-ghost mirror slots (issue #22) --------------------------------

void ScoreOverlayBase::setMarkerDragGhost(std::int64_t id,
                                          std::int64_t ms) {
    dragGhost_ = DragGhost{ DragKind::Marker, id, ms };
    update();
}

void ScoreOverlayBase::setLoopDragGhost(std::int64_t id,
                                        bool isStart,
                                        std::int64_t ms) {
    dragGhost_ = DragGhost{
        isStart ? DragKind::LoopStart : DragKind::LoopEnd, id, ms };
    update();
}

void ScoreOverlayBase::clearDragGhost() {
    if (!dragGhost_) return;
    dragGhost_.reset();
    update();
}

// ---- test seam ----------------------------------------------------------

std::optional<std::int64_t>
ScoreOverlayBase::dragGhostMs(std::int64_t id) const noexcept {
    if (!dragGhost_) return std::nullopt;
    if (dragGhost_->id != id) return std::nullopt;
    return dragGhost_->ms;
}

// ---- effective-ms helpers (paint code) ----------------------------------

std::int64_t
ScoreOverlayBase::effectiveMarkerMs(std::int64_t id,
                                    std::int64_t modelMs) const noexcept
{
    if (dragGhost_
        && dragGhost_->kind == DragKind::Marker
        && dragGhost_->id == id) {
        return dragGhost_->ms;
    }
    return modelMs;
}

std::pair<std::int64_t, std::int64_t>
ScoreOverlayBase::effectiveLoopRange(std::int64_t id,
                                     std::int64_t modelStartMs,
                                     std::int64_t modelEndMs) const noexcept
{
    if (dragGhost_ && dragGhost_->id == id) {
        if (dragGhost_->kind == DragKind::LoopStart) {
            return { dragGhost_->ms, modelEndMs };
        }
        if (dragGhost_->kind == DragKind::LoopEnd) {
            return { modelStartMs, dragGhost_->ms };
        }
    }
    return { modelStartMs, modelEndMs };
}

std::pair<std::int64_t, std::int64_t>
ScoreOverlayBase::effectiveNoteRange(std::int64_t id,
                                     std::int64_t modelStartMs,
                                     std::int64_t modelEndMs) const noexcept
{
    if (noteDragGhost_ && noteDragGhost_->id == id
        && (noteDragGhost_->kind == DragKind::NoteMove
            || noteDragGhost_->kind == DragKind::NoteResizeStart
            || noteDragGhost_->kind == DragKind::NoteResizeEnd))
    {
        return { noteDragGhost_->startMs, noteDragGhost_->endMs };
    }
    return { modelStartMs, modelEndMs };
}

int ScoreOverlayBase::effectiveNoteMidi(std::int64_t id,
                                        int modelMidi) const noexcept
{
    if (noteDragGhost_ && noteDragGhost_->id == id
        && (noteDragGhost_->kind == DragKind::NoteMove
            || noteDragGhost_->kind == DragKind::NoteResizeStart
            || noteDragGhost_->kind == DragKind::NoteResizeEnd))
    {
        return noteDragGhost_->midi;
    }
    return modelMidi;
}

std::optional<ScoreOverlayBase::PhantomNote>
ScoreOverlayBase::phantomNoteGhost() const noexcept
{
    if (noteDragGhost_ && noteDragGhost_->kind == DragKind::NoteCreate) {
        return PhantomNote{noteDragGhost_->startMs,
                           noteDragGhost_->endMs,
                           noteDragGhost_->midi};
    }
    return std::nullopt;
}

std::optional<ScoreOverlayBase::LoopEdgeHit>
ScoreOverlayBase::hitLoopEdge(int x) const noexcept {
    if (!loopModel_) return std::nullopt;
    int bestDistPx = kEdgeTolerancePx + 1;
    LoopEdgeHit best{};
    bool found = false;
    for (const auto& l : loopModel_->loops()) {
        const int xStart = msToX(l.startMs);
        const int xEnd   = msToX(l.endMs);
        const int dStart = std::abs(x - xStart);
        const int dEnd   = std::abs(x - xEnd);
        if (dStart <= kEdgeTolerancePx && dStart < bestDistPx) {
            bestDistPx = dStart;
            best = {l.id, /*isStart=*/true};
            found = true;
        }
        if (dEnd <= kEdgeTolerancePx && dEnd < bestDistPx) {
            bestDistPx = dEnd;
            best = {l.id, /*isStart=*/false};
            found = true;
        }
    }
    return found ? std::optional<LoopEdgeHit>(best) : std::nullopt;
}

std::optional<std::int64_t>
ScoreOverlayBase::findSnapAnchor(std::int64_t cursorMs) const noexcept {
    const auto tolMs = pixelsToMs(kSnapTolerancePx);
    if (tolMs <= 0) return std::nullopt;

    std::int64_t bestMs = 0;
    std::int64_t bestDist = tolMs + 1;
    if (barlineModel_) {
        for (const auto bMs : barlineModel_->barlines()) {
            const auto dist = std::abs(bMs - cursorMs);
            if (dist <= tolMs && dist < bestDist) {
                bestMs = bMs;
                bestDist = dist;
            }
        }
    }
    if (markerModel_) {
        for (const auto& m : markerModel_->markers()) {
            const auto dist = std::abs(m.sourceMs - cursorMs);
            if (dist <= tolMs && dist < bestDist) {
                bestMs = m.sourceMs;
                bestDist = dist;
            }
        }
    }
    return bestDist <= tolMs
        ? std::optional<std::int64_t>(bestMs)
        : std::nullopt;
}

void ScoreOverlayBase::leaveEvent(QEvent* event) {
    updateZoomAnchorGuide(std::nullopt);
    QWidget::leaveEvent(event);
}

bool ScoreOverlayBase::eventFilter(QObject* /*watched*/, QEvent* event) {
    // Watch global Ctrl press/release so the zoom-anchor guide
    // appears immediately when the user presses Ctrl with the
    // mouse already hovering inside the widget — no mouse motion
    // and no focus required. Cheap: short-circuits unless the
    // event is a Ctrl key event.
    if (event->type() != QEvent::KeyPress
        && event->type() != QEvent::KeyRelease) {
        return false;
    }
    const auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (keyEvent->key() != Qt::Key_Control) return false;

    const bool isPress = (event->type() == QEvent::KeyPress);
    if (isPress) {
        if (!underMouse() || !hasContent()) return false;
        const int x = mapFromGlobal(QCursor::pos()).x();
        if (x < 0 || x >= width()) return false;
        updateZoomAnchorGuide(x);
    } else {
        updateZoomAnchorGuide(std::nullopt);
    }
    return false;   // never consume; pass through to other widgets
}

void ScoreOverlayBase::keyReleaseEvent(QKeyEvent* event) {
    // MEMO[#49 smoke]: pre-fix, the zoom-anchor guide only appeared
    // after the user nudged the mouse — Ctrl pressed while the
    // pointer was stationary inside the widget left the guide
    // invisible. Update the guide on Ctrl release (and the press
    // mirror lives in keyPressEvent below) so it tracks Ctrl
    // independent of mouse motion.
    if (event->key() == Qt::Key_Control) {
        updateZoomAnchorGuide(std::nullopt);
    }
    QWidget::keyReleaseEvent(event);
}

void ScoreOverlayBase::paintZoomAnchorGuide(QPainter& painter) const {
    // Prefer the OWN-widget guide (the user is hovering here with
    // Ctrl held); else fall back to the mirror from the sister
    // widget so the dashed line spans both surfaces continuously.
    int x = -1;
    if (zoomAnchorGuideX_.has_value()) {
        x = *zoomAnchorGuideX_;
    } else if (mirroredZoomAnchorAxisX_.has_value()) {
        x = leftMarginPx() + *mirroredZoomAnchorAxisX_;
    }
    if (x < 0 || x >= width()) return;
    // Faint amber line — distinct from the playback cursor (red),
    // the secondary anchor (cyan), and the loop selection markers.
    QPen guidePen(QColor(255, 175, 60, 180));
    guidePen.setWidth(1);
    guidePen.setStyle(Qt::DashLine);
    painter.save();
    painter.setPen(guidePen);
    painter.drawLine(x, 0, x, height());
    painter.restore();
}

void ScoreOverlayBase::updateZoomAnchorGuide(std::optional<int> newX) {
    if (newX == zoomAnchorGuideX_) return;
    zoomAnchorGuideX_ = newX;
    update();
    // Emit the TIME-AXIS x (widget-x minus the keyboard column),
    // not source-ms. Integer copy → integer copy with no precision
    // loss; ms quantises sub-millisecond positions that matter at
    // deep zoom and would visibly desync the sister's guide.
    const std::optional<int> axisX =
        newX.has_value()
        ? std::make_optional<int>(*newX - leftMarginPx())
        : std::nullopt;
    emit zoomAnchorGuideChanged(axisX);
}

void ScoreOverlayBase::setMirroredZoomAnchorAxisX(
    std::optional<int> axisX)
{
    if (axisX == mirroredZoomAnchorAxisX_) return;
    mirroredZoomAnchorAxisX_ = axisX;
    update();   // no signal emit — receiver-only; breaks the loop
}

void ScoreOverlayBase::wheelEvent(QWheelEvent* event) {
    // Ctrl+wheel zooms; Shift+wheel pans; everything else falls
    // through to QWidget so an outer QScrollArea (none today, but
    // future-friendly) can handle vertical scroll without being
    // hijacked.
    const Qt::KeyboardModifiers mods = event->modifiers();
    const QPoint  pixelDelta = event->pixelDelta();
    const QPoint  angleDelta = event->angleDelta();

    if (mods & Qt::ControlModifier) {
        if (!hasContent()) { event->ignore(); return; }
        // angleDelta is in eighths of a degree; one standard notch
        // is 120 (15°). Zoom by √2 per notch — three notches gives
        // ~2.8× change, fast enough to feel responsive without
        // overshooting.
        const int dy = angleDelta.y() != 0
            ? angleDelta.y() : pixelDelta.y();
        if (dy == 0) { event->ignore(); return; }
        const double notches = static_cast<double>(dy) / 120.0;
        // dy > 0 (wheel up) zooms IN -> factor < 1.
        const double factor = std::pow(1.0 / std::sqrt(2.0), notches);
        // MEMO[#49 smoke]: anchor on the GUIDE pixel, not the
        // wheel event's qreal position(). The guide is what the
        // user sees and aims; QWheelEvent::position() can sit a
        // fraction of a pixel off the cached guide x, and the
        // sub-pixel offset compounds every wheel notch — by the
        // third step the targeted feature has visibly drifted off
        // the guide. Anchoring directly on the cached guide pixel
        // makes the visual reference equal to the math reference,
        // so the feature stays under the guide forever.
        const int anchorX = zoomAnchorGuideX_.has_value()
            ? *zoomAnchorGuideX_
            : static_cast<int>(event->position().x());
        const std::int64_t anchorMs = xToMs(anchorX);
        zoomBy(factor, anchorMs);
        event->accept();
        return;
    }

    if (mods & Qt::ShiftModifier) {
        if (!isZoomed()) { event->ignore(); return; }
        // Pan by ~10% of the visible span per notch. dy > 0 (wheel
        // up) pans LEFT (toward earlier time), matching the
        // direction convention of horizontal scroll on a trackpad.
        const int dy = angleDelta.y() != 0
            ? angleDelta.y() : pixelDelta.y();
        if (dy == 0) { event->ignore(); return; }
        const double notches = static_cast<double>(dy) / 120.0;
        const std::int64_t step =
            static_cast<std::int64_t>(viewportSpanMs() * 0.1 * notches);
        panBy(-step);
        emit userScrolled();
        event->accept();
        return;
    }

    event->ignore();
}

void ScoreOverlayBase::keyPressEvent(QKeyEvent* event) {
    // MEMO[#49 smoke]: surface the zoom-anchor guide as soon as
    // the user presses Ctrl with the mouse already inside the
    // widget — pre-fix it only appeared on the next mouse move.
    // mapFromGlobal(QCursor::pos()) gives the live pointer
    // position even though no QMouseEvent fires for a stationary
    // Ctrl-press.
    if (event->key() == Qt::Key_Control && underMouse() && hasContent()) {
        const int x = mapFromGlobal(QCursor::pos()).x();
        if (x >= 0 && x < width()) {
            updateZoomAnchorGuide(x);
        }
    }

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
                if (const auto prevId = markerModel_->idAt(*idx - 1)) {
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
                if (const auto nextId = markerModel_->idAt(*idx + 1)) {
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
