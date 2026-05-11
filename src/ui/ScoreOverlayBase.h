// ScoreOverlayBase — common base class for the score overlay widgets
// (WaveformWidget, StaffWidget). Holds everything the two widgets
// genuinely share: the three artifact models (barlines / markers /
// loops), the cross-kind mutually-exclusive selection state, the
// secondary anchor for loop creation, the playhead position, the
// click hit-test priority, and the keyboard navigation logic.
//
// Concrete subclasses provide just the bits that genuinely differ:
//
//   * `xToMs(int)` and `msToX(int64_t)` — coord transforms. Each
//     subclass derives these from its own data source (the
//     waveform's `WaveformOverview::duration()` or the staff's
//     `durationMs_` field). Pure virtual.
//   * `hasContent()` — has anything been loaded yet? The waveform
//     needs an overview; the staff needs a non-zero duration. The
//     base's mousePressEvent uses this to gate click handling.
//   * `paintEvent` — each subclass renders its own visual on top
//     of the shared selection / secondary-anchor state which the
//     base exposes via getters.
//
// MEMO[refactor]: this class is the result of issue #12. Before
// the extraction, WaveformWidget and StaffWidget mirrored ~380
// lines of selection / event / model-wiring logic each; the
// upcoming drag-to-adjust (#11) and future zoom / scroll work
// would have doubled that. See the issue for the full rationale.

#pragma once

#include <QPoint>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class QKeyEvent;
class QMouseEvent;
class QPainter;
class QWheelEvent;

namespace fiddler::score {
class BarlineModel;
class LoopModel;
class MarkerModel;
}

namespace fiddler::ui {

class ScoreOverlayBase : public QWidget {
    Q_OBJECT
public:
    explicit ScoreOverlayBase(QWidget* parent = nullptr);
    ~ScoreOverlayBase() override;

    // ---- model attachment ---------------------------------------

    void setBarlineModel(std::shared_ptr<const score::BarlineModel> model);
    [[nodiscard]] std::shared_ptr<const score::BarlineModel>
        barlineModel() const noexcept { return barlineModel_; }

    void setMarkerModel(std::shared_ptr<const score::MarkerModel> model);
    [[nodiscard]] std::shared_ptr<const score::MarkerModel>
        markerModel() const noexcept { return markerModel_; }

    void setLoopModel(std::shared_ptr<const score::LoopModel> model);
    [[nodiscard]] std::shared_ptr<const score::LoopModel>
        loopModel() const noexcept { return loopModel_; }

    // ---- read-only state ----------------------------------------

    [[nodiscard]] std::int64_t positionMs() const noexcept { return positionMs_; }
    [[nodiscard]] std::optional<std::size_t>
        selectedBarline() const noexcept { return selectedBarline_; }
    [[nodiscard]] std::optional<std::int64_t>
        selectedMarkerId() const noexcept { return selectedMarkerId_; }
    [[nodiscard]] std::optional<std::int64_t>
        selectedLoopId() const noexcept { return selectedLoopId_; }
    [[nodiscard]] std::optional<std::int64_t>
        secondaryAnchorMs() const noexcept { return secondaryAnchorMs_; }

    // Resolve the current primary selection (barline or marker) to
    // its source-time ms, or nullopt if nothing primary is selected.
    // The L shortcut in MainWindow uses this to read whatever the
    // user has selected — across both widget kinds — without the
    // caller needing to know which widget owns the selection.
    [[nodiscard]] std::optional<std::int64_t>
        primaryAnchorMs() const noexcept;

    // Coord transforms. The math lives in the base class and uses
    // the viewport range below; subclasses only have to supply the
    // total source duration via `durationMs()`. With no viewport
    // explicitly set the math falls back to "fit-to-window" — the
    // pre-#49 behaviour exactly.
    [[nodiscard]] std::int64_t xToMs(int x) const noexcept;
    [[nodiscard]] int          msToX(std::int64_t ms) const noexcept;

    // Source-time duration of the loaded content, in milliseconds.
    // Returns 0 when nothing has been loaded yet. Each subclass
    // sources this from its own data — the WaveformWidget reads
    // `WaveformOverview::duration()`, the StaffWidget exposes a
    // `setDurationMs` setter the GUI calls on every load.
    [[nodiscard]] virtual std::int64_t durationMs() const noexcept = 0;

