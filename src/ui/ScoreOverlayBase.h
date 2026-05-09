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

    // Coord transforms — each subclass derives from its own data
    // source. Pure virtual: ScoreOverlayBase has no concept of
    // duration on its own.
    [[nodiscard]] virtual std::int64_t xToMs(int x) const noexcept = 0;
    [[nodiscard]] virtual int          msToX(std::int64_t ms) const noexcept = 0;

public slots:
    void setPositionMs(std::int64_t ms);
    // MEMO: setting any one selection clears the others (cross-kind
    // mutual exclusion). All four setters share that contract.
    void setSelectedBarline(std::optional<std::size_t> index);
    void setSelectedMarkerId(std::optional<std::int64_t> id);
    void setSelectedLoopId(std::optional<std::int64_t> id);
    void setSecondaryAnchorMs(std::optional<std::int64_t> ms);

signals:
    void seekRequested(std::int64_t ms);
    void barlineSelectionChanged(std::optional<std::size_t> index);
    void markerSelectionChanged (std::optional<std::int64_t> id);
    void loopSelectionChanged   (std::optional<std::int64_t> id);
    void secondaryAnchorChanged (std::optional<std::int64_t> ms);
    void barlineDeleteRequested (std::size_t index);
    void markerDeleteRequested  (std::int64_t id);

    // Live drag updates (issue #11). Fired on every mouse-move past
    // the click-vs-drag threshold so the dock + sibling widgets see
    // the artifact track the cursor in real time. MainWindow turns
    // these into MarkerModel::setPosition / LoopModel::setRange
    // calls — the widget holds only `shared_ptr<const>` handles, so
    // model writes have to round-trip through the parent.
    void markerDragRequested(std::int64_t id, std::int64_t newMs);
    void loopDragRequested  (std::int64_t id,
                             std::int64_t newStartMs,
                             std::int64_t newEndMs);

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

    // Subclass declares "I have something loaded; clicks should be
    // processed". Without content the base treats clicks as
    // QWidget defaults (no seek, no select).
    [[nodiscard]] virtual bool hasContent() const noexcept = 0;

    // Pixel-distance → source-ms helper, used by the mouse hit-test.
    // Computed via xToMs so it scales with whatever zoom / viewport
    // the subclass implements.
    [[nodiscard]] std::int64_t pixelsToMs(int px) const noexcept;

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
};

} // namespace fiddler::ui
