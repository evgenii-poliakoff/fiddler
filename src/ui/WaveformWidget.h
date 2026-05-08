// WaveformWidget — paints a WaveformOverview's peaks plus a playhead
// cursor, and turns user clicks into seek requests.
//
// Decoupled from Player by design: the widget consumes a
// shared_ptr<const WaveformOverview> and emits seekRequested(ms);
// the parent (MainWindow) wires the slot back to Player::seek. This
// shape lets future views (mini-strip, second waveform pane, the
// staff overlay landing in step 5) drop in against the same data
// without touching Player or each other.
//
// Source-time milliseconds is the universal coordinate. xToMs() and
// msToX() are public so step 5's overlay code can map between pixel
// columns and source-time positions without reimplementing the math.

#pragma once

#include "audio/WaveformOverview.h"

#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

namespace fiddler::score {
class BarlineModel;
class LoopModel;
class MarkerModel;
}

namespace fiddler::ui {

class WaveformWidget : public QWidget {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget* parent = nullptr);
    ~WaveformWidget() override;

    void setOverview(std::shared_ptr<const audio::WaveformOverview> ov);
    [[nodiscard]] std::shared_ptr<const audio::WaveformOverview>
        overview() const noexcept { return overview_; }

    // Barline overlay. The widget paints ticks for every barline in
    // the model and supports click-to-select / key-nav / Del. Pass
    // nullptr to detach. The widget connects to the model's
    // `changed()` signal to repaint and to invalidate the selection
    // if the selected entry is removed.
    void setBarlineModel(std::shared_ptr<const score::BarlineModel> model);
    [[nodiscard]] std::shared_ptr<const score::BarlineModel>
        barlineModel() const noexcept { return barlineModel_; }

    // Marker overlay. Same shape as the barline overlay but tracks
    // selection by the marker's stable int64_t ID — markers can be
    // moved (setPosition) and renamed (rename) which would change
    // their index but not their ID.
    //
    // MEMO: barline-selection and marker-selection are mutually
    // exclusive — selecting a marker clears any barline selection
    // and vice versa. The user wanted "the selected artifact" to be
    // a single concept (the property viewer in PR step 5.5 shows
    // its properties), so we enforce that here. Both selections
    // are stored and reported separately, but at most one is
    // populated at any given moment.
    void setMarkerModel(std::shared_ptr<const score::MarkerModel> model);
    [[nodiscard]] std::shared_ptr<const score::MarkerModel>
        markerModel() const noexcept { return markerModel_; }

    // Loop overlay. Loops are drawn as translucent bands spanning
    // [startMs, endMs). Selection is currently dock-driven — the
    // widget renders the selected loop with higher opacity but does
    // not originate loop selection from clicks (loops are big
    // regions; clicking inside one would conflict with seek-anywhere
    // affordance). Pass nullptr to detach.
    //
    // MEMO: loop-selection participates in the same mutual-exclusion
    // rule as barlines and markers — at most one of {barline, marker,
    // loop} is selected at any time. Setting any one clears the
    // others, with loopSelectionChanged emitted when the loop slot
    // becomes empty as a side effect.
    void setLoopModel(std::shared_ptr<const score::LoopModel> model);
    [[nodiscard]] std::shared_ptr<const score::LoopModel>
        loopModel() const noexcept { return loopModel_; }

    [[nodiscard]] std::int64_t positionMs() const noexcept { return positionMs_; }
    [[nodiscard]] std::optional<std::size_t>
        selectedBarline() const noexcept { return selectedBarline_; }
    [[nodiscard]] std::optional<std::int64_t>
        selectedMarkerId() const noexcept { return selectedMarkerId_; }
    [[nodiscard]] std::optional<std::int64_t>
        selectedLoopId() const noexcept { return selectedLoopId_; }

    // Coordinate transforms — public so overlays can map pixels to
    // source time and back. Both clamp to the widget's current bounds
    // and the overview's duration; calling them when no overview is
    // set returns 0.
    [[nodiscard]] std::int64_t xToMs(int x) const noexcept;
    [[nodiscard]] int          msToX(std::int64_t ms) const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

public slots:
    void setPositionMs(std::int64_t ms);
    // Programmatic selection setter. Used by MainWindow to keep the
    // staff and waveform showing the same selected barline.
    // MEMO: setting a barline selection clears any marker selection
    // (mutual exclusion).
    void setSelectedBarline(std::optional<std::size_t> index);
    // Programmatic marker selection setter. Used by the project
    // viewer + MainWindow to mirror selection across waveform / staff
    // / dock. MEMO: setting a marker selection clears any barline
    // selection (mutual exclusion).
    void setSelectedMarkerId(std::optional<std::int64_t> id);
    // Programmatic loop selection setter. Used by the project viewer
    // dock to mirror selection across waveform / staff. Setting a
    // loop selection clears any barline / marker selection (mutual
    // exclusion).
    void setSelectedLoopId(std::optional<std::int64_t> id);

signals:
    void seekRequested(std::int64_t ms);
    // The widget's barline selection changed in response to a click,
    // key-nav, or a model change. nullopt when the selection was
    // cleared (including by the mutual-exclusion swap).
    void barlineSelectionChanged(std::optional<std::size_t> index);
    void markerSelectionChanged (std::optional<std::int64_t> id);
    // Loop-selection signal — emitted when the widget's loop
    // selection clears as a side effect of mutual exclusion (a
    // barline or marker becomes selected) or when the selected loop
    // is removed from the model. The widget never *originates* a
    // loop selection in this commit — that path is dock-driven.
    void loopSelectionChanged(std::optional<std::int64_t> id);
    // User pressed Del while a barline / marker was selected.
    // MainWindow turns these into model->removeAt / model->remove
    // calls.
    void barlineDeleteRequested(std::size_t index);
    void markerDeleteRequested (std::int64_t id);

protected:
    void paintEvent(QPaintEvent* event)        override;
    void mousePressEvent(QMouseEvent* event)   override;
    void keyPressEvent(QKeyEvent* event)       override;

private:
    void onBarlineModelChanged();
    void onMarkerModelChanged();
    void onLoopModelChanged();

    std::shared_ptr<const audio::WaveformOverview> overview_;
    std::shared_ptr<const score::BarlineModel>     barlineModel_;
    std::shared_ptr<const score::MarkerModel>      markerModel_;
    std::shared_ptr<const score::LoopModel>        loopModel_;
    std::int64_t                                   positionMs_ = 0;
    std::optional<std::size_t>                     selectedBarline_;
    std::optional<std::int64_t>                    selectedMarkerId_;
    std::optional<std::int64_t>                    selectedLoopId_;
};

} // namespace fiddler::ui