    // ---- viewport / zoom (#49) ----------------------------------
    //
    // The viewport is the source-time range [start, end) the widget
    // maps to its full pixel width. With viewport unset (start == 0
    // && end == 0) or invalid (end <= start) the math falls back to
    // "fit to window" — the entire source duration on the canvas.
    //
    // MainWindow owns the canonical pair and pushes it to both
    // widgets in lockstep via setViewport. Direct callers (tests,
    // future scroll automations) use it the same way.
    //
    // GUI-only state; the audio pipeline is completely untouched.
    [[nodiscard]] std::int64_t viewportStartMs() const noexcept {
        return viewportStartMs_;
    }
    [[nodiscard]] std::int64_t viewportEndMs() const noexcept {
        return viewportEndMs_;
    }
    [[nodiscard]] std::int64_t viewportSpanMs() const noexcept {
        return viewportEndMs_ > viewportStartMs_
            ? viewportEndMs_ - viewportStartMs_ : 0;
    }
    // True iff a strictly-narrower-than-duration viewport is in
    // effect. Used by MainWindow to show / hide the scrollbar.
    [[nodiscard]] bool isZoomed() const noexcept;

    // Multiply the viewport's span by `factor` keeping `anchorMs`
    // pinned at the same pixel. factor < 1 zooms IN (smaller span),
    // factor > 1 zooms OUT. Clamped at fit-to-window on the wide
    // end and kMinViewportSpanMs on the narrow end. If the viewport
    // wasn't set yet, the current "fit" range is used as the
    // starting span. Emits viewportChanged when the range changes.
    void zoomBy(double factor, std::int64_t anchorMs);

    // Slide the viewport by `deltaMs` source-time. Clamped so the
    // viewport stays inside [0, durationMs]. No-op when not zoomed.
    void panBy(std::int64_t deltaMs);

    // Test seam (#22) — query the live drag-ghost ms for an
    // artifact, or nullopt if no ghost matches it. Used by
    // integration tests to verify that the sister widget mirrored
    // a partner-driven drag.
    [[nodiscard]] std::optional<std::int64_t>
        dragGhostMs(std::int64_t id) const noexcept;

public slots:
    // Update the visible source-time range. Clamps to [0, durationMs()]
    // and enforces a minimum span (kMinViewportSpanMs) so the pixels-
    // per-ms factor stays well-defined even at deep zoom. Falls back
    // to fit-to-window when called with (0, 0) or any inverted pair.
    // Emits viewportChanged on actual changes; cheap no-op otherwise.
    void setViewport(std::int64_t startMs, std::int64_t endMs);
    void setPositionMs(std::int64_t ms);
    // MEMO: setting any one selection clears the others (cross-kind
    // mutual exclusion). All four setters share that contract.
    void setSelectedBarline(std::optional<std::size_t> index);
    void setSelectedMarkerId(std::optional<std::int64_t> id);
    void setSelectedLoopId(std::optional<std::int64_t> id);
    void setSecondaryAnchorMs(std::optional<std::int64_t> ms);

    // Drag-ghost mirrors (issue #22). When the partner widget is
    // driving a drag, MainWindow forwards its live ghost position
    // to this widget so the dragged artifact glides under the
    // cursor on BOTH surfaces, not just the one being touched.
    // The model isn't written during the drag — the ghost is a
    // paint-only override; the model commits once on release.
    void setMarkerDragGhost(std::int64_t id, std::int64_t ms);
    void setLoopDragGhost  (std::int64_t id, bool isStart, std::int64_t ms);
    void clearDragGhost();

signals:
    void seekRequested(std::int64_t ms);
    // Fired after a setViewport call that actually changed the
    // visible range. Used by MainWindow to keep the scrollbar in
    // sync and to broadcast the new viewport to the sister widget.
    void viewportChanged(std::int64_t startMs, std::int64_t endMs);
    // Fired on Shift+wheel pan during playback — MainWindow uses
    // this to flip the "Follow playback" toggle off so the manual
    // scroll isn't immediately undone by the next position tick.
    void userScrolled();
    void barlineSelectionChanged(std::optional<std::size_t> index);
    void markerSelectionChanged (std::optional<std::int64_t> id);
    void loopSelectionChanged   (std::optional<std::int64_t> id);
    void secondaryAnchorChanged (std::optional<std::int64_t> ms);
    void barlineDeleteRequested (std::size_t index);
    void markerDeleteRequested  (std::int64_t id);

