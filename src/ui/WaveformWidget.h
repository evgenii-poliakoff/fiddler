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

namespace fiddler::score { class BarlineModel; }

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

    [[nodiscard]] std::int64_t positionMs() const noexcept { return positionMs_; }
    [[nodiscard]] std::optional<std::size_t>
        selectedBarline() const noexcept { return selectedBarline_; }

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
    void setSelectedBarline(std::optional<std::size_t> index);

signals:
    void seekRequested(std::int64_t ms);
    // The widget's selection changed in response to a click, key-nav,
    // or a model change that invalidated the previous selection.
    void barlineSelectionChanged(std::optional<std::size_t> index);
    // User pressed Del while a barline was selected. MainWindow
    // turns this into a model->removeAt() call.
    void barlineDeleteRequested(std::size_t index);

protected:
    void paintEvent(QPaintEvent* event)        override;
    void mousePressEvent(QMouseEvent* event)   override;
    void keyPressEvent(QKeyEvent* event)       override;

private:
    void onBarlineModelChanged();

    std::shared_ptr<const audio::WaveformOverview> overview_;
    std::shared_ptr<const score::BarlineModel>     barlineModel_;
    std::int64_t                                   positionMs_ = 0;
    std::optional<std::size_t>                     selectedBarline_;
};

} // namespace fiddler::ui
