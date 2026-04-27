// GUI tests for WaveformWidget. Driven through real Qt event
// machinery (QTest::mouseClick + QSignalSpy), running on the
// "offscreen" platform plugin so the suite stays headless on CI.

#include "audio/WaveformOverview.h"
#include "qt_test_app.h"
#include "ui/WaveformWidget.h"

#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using fiddler::audio::WaveformOverview;
using fiddler::test::qtApp;
using fiddler::ui::WaveformWidget;

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels   = 2;

// Build a 1-second stereo overview filled with a constant amplitude
// — content doesn't matter for click/coord tests, just shape.
std::shared_ptr<const WaveformOverview>
makeOverview(int seconds = 1, std::size_t buckets = 1024) {
    std::vector<float> samples(
        static_cast<std::size_t>(seconds) * kSampleRate * kChannels, 0.5f);
    return std::make_shared<const WaveformOverview>(
        WaveformOverview::fromSamples(samples, kSampleRate, kChannels, buckets));
}

} // namespace

// ---------------------------------------------------------------------------
// Empty / degenerate state
// ---------------------------------------------------------------------------

TEST_CASE("WaveformWidget: empty state renders without crashing",
          "[waveform-widget][gui]") {
    qtApp();
    WaveformWidget w;
    w.resize(400, 100);
    w.show();
    QTest::qWait(20);   // let the paint event flush
    SUCCEED();          // no crash is the assertion
}

TEST_CASE("WaveformWidget: coord transforms return 0 when no overview is set",
          "[waveform-widget][gui]") {
    qtApp();
    WaveformWidget w;
    w.resize(800, 100);
    REQUIRE(w.xToMs(0)   == 0);
    REQUIRE(w.xToMs(400) == 0);
    REQUIRE(w.msToX(0)    == 0);
    REQUIRE(w.msToX(5000) == 0);
}

TEST_CASE("WaveformWidget: click without overview emits nothing",
          "[waveform-widget][gui]") {
    qtApp();
    WaveformWidget w;
    w.resize(400, 100);
    QSignalSpy spy(&w, &WaveformWidget::seekRequested);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(200, 50));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// Coordinate transforms
// ---------------------------------------------------------------------------

TEST_CASE("WaveformWidget: xToMs maps endpoints and middle correctly",
          "[waveform-widget][gui][coords]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/2));
    w.resize(1000, 100);

    // Two-second file across 1000 px → 1 px ≈ 2 ms.
    REQUIRE(w.xToMs(0)    == 0);
    REQUIRE(w.xToMs(1000) == 2000);   // clamped to duration
    const auto mid = w.xToMs(500);
    REQUIRE(mid >= 990);
    REQUIRE(mid <= 1010);
}

TEST_CASE("WaveformWidget: msToX maps endpoints and middle correctly",
          "[waveform-widget][gui][coords]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/2));
    w.resize(1000, 100);

    REQUIRE(w.msToX(0)    == 0);
    REQUIRE(w.msToX(2000) == 999);    // clamped to width-1
    const int xMid = w.msToX(1000);
    REQUIRE(xMid >= 490);
    REQUIRE(xMid <= 510);
}

TEST_CASE("WaveformWidget: xToMs / msToX round-trip across the width",
          "[waveform-widget][gui][coords]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/10));
    w.resize(800, 80);

    for (int x = 0; x < 800; x += 37) {
        const auto ms        = w.xToMs(x);
        const int  roundTrip = w.msToX(ms);
        // Allow ±1 px slack for the integer truncation in either
        // direction.
        REQUIRE(roundTrip >= x - 1);
        REQUIRE(roundTrip <= x + 1);
    }
}

TEST_CASE("WaveformWidget: out-of-range x clamps to file bounds",
          "[waveform-widget][gui][coords]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/3));
    w.resize(600, 80);

    REQUIRE(w.xToMs(-100)  == 0);
    REQUIRE(w.xToMs(99999) == 3000);
    REQUIRE(w.msToX(-500)  == 0);
    REQUIRE(w.msToX(99999) == 599);
}

// ---------------------------------------------------------------------------
// Click → seekRequested
// ---------------------------------------------------------------------------

TEST_CASE("WaveformWidget: left click emits seekRequested with correct ms",
          "[waveform-widget][gui][click]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(1000, 100);

    QSignalSpy spy(&w, &WaveformWidget::seekRequested);

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(0, 50));
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(500, 50));
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(999, 50));

    REQUIRE(spy.count() == 3);

    const auto msAtStart = spy.at(0).at(0).toLongLong();
    const auto msAtMid   = spy.at(1).at(0).toLongLong();
    const auto msAtEnd   = spy.at(2).at(0).toLongLong();

    REQUIRE(msAtStart == 0);
    REQUIRE(msAtMid >= 1980);
    REQUIRE(msAtMid <= 2020);
    REQUIRE(msAtEnd  >= 3990);
    REQUIRE(msAtEnd  <= 4000);
}

TEST_CASE("WaveformWidget: right click does not emit seekRequested",
          "[waveform-widget][gui][click]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview());
    w.resize(800, 100);

    QSignalSpy spy(&w, &WaveformWidget::seekRequested);
    QTest::mouseClick(&w, Qt::RightButton,  Qt::NoModifier, QPoint(400, 50));
    QTest::mouseClick(&w, Qt::MiddleButton, Qt::NoModifier, QPoint(400, 50));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// setPositionMs / setOverview state
// ---------------------------------------------------------------------------

TEST_CASE("WaveformWidget: setPositionMs updates the public observable",
          "[waveform-widget][gui]") {
    qtApp();
    WaveformWidget w;
    REQUIRE(w.positionMs() == 0);
    w.setPositionMs(1234);
    REQUIRE(w.positionMs() == 1234);
    w.setPositionMs(0);
    REQUIRE(w.positionMs() == 0);
}

TEST_CASE("WaveformWidget: setOverview replaces and clears state",
          "[waveform-widget][gui]") {
    qtApp();
    WaveformWidget w;
    REQUIRE(w.overview() == nullptr);

    auto first = makeOverview(/*seconds=*/1);
    w.setOverview(first);
    REQUIRE(w.overview() == first);

    auto second = makeOverview(/*seconds=*/5);
    w.setOverview(second);
    REQUIRE(w.overview() == second);

    w.setOverview(nullptr);
    REQUIRE(w.overview() == nullptr);
}

TEST_CASE("WaveformWidget: paint survives extreme widget sizes",
          "[waveform-widget][gui]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview());

    for (const auto& sz : {QSize(1, 1), QSize(1, 200), QSize(2000, 40),
                           QSize(50, 4)}) {
        w.resize(sz);
        w.show();
        QTest::qWait(5);
    }
    SUCCEED();
}