    // Live drag updates (issues #11, #22). Fired on every mouse-move
    // past the click-vs-drag threshold. MainWindow forwards these to
    // the SISTER widget's drag-ghost slots so the artifact glides
    // under the cursor on both surfaces. The model is NOT written
    // during the drag — the ghost is a paint-only override; the
    // single model write happens on release via *DragCommitted.
    //
    // MEMO[#22]: pre-#22 this signal triggered a per-move
    // setPosition / setRange that re-sorted the model and rebuilt
    // the dock tree, starving the GUI thread. Decoupling the live
    // path from model writes is what makes the drag visually
    // smooth at typical mouse speeds.
    void markerDragRequested(std::int64_t id, std::int64_t newMs);
    void loopDragRequested  (std::int64_t id,
                             std::int64_t newStartMs,
                             std::int64_t newEndMs);

    // One-shot signals bracketing an active drag — fired when the
    // user crosses the click-vs-drag threshold (started) and on
    // release after a successful commit (ended). MainWindow uses
    // these to pause the playback-position poll while the drag is
    // in flight, removing one more source of mid-drag repaint
    // noise.
    void dragStarted();
    void dragEnded();

    // Fired ONCE on mouse release after a drag has actually committed
    // (i.e. the threshold was crossed). MainWindow logs this; the
    // intermediate per-move signals stay quiet to avoid log flooding.
    // `fromMs` is the artifact's position at press; `toMs` is the
    // final landed value (post-snap, post-clamp).
    void markerDragCommitted(std::int64_t id,
                             std::int64_t fromMs,
                             std::int64_t toMs);
    // For loops the "edge" boolean disambiguates start vs end so the
    // log line and any future replay machinery can tell which side
    // moved.
    void loopDragCommitted(std::int64_t id,
                           bool        isStartEdge,
                           std::int64_t fromMs,
                           std::int64_t toMs);

protected:
    void mousePressEvent  (QMouseEvent* event) override;
    void mouseMoveEvent   (QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent    (QKeyEvent*   event) override;
    void keyReleaseEvent  (QKeyEvent*   event) override;
    void wheelEvent       (QWheelEvent* event) override;
    void leaveEvent       (QEvent*      event) override;
    // Application-wide filter so we catch Ctrl press / release
    // regardless of which widget has keyboard focus — important
    // because the user is typically hovering with the mouse, not
    // clicked. Updates the zoom-anchor guide live.
    bool eventFilter      (QObject* watched, QEvent* event) override;

    // Paint the "zoom anchor" guide — a thin vertical line at the
    // current mouse position when Ctrl is held over the widget.
    // Subclasses' paintEvent call this at the end so the guide
    // floats above all overlays. No-op when no Ctrl is held.
    void paintZoomAnchorGuide(QPainter& painter) const;

    // Subclass declares "I have something loaded; clicks should be
    // processed". Without content the base treats clicks as
    // QWidget defaults (no seek, no select).
    [[nodiscard]] virtual bool hasContent() const noexcept = 0;

    // Pixel-distance → source-ms helper, used by the mouse hit-test.
    // Computed via xToMs so it scales with whatever zoom / viewport
    // the subclass implements.
    [[nodiscard]] std::int64_t pixelsToMs(int px) const noexcept;

    // Drag-ghost overrides for paint code (issue #22). Subclass
    // paintMarkers / paintLoops call these instead of reading
    // sourceMs / startMs / endMs from the model directly. If a
    // drag ghost matches the artifact, the ghost's ms wins; else
    // the model's value is returned unchanged. This is what makes
    // the dragged tick glide under the cursor without writing to
    // the model on every mouse-move.
    [[nodiscard]] std::int64_t effectiveMarkerMs(std::int64_t id,
                                                 std::int64_t modelMs) const noexcept;
    [[nodiscard]] std::pair<std::int64_t, std::int64_t>
        effectiveLoopRange(std::int64_t id,
                           std::int64_t modelStartMs,
                           std::int64_t modelEndMs) const noexcept;

private slots:
    void onBarlineModelChanged();
    void onMarkerModelChanged();
    void onLoopModelChanged();

private:
    // Drag state (issue #11). What was hit on press, where the press
    // landed (for the click-vs-drag threshold), and whether we've
    // crossed it yet. `dragOriginalMs_` snapshots the artifact's
    // position at press so the mouseRelease commit signal can report
    // a meaningful from→to pair.
    enum class DragKind { None, Marker, LoopStart, LoopEnd };
    DragKind     dragKind_       = DragKind::None;
    std::int64_t dragId_         = 0;
    QPoint       dragPressPos_;
    bool         dragActive_     = false;
    std::int64_t dragOriginalMs_ = 0;

    // Per-move snapshot of the dragged artifact's "where would it
    // land if released right now" position. Set on press (with
    // `dragOriginalMs_`), updated on every mouseMove past the
    // click-vs-drag threshold, cleared on release / cancel.
    // Subclass paint code consults `effectiveMarkerMs` /
    // `effectiveLoopRange` rather than the model directly so the
    // dragged tick paints at the ghost ms without a model write.
    //
    // The same struct holds the SISTER widget's ghost when the
    // partner is driving — both widgets paint with the same data.
    struct DragGhost {
        DragKind     kind = DragKind::None;
        std::int64_t id   = 0;
        std::int64_t ms   = 0;
    };
    std::optional<DragGhost> dragGhost_;

    // Cache of the last seekRequested value so a per-move drag
    // doesn't keep firing identical seeks (mouse-move events can
    // burst at the same pixel, especially under load — log noise
    // and wasted player work).
    std::optional<std::int64_t> lastSeekMs_;

    // Hit-test the loop band edges at pixel x. Returns the loop id
    // and which edge (start/end) was hit, within `kEdgeTolerancePx`.
    // Used by mousePressEvent (after the marker / barline checks
    // since markers visually sit atop barlines and edges).
    struct LoopEdgeHit { std::int64_t id; bool isStart; };
    [[nodiscard]] std::optional<LoopEdgeHit>
        hitLoopEdge(int x) const noexcept;

    // Find the nearest barline OR marker to `cursorMs` within
    // `pixelsToMs(kSnapTolerancePx)`. Used by loop-edge drag to
    // implement live snap-to-anchor (the magnet feel from DAWs).
    // Returns the snapped ms, or nullopt if no anchor is in range.
    [[nodiscard]] std::optional<std::int64_t>
        findSnapAnchor(std::int64_t cursorMs) const noexcept;

    std::shared_ptr<const score::BarlineModel> barlineModel_;
    std::shared_ptr<const score::MarkerModel>  markerModel_;
    std::shared_ptr<const score::LoopModel>    loopModel_;
    std::int64_t                               positionMs_ = 0;
    std::optional<std::size_t>                 selectedBarline_;
    std::optional<std::int64_t>                selectedMarkerId_;
    std::optional<std::int64_t>                selectedLoopId_;
    std::optional<std::int64_t>                secondaryAnchorMs_;

    // Viewport (#49). Both are stored in source-time milliseconds.
    // Defaults of (0, 0) mean "fit to window" — the math falls back
    // to mapping [0, durationMs()] across the full pixel width.
    std::int64_t viewportStartMs_ = 0;
    std::int64_t viewportEndMs_   = 0;

    // Minimum span we'll accept from setViewport. Picked so that
    // even on a wide widget (~2000 px) each pixel still spans at
    // least ~25 µs of source time — fine-grained enough for sub-ms
    // precision without the divisions in xToMs/msToX going degenerate.
    static constexpr std::int64_t kMinViewportSpanMs = 50;

    // Pixel-x of the mouse when Ctrl is held over the widget — the
    // anchor that Ctrl+wheel will pivot around. Drawn as a thin
    // vertical guide so the user can see WHERE the zoom will land
    // before they spin the wheel. nullopt when Ctrl isn't held or
    // the mouse is outside the widget. Updated from mouseMoveEvent;
    // refreshed by enterEvent so a Ctrl-down-then-enter case
    // doesn't miss it.
    std::optional<int> zoomAnchorGuideX_;
};

} // namespace fiddler::ui
