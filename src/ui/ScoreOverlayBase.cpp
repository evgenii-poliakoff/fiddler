#include "ui/ScoreOverlayBase.h"

#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cstdlib>

namespace fiddler::ui {

namespace {

// Click-to-select tolerance in pixels — used by mousePressEvent's
// hit test for barlines and markers. Same value the widgets used
// before the refactor (#12) so user feel is unchanged.
constexpr int kHitTolerancePx = 5;

// Loop-edge hit-test tolerance — slightly larger than the marker /
// barline tolerance because edges are 1-px lines (not flagged ticks)
// and benefit from a more generous catch radius. Issue #11.
constexpr int kEdgeTolerancePx = 6;

// Snap-to-anchor tolerance for loop-edge dragging — same value as
// the edge hit radius so the affordances match: if you can grab the
// edge from N px, you can also park it on an anchor from N px.
constexpr int kSnapTolerancePx = 6;

} // namespace

ScoreOverlayBase::ScoreOverlayBase(QWidget* parent) : QWidget(parent) {
    // StrongFocus so the widget can receive arrow / Esc / Del keys
    // after a click gives it focus.
    setFocusPolicy(Qt::StrongFocus);
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
    // MEMO: derive the pixel→ms scale by sampling xToMs at the
    // origin and at `px`. Using the virtual lets a future
    // zoom-aware subclass return a tighter ms-per-pixel value
    // automatically.
    return xToMs(px) - xToMs(0);
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
    if (selectedBarline_ == index) {
        // Repaint just in case the marker/loop-clear above changed
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
    if (selectedLoopId_ == id) {
        update();
        return;
    }
    selectedLoopId_ = id;
    update();
    emit loopSelectionChanged(selectedLoopId_);
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

// ---- mouse + key handling ------------------------------------------------

void ScoreOverlayBase::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !hasContent()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    const int x = event->pos().x();
    const auto ms = xToMs(x);
    const bool ctrlHeld =
        (event->modifiers() & Qt::ControlModifier) != 0;
    const auto tolMs = pixelsToMs(kHitTolerancePx);

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

    // 4. No artifact hit. Ctrl+click on empty space is a no-op so
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
    if (secondaryAnchorMs_.has_value()) {
        setSecondaryAnchorMs(std::nullopt);
    }
    emit seekRequested(ms);
    event->accept();
}

void ScoreOverlayBase::mouseMoveEvent(QMouseEvent* event) {
    // Mouse-tracking is off (the default) so this only fires while
    // a button is pressed. We only react when a drag candidate was
    // armed during mousePressEvent — otherwise pass through.
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
    }

    const auto cursorMs = xToMs(event->pos().x());

    // Defensive: the dragged artifact may have been deleted
    // (Ctrl+Z, Del on a sibling widget, model rebuild). If so the
    // drag has nothing to commit to — drop quietly.
    if (dragKind_ == DragKind::Marker) {
        if (!markerModel_ || !markerModel_->indexOf(dragId_)) {
            dragKind_ = DragKind::None;
            event->accept();
            return;
        }
        emit markerDragRequested(dragId_, cursorMs);
        // Cursor follows the marker live so the user visually
        // tracks where the marker is going.
        emit seekRequested(cursorMs);
        event->accept();
        return;
    }

    // Loop edge drag — pull the loop's CURRENT range from the
    // model so the partner edge stays put even after a previous
    // mouse-move in this gesture mutated the loop.
    if (!loopModel_) { event->accept(); return; }
    const auto idx = loopModel_->indexOf(dragId_);
    if (!idx) {
        dragKind_ = DragKind::None;
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
        emit loopDragRequested(dragId_, snappedMs, loop.endMs);
    } else { // LoopEnd
        if (snappedMs <= loop.startMs) {
            event->accept();
            return;
        }
        emit loopDragRequested(dragId_, loop.startMs, snappedMs);
    }
    event->accept();
}

void ScoreOverlayBase::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton
        || dragKind_ == DragKind::None)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (dragActive_) {
        // Read the artifact's final ms from the model — that's the
        // post-snap, post-clamp value (every successful move emitted
        // a request that MainWindow committed). `dragOriginalMs_`
        // gives us the from-side of the from→to log line.
        if (dragKind_ == DragKind::Marker
            && markerModel_)
        {
            if (const auto idx = markerModel_->indexOf(dragId_)) {
                const auto toMs =
                    markerModel_->markers()[*idx].sourceMs;
                emit markerDragCommitted(dragId_, dragOriginalMs_, toMs);
            }
        } else if (loopModel_) {
            if (const auto idx = loopModel_->indexOf(dragId_)) {
                const auto& loop = loopModel_->loops()[*idx];
                const bool isStart = (dragKind_ == DragKind::LoopStart);
                const auto toMs = isStart ? loop.startMs : loop.endMs;
                emit loopDragCommitted(dragId_, isStart,
                                       dragOriginalMs_, toMs);
            }
        }
    }

    dragKind_       = DragKind::None;
    dragActive_     = false;
    dragId_         = 0;
    dragOriginalMs_ = 0;
    event->accept();
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

void ScoreOverlayBase::keyPressEvent(QKeyEvent* event) {
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
