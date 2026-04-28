// StaffWidget — empty 5-line music staff with the current time
// signature, the user-placed barlines, and a playhead cursor.
//
// The widget does not know about Player, the audio engine, or any
// audio decoding. It receives:
//
//   * the source-time duration of the file (via setDurationMs),
//   * the shared BarlineModel (via setBarlineModel),
//   * the current playback position in source-time (via setPositionMs),
//
// and emits seekRequested(ms) on left-click. MainWindow wires those
// signals back to Player. This is the same shape as WaveformWidget,
// so the two views can sit next to each other and stay in sync via
// MainWindow's plumbing.
//
// Why source-time-proportional bar widths (and not equal-width bars
// like real engraving): for an empty staff with no notes yet, lining
// the staff up 1:1 with the waveform is more intuitive — clicking a
// bar on the staff lands you at the same x in the waveform, and the
// cursor moves at one rate across both. When notes land in step 6+
// we may switch to a notation-aware layout for the staff alone; we
// can revisit then.

#pragma once

#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;

namespace fiddler::score { class BarlineModel; }

namespace fiddler::ui {

class StaffWidget : public QWidget {
    Q_OBJECT
public:
    explicit StaffWidget(QWidget* parent = nullptr);
    ~StaffWidget() override;

    // The source-time duration of the loaded file, in milliseconds.
    // Used as the right-hand bound of the source-time → x-pixel
    // mapping. Set to 0 (the default) when no file is loaded.
    void setDurationMs(std::int64_t ms);
    [[nodiscard]] std::int64_t durationMs() const noexcept { return durationMs_; }

    // Attach (or detach with nullptr) a BarlineModel. The widget
    // listens to the model's `changed()` signal and repaints; if the
    // currently selected entry is removed, the selection is cleared
    // automatically.
    void setBarlineModel(std::shared_ptr<const score::BarlineModel> model);
    [[nodiscard]] std::shared_ptr<const score::BarlineModel>
        barlineModel() const noexcept { return barlineModel_; }

    [[nodiscard]] std::int64_t positionMs() const noexcept { return positionMs_; }
    [[nodiscard]] std::optional<std::size_t>
        selectedBarline() const noexcept { return selectedBarline_; }

    // Coordinate transforms — public so other code can map between
    // pixel columns and source-time milliseconds without
    // reimplementing the math. xToMs() returns 0 when no duration
    // is set; msToX() returns 0 in the same case.
    [[nodiscard]] std::int64_t xToMs(int x) const noexcept;
    [[nodiscard]] int          msToX(std::int64_t ms) const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

public slots:
    void setPositionMs(std::int64_t ms);
    // Programmatic selection setter. MainWindow uses this to keep
    // the waveform and staff showing the same selected barline —
    // when the user clicks a bar in the waveform, MainWindow forwards
    // that index to the staff via this method, and vice versa.
    void setSelectedBarline(std::optional<std::size_t> index);

signals:
    // Fires on left-click. The argument is the source-time the user
    // clicked (or the exact ms of a clicked-on barline, see the .cpp
    // for the click-to-select details). MainWindow wires this to
    // Player::seek().
    void seekRequested(std::int64_t ms);

    // Fires when the widget's selection state changes — by click,
    // arrow-key navigation, Esc, or because the model invalidated
    // the previous selection. MainWindow mirrors the value to the
    // sibling WaveformWidget so both views show the same highlight.
    void barlineSelectionChanged(std::optional<std::size_t> index);

    // Fires when the user presses Del while a barline is selected.
    // MainWindow turns this into a BarlineModel::removeAt() call.
    void barlineDeleteRequested(std::size_t index);

protected:
    void paintEvent(QPaintEvent* event)        override;
    void mousePressEvent(QMouseEvent* event)   override;
    void keyPressEvent(QKeyEvent* event)       override;

private:
    // Slot connected to BarlineModel::changed in setBarlineModel().
    // Invalidates a now-out-of-range selection and triggers a repaint.
    void onBarlineModelChanged();

    // paintEvent's work is split into one helper per visual concern.
    // Each helper assumes `painter` is already targeting this widget
    // and the background has been filled.
    void paintStaffLines(QPainter& painter)    const;
    void paintTimeSignature(QPainter& painter) const;
    void paintBarlines(QPainter& painter)      const;
    void paintCursor(QPainter& painter)        const;

    // Pixel y-coordinates of the top and bottom staff lines.
    [[nodiscard]] int staffTopY()    const noexcept;
    [[nodiscard]] int staffBottomY() const noexcept;

    std::shared_ptr<const score::BarlineModel> barlineModel_;
    std::int64_t                               durationMs_      = 0;
    std::int64_t                               positionMs_      = 0;
    std::optional<std::size_t>                 selectedBarline_;
};

} // namespace fiddler::ui
