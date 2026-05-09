// LoopCountdownWidget — circular tick-ring countdown, drawn at the
// bottom of the loop property page. While a loop is armed and the
// transport is paused between repeats, the 16 radial ticks deplete
// one by one over the pauseMs window. When idle (loop selected but
// not counting down), the ring is shown full so the widget is
// self-explanatory before the user has experienced a wrap.
//
// MEMO: 16 ticks chosen as a balance — denser than a clock face
// (so it reads as "digital" rather than analog), but not so dense
// that individual depletions are imperceptible at typical
// pauseMs (500ms → ~31ms per tick is still discriminable as a
// stepping ring rather than a smooth sweep).
//
// MEMO: depletion direction is clockwise from 12 o'clock. The eye
// naturally returns to 12 when the ring empties; that's where the
// "next take begins" cue lands.
//
// API shape: start(int totalMs) launches a countdown; cancel()
// halts it; setIdleVisible(bool) controls whether the full ring
// shows when not counting down (used to hide the widget when no
// loop is selected). The widget owns its own QTimer; it doesn't
// piggy-back on MainWindow's position poll because the depletion
// cadence depends on pauseMs, not on the GUI tick rate.

#pragma once

#include <QWidget>

#include <chrono>

class QPaintEvent;
class QTimer;

namespace fiddler::ui {

class LoopCountdownWidget : public QWidget {
    Q_OBJECT
public:
    explicit LoopCountdownWidget(QWidget* parent = nullptr);
    ~LoopCountdownWidget() override;

    // Total tick count around the ring. Public so tests can pin
    // the value; production code never overrides it.
    static constexpr int kTickCount = 16;

    // Begin a countdown that should fully deplete the ring after
    // `totalMs` milliseconds. If `totalMs <= 0` the call is a
    // no-op (no countdown to run). Restarts any previous active
    // countdown.
    void start(int totalMs);

    // Stop any active countdown. The ring snaps back to full when
    // idle is visible, otherwise the widget paints empty.
    void cancel();

    // Whether the widget paints anything when no countdown is
    // running. True (default) draws a full ring as a visual hint
    // for "this is where the countdown will appear". False paints
    // a blank widget — useful for hiding the affordance when no
    // loop is selected.
    void setIdleVisible(bool visible);
    [[nodiscard]] bool idleVisible() const noexcept { return idleVisible_; }

    // Armed state controls the idle visual weight. Three tiers:
    //   * not armed (default)  → dim, desaturated cyan, no glow
    //   * armed, not counting  → mid-brightness cyan, no glow
    //   * counting (start())   → full bright cyan + glow halo
    // Same hue across all three (cyan family) so the eye reads
    // them as one affordance in different states. See
    // memory/feedback_progressive_visual_weight.md for the
    // canonical rule.
    void setArmed(bool armed);
    [[nodiscard]] bool isArmed() const noexcept { return armed_; }

    // True between start() and either the natural finish or
    // cancel(). Tests use this to verify state machine.
    [[nodiscard]] bool isCountingDown() const noexcept { return countingDown_; }

    // Number of ticks still drawn at this instant. Counts down
    // from kTickCount to 0 over the pause window.
    [[nodiscard]] int remainingTicks() const noexcept { return remainingTicks_; }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    // Emitted when remainingTicks drops below kTickCount because
    // a tick has been consumed. Useful for tests; production
    // wiring listens via the visible repaint.
    void tickConsumed(int remaining);

    // Emitted when the countdown reaches 0 naturally (NOT on
    // cancel()). MainWindow could consume this in the future as
    // a sample-accurate "resume now" cue; today the existing
    // QTimer::singleShot in updatePosition is the source of
    // truth for the resume moment.
    void finished();

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTickTimer();

private:
    QTimer* tickTimer_ = nullptr;
    int     remainingTicks_ = kTickCount;
    bool    countingDown_   = false;
    bool    idleVisible_    = true;
    bool    armed_          = false;
};

} // namespace fiddler::ui
