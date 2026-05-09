// Tests for ui::LoopCountdownWidget — the circular tick-ring
// countdown shown at the bottom of the loop property page.
//
// MEMO[refactor]: each TEST_CASE pins one rule of the state
// machine: start() → counting; cancel() → idle (full ring);
// finished signal fires once at the end of a natural depletion;
// repaint survives extreme sizes. Visual correctness (cyan glow,
// clockwise depletion) belongs in the user's smoke test — pixel-
// inspection here would just lock down arbitrary RGB values.

#include "qt_test_app.h"
#include "ui/LoopCountdownWidget.h"

#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using fiddler::test::qtApp;
using fiddler::ui::LoopCountdownWidget;

TEST_CASE("LoopCountdownWidget: defaults to idle, full ring",
          "[loop-countdown]") {
    qtApp();
    LoopCountdownWidget w;
    REQUIRE_FALSE(w.isCountingDown());
    REQUIRE(w.remainingTicks() == LoopCountdownWidget::kTickCount);
    REQUIRE(w.idleVisible());
}

TEST_CASE("LoopCountdownWidget: start(positive) enters counting state",
          "[loop-countdown]") {
    qtApp();
    LoopCountdownWidget w;
    w.start(800);
    REQUIRE(w.isCountingDown());
    REQUIRE(w.remainingTicks() == LoopCountdownWidget::kTickCount);
}

TEST_CASE("LoopCountdownWidget: start(0) is a no-op + cancels any active",
          "[loop-countdown]") {
    // MEMO: pauseMs == 0 is the "tight wrap" branch in MainWindow's
    // updatePosition — no countdown to run there. The widget
    // defends the same contract.
    qtApp();
    LoopCountdownWidget w;
    w.start(800);
    REQUIRE(w.isCountingDown());

    w.start(0);
    REQUIRE_FALSE(w.isCountingDown());
}

TEST_CASE("LoopCountdownWidget: cancel() snaps back to full ring",
          "[loop-countdown]") {
    qtApp();
    LoopCountdownWidget w;
    w.start(160);   // 16 ticks × 10 ms each
    QTest::qWait(35);    // ≥ 3 ticks elapsed
    REQUIRE(w.isCountingDown());
    REQUIRE(w.remainingTicks() < LoopCountdownWidget::kTickCount);

    w.cancel();
    REQUIRE_FALSE(w.isCountingDown());
    REQUIRE(w.remainingTicks() == LoopCountdownWidget::kTickCount);
}

TEST_CASE("LoopCountdownWidget: ticks deplete over the configured window",
          "[loop-countdown]") {
    // MEMO: load-bearing — pin the cadence. Each tick consumes
    // totalMs / kTickCount; after a fraction of the total window
    // the ring should be partially depleted, and after the full
    // window it should be empty. We don't pin exact intermediate
    // counts because timer scheduling jitter on CI can shift them
    // by ±1 tick.
    qtApp();
    LoopCountdownWidget w;
    QSignalSpy tickSpy(&w, &LoopCountdownWidget::tickConsumed);
    QSignalSpy doneSpy(&w, &LoopCountdownWidget::finished);

    w.start(160);    // 10 ms per tick

    // Wait for the natural finish — a generous timeout absorbs
    // CI jitter.
    REQUIRE(QTest::qWaitFor(
        [&]() { return !w.isCountingDown(); }, 2000));

    REQUIRE(w.remainingTicks() == 0);
    REQUIRE(tickSpy.count() == LoopCountdownWidget::kTickCount);
    REQUIRE(doneSpy.count() == 1);
}

TEST_CASE("LoopCountdownWidget: setIdleVisible toggles paint state",
          "[loop-countdown]") {
    // The widget paints unconditionally during a countdown; the
    // idleVisible flag only affects the paint when no countdown
    // is running. Verify the getter reflects what we set.
    qtApp();
    LoopCountdownWidget w;
    REQUIRE(w.idleVisible());
    w.setIdleVisible(false);
    REQUIRE_FALSE(w.idleVisible());
    w.setIdleVisible(true);
    REQUIRE(w.idleVisible());
}

TEST_CASE("LoopCountdownWidget: paint survives extreme sizes",
          "[loop-countdown][gui]") {
    qtApp();
    LoopCountdownWidget w;

    // Tiny — radii could collapse to negative if the math isn't
    // careful. Verify nothing throws.
    w.resize(8, 8);
    w.show();
    QTest::qWait(20);

    // Large — make sure the ring scales smoothly with the widget.
    w.resize(400, 400);
    QTest::qWait(20);

    // Mid-countdown paint.
    w.start(800);
    QTest::qWait(60);
    SUCCEED();
}

TEST_CASE("LoopCountdownWidget: defaults to not-armed",
          "[loop-countdown][progressive-weight]") {
    qtApp();
    LoopCountdownWidget w;
    REQUIRE_FALSE(w.isArmed());
}

TEST_CASE("LoopCountdownWidget: setArmed toggles independently of counting",
          "[loop-countdown][progressive-weight]") {
    // MEMO: armed and counting are orthogonal — a loop can be
    // armed without currently being in a wrap-pause. The widget
    // tracks them as separate flags so the paint can choose the
    // right tier on the disabled / ready / active axis.
    qtApp();
    LoopCountdownWidget w;
    w.setArmed(true);
    REQUIRE(w.isArmed());
    REQUIRE_FALSE(w.isCountingDown());

    w.setArmed(false);
    REQUIRE_FALSE(w.isArmed());
}

TEST_CASE("LoopCountdownWidget: paint survives across all three tiers",
          "[loop-countdown][progressive-weight][gui]") {
    // MEMO: smoke-paints the disabled / ready / active states so
    // a future paint-event refactor doesn't crash on any one of
    // them. Visual correctness (correct hues + glow only when
    // active) is verified in the user's smoke test.
    qtApp();
    LoopCountdownWidget w;
    w.resize(96, 96);
    w.show();

    // Disabled tier (default).
    QTest::qWait(20);

    // Ready tier.
    w.setArmed(true);
    QTest::qWait(20);

    // Active tier.
    w.start(160);
    QTest::qWait(60);
    w.cancel();
    SUCCEED();
}

TEST_CASE("LoopCountdownWidget: restarting an active countdown resets the ring",
          "[loop-countdown]") {
    qtApp();
    LoopCountdownWidget w;
    w.start(160);
    QTest::qWait(35);
    REQUIRE(w.remainingTicks() < LoopCountdownWidget::kTickCount);

    // Fresh start — the ring should snap back to full and start
    // counting again with the new totalMs.
    w.start(800);
    REQUIRE(w.isCountingDown());
    REQUIRE(w.remainingTicks() == LoopCountdownWidget::kTickCount);
    w.cancel();
}
