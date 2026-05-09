// GUI tests for WaveformWidget. Driven through real Qt event
// machinery (QTest::mouseClick + QSignalSpy), running on the
// "offscreen" platform plugin so the suite stays headless on CI.

#include "audio/WaveformOverview.h"
#include "qt_test_app.h"
#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "ui/WaveformWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using fiddler::audio::WaveformOverview;
using fiddler::score::BarlineModel;
using fiddler::score::LoopModel;
using fiddler::score::MarkerModel;
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

// ---------------------------------------------------------------------------
// Barline overlay: paint, selection, key-nav, delete
// ---------------------------------------------------------------------------

namespace {

// Helper: install a barline model on the widget with the given
// timestamps. The model is heap-allocated as shared_ptr so the
// widget can own a const reference; the test holds a non-const handle
// to mutate it later.
std::shared_ptr<BarlineModel>
installModel(WaveformWidget& w, std::span<const std::int64_t> stamps = {}) {
    auto model = std::make_shared<BarlineModel>();
    for (auto ms : stamps) model->add(ms);
    w.setBarlineModel(model);
    return model;
}

} // namespace

TEST_CASE("WaveformWidget: paints barline ticks without crashing",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500, 2500, 3500 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    QTest::qWait(20);
    SUCCEED();
}

TEST_CASE("WaveformWidget: click on a barline selects it and seeks to its ms",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));      // 4 s, msToX is linear
    // Place a barline at exactly 1000 ms — at width 800 that's x=200.
    const std::int64_t stamps[] = { 1000 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);

    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);
    QSignalSpy selSpy(&w,  &WaveformWidget::barlineSelectionChanged);

    // Click at x=202 — within 5 px of the tick at x=200.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(202, 50));

    REQUIRE(w.selectedBarline() == 0);

    // The seek emission carries the *barline's* ms (1000), not the
    // click's ms (which would be ~1010).
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 1000);

    REQUIRE(selSpy.count() == 1);
    REQUIRE(selSpy.takeFirst().at(0).value<std::optional<std::size_t>>()
            == std::optional<std::size_t>{0});
}

TEST_CASE("WaveformWidget: click far from any barline seeks without selecting",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1000 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);

    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);
    QSignalSpy selSpy(&w,  &WaveformWidget::barlineSelectionChanged);

    // Click far from x=200 — say x=600 (3000 ms), no barline anywhere
    // near. Selection stays empty, seek goes to clicked ms.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(600, 50));

    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(seekSpy.count() == 1);
    const auto seekedMs = seekSpy.takeFirst().at(0).toLongLong();
    REQUIRE(seekedMs >= 2980);
    REQUIRE(seekedMs <= 3020);
    REQUIRE(selSpy.count() == 0);   // no selection change to report
}

TEST_CASE("WaveformWidget: clicking elsewhere clears an existing selection",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1000 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);

    // Pre-select via click on the bar.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(200, 50));
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy selSpy(&w, &WaveformWidget::barlineSelectionChanged);
    // Click in empty space.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(600, 50));
    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(selSpy.count() == 1);
    REQUIRE(selSpy.takeFirst().at(0).value<std::optional<std::size_t>>()
            == std::optional<std::size_t>{});
}

TEST_CASE("WaveformWidget: arrow keys navigate selection between barlines",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500, 2500, 3500 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);

    // Pre-select index 1 by simulated click at the 1500 ms tick (x=300).
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(300, 50));
    REQUIRE(w.selectedBarline() == 1);

    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);
    seekSpy.clear();

    QTest::keyClick(&w, Qt::Key_Right);
    REQUIRE(w.selectedBarline() == 2);
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 2500);

    QTest::keyClick(&w, Qt::Key_Left);
    QTest::keyClick(&w, Qt::Key_Left);
    REQUIRE(w.selectedBarline() == 0);
}

TEST_CASE("WaveformWidget: arrow keys saturate at boundaries",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);

    // Pre-select last (index 2).
    w.setSelectedBarline(2);
    QTest::keyClick(&w, Qt::Key_Right);   // already at end
    REQUIRE(w.selectedBarline() == 2);

    // Pre-select first (index 0).
    w.setSelectedBarline(0);
    QTest::keyClick(&w, Qt::Key_Left);    // already at start
    REQUIRE(w.selectedBarline() == 0);
}

TEST_CASE("WaveformWidget: Esc clears the selection",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1000 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);
    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy selSpy(&w, &WaveformWidget::barlineSelectionChanged);
    QTest::keyClick(&w, Qt::Key_Escape);
    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(selSpy.count() == 1);
}

TEST_CASE("WaveformWidget: Del fires barlineDeleteRequested with the index",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);
    w.setSelectedBarline(1);

    QSignalSpy delSpy(&w, &WaveformWidget::barlineDeleteRequested);
    QTest::keyClick(&w, Qt::Key_Delete);
    REQUIRE(delSpy.count() == 1);
    REQUIRE(delSpy.takeFirst().at(0).value<std::size_t>() == 1u);
}

TEST_CASE("WaveformWidget: Del with no selection does nothing",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    installModel(w, std::span<const std::int64_t>{});
    w.resize(800, 100);
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);

    QSignalSpy delSpy(&w, &WaveformWidget::barlineDeleteRequested);
    QTest::keyClick(&w, Qt::Key_Delete);
    REQUIRE(delSpy.count() == 0);
}

TEST_CASE("WaveformWidget: model removeAt of selected entry clears selection",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    auto model = installModel(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.setSelectedBarline(2);
    REQUIRE(w.selectedBarline() == 2);

    QSignalSpy selSpy(&w, &WaveformWidget::barlineSelectionChanged);

    // Remove the selected entry. The widget should observe the
    // model's `changed` signal and drop the now-out-of-range index.
    model->removeAt(2);
    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(selSpy.count() >= 1);
}

TEST_CASE("WaveformWidget: setBarlineModel(nullptr) detaches and clears selection",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1000 };
    installModel(w, std::span<const std::int64_t>{stamps});
    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline() == 0);

    w.setBarlineModel(nullptr);
    REQUIRE(w.barlineModel() == nullptr);
    REQUIRE_FALSE(w.selectedBarline().has_value());
}

TEST_CASE("WaveformWidget: setSelectedBarline rejects out-of-range indices",
          "[waveform-widget][gui][barlines]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500 };
    installModel(w, std::span<const std::int64_t>{stamps});

    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline() == 0);

    // Out of range — silently coerced to nullopt.
    w.setSelectedBarline(99);
    REQUIRE_FALSE(w.selectedBarline().has_value());
}

// ---------------------------------------------------------------------------
// Marker overlay
// ---------------------------------------------------------------------------

namespace {

// Install a fresh MarkerModel on the widget with the given ms
// values. Returns the model handle so the test can mutate it later.
// The model is heap-allocated; the widget takes a const view.
std::shared_ptr<MarkerModel>
installMarkers(WaveformWidget& w, std::span<const std::int64_t> stamps) {
    auto model = std::make_shared<MarkerModel>();
    for (auto ms : stamps) (void)model->add(ms);
    w.setMarkerModel(model);
    return model;
}

} // namespace

TEST_CASE("WaveformWidget: paints marker ticks + label flags without crashing",
          "[waveform-widget][gui][markers]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    installMarkers(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    QTest::qWait(20);
    SUCCEED();
}

TEST_CASE("WaveformWidget: click on a marker selects by ID and seeks",
          "[waveform-widget][gui][markers]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));   // 4 s file
    // Place a marker at exactly 1000 ms — at width=800 that's x=200.
    const std::int64_t stamps[] = { 1000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);

    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);
    QSignalSpy selSpy (&w, &WaveformWidget::markerSelectionChanged);

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(202, 50));

    // MEMO: load-bearing — selection is by stable ID, not index.
    // The widget's API surface for marker selection is selectedMarkerId().
    REQUIRE(w.selectedMarkerId().has_value());
    REQUIRE(*w.selectedMarkerId() == *model->idAt(0));

    // Seek goes to the marker's exact ms (1000), not the click ms (~1010).
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 1000);

    REQUIRE(selSpy.count() == 1);
}

TEST_CASE("WaveformWidget: marker click clears any active barline selection",
          "[waveform-widget][gui][markers]") {
    // MEMO: load-bearing — barline-selection and marker-selection
    // are mutually exclusive. Selecting a marker must clear the
    // barline selection, otherwise the property viewer can't show
    // a coherent "selected artifact".
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t bars[]    = { 500 };           // barline at x=100
    const std::int64_t markers[] = { 2000 };          // marker at x=400
    auto barModel = std::make_shared<BarlineModel>();
    for (auto ms : bars) (void)barModel->add(ms);
    w.setBarlineModel(barModel);
    installMarkers(w, std::span<const std::int64_t>{markers});
    w.resize(800, 100);

    // Pre-select a barline by clicking on it.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(102, 50));
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy barSelSpy(&w, &WaveformWidget::barlineSelectionChanged);

    // Click on the marker. Marker becomes selected; barline cleared.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(402, 50));

    REQUIRE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedBarline().has_value());
    // The barline-selection-changed signal fired once with nullopt.
    REQUIRE(barSelSpy.count() == 1);
    REQUIRE(barSelSpy.takeFirst().at(0)
            .value<std::optional<std::size_t>>() == std::nullopt);
}

TEST_CASE("WaveformWidget: barline click clears any active marker selection",
          "[waveform-widget][gui][markers]") {
    // Mirror of the previous test — the rule is symmetric.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t bars[]    = { 2000 };
    const std::int64_t markers[] = { 500 };
    auto barModel = std::make_shared<BarlineModel>();
    for (auto ms : bars) (void)barModel->add(ms);
    w.setBarlineModel(barModel);
    installMarkers(w, std::span<const std::int64_t>{markers});
    w.resize(800, 100);

    // Pre-select the marker.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(102, 50));
    REQUIRE(w.selectedMarkerId().has_value());

    QSignalSpy markerSelSpy(&w, &WaveformWidget::markerSelectionChanged);

    // Click the barline.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(402, 50));

    REQUIRE(w.selectedBarline() == 0);
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
    REQUIRE(markerSelSpy.count() == 1);
}

TEST_CASE("WaveformWidget: Del with marker selection fires markerDeleteRequested",
          "[waveform-widget][gui][markers][keys]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);
    w.setSelectedMarkerId(*model->idAt(0));

    QSignalSpy delSpy(&w, &WaveformWidget::markerDeleteRequested);
    QTest::keyClick(&w, Qt::Key_Delete);

    REQUIRE(delSpy.count() == 1);
    // MEMO: delete-by-ID, not by index — markers can move under
    // selection and the widget mustn't translate to a stale index.
    REQUIRE(delSpy.takeFirst().at(0).value<std::int64_t>()
            == *model->idAt(0));
}

TEST_CASE("WaveformWidget: arrow nav cycles through markers when one is selected",
          "[waveform-widget][gui][markers][keys]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 500, 1500, 2500, 3500 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.resize(800, 100);
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);

    // Pre-select the second marker (index 1 in sort order).
    w.setSelectedMarkerId(*model->idAt(1));
    REQUIRE(*w.selectedMarkerId() == *model->idAt(1));

    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);
    QTest::keyClick(&w, Qt::Key_Right);
    REQUIRE(*w.selectedMarkerId() == *model->idAt(2));
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 2500);

    QTest::keyClick(&w, Qt::Key_Left);
    QTest::keyClick(&w, Qt::Key_Left);
    REQUIRE(*w.selectedMarkerId() == *model->idAt(0));
}

TEST_CASE("WaveformWidget: marker setPosition keeps selection alive across re-sort",
          "[waveform-widget][gui][markers]") {
    // MEMO: load-bearing — this is exactly what stable IDs buy us.
    // After moving the selected marker past its neighbour, its
    // index changes from 0 to 1 but its ID is the same; the widget
    // selection (held by ID) tracks correctly.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1000, 2000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    const auto firstId = *model->idAt(0);

    w.setSelectedMarkerId(firstId);
    REQUIRE(*w.selectedMarkerId() == firstId);

    // Move the first marker past the second.
    REQUIRE(model->setPosition(firstId, 3000));

    REQUIRE(w.selectedMarkerId() == firstId);   // still tracking
    REQUIRE(*model->indexOf(firstId) == 1);      // but at a new index
}

TEST_CASE("WaveformWidget: removing the selected marker clears the selection",
          "[waveform-widget][gui][markers]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1000, 2000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    const auto secondId = *model->idAt(1);
    w.setSelectedMarkerId(secondId);
    REQUIRE(w.selectedMarkerId() == secondId);

    QSignalSpy selSpy(&w, &WaveformWidget::markerSelectionChanged);
    REQUIRE(model->remove(secondId));
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
    REQUIRE(selSpy.count() >= 1);
}

TEST_CASE("WaveformWidget: setMarkerModel(nullptr) detaches and clears selection",
          "[waveform-widget][gui][markers]") {
    qtApp();
    WaveformWidget w;
    const std::int64_t stamps[] = { 1000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.setSelectedMarkerId(*model->idAt(0));
    REQUIRE(w.selectedMarkerId().has_value());

    w.setMarkerModel(nullptr);
    REQUIRE(w.markerModel() == nullptr);
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
}

// ---------------------------------------------------------------------------
// Loop overlay
//
// MEMO[refactor]: loops in this commit are render-only — selection
// is dock-driven, so these tests pin two properties: (1) the widget
// can paint loop bands without crashing, and (2) the mutual-exclusion
// rule extends across all three artifact kinds (barline, marker,
// loop). Click-to-select-loop, double-click-to-arm, and
// arrow-nav-on-loops are deliberately NOT tested here because the
// widget does not implement them yet (commits 3+ for dock-driven
// arming, possibly later for click-on-band).
// ---------------------------------------------------------------------------

namespace {

// Install a fresh LoopModel on the widget with the given (start, end)
// pairs. Returns the handle so tests can mutate it.
std::shared_ptr<LoopModel>
installLoops(WaveformWidget& w,
             std::span<const std::pair<std::int64_t, std::int64_t>> ranges) {
    auto model = std::make_shared<LoopModel>();
    for (const auto& r : ranges) (void)model->add(r.first, r.second);
    w.setLoopModel(model);
    return model;
}

} // namespace

TEST_CASE("WaveformWidget: paints loop bands without crashing",
          "[waveform-widget][gui][loops]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1500}, {2000, 3000}
    };
    installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.resize(800, 120);
    w.show();
    // The return value is discarded on purpose — under the offscreen
    // platform plugin the function may legitimately time out on
    // headless CI machines, but the paint event still fires before
    // we get here.
    (void)QTest::qWaitForWindowExposed(&w);
    SUCCEED();
}

TEST_CASE("WaveformWidget: setSelectedLoopId clears barline + marker selections",
          "[waveform-widget][gui][loops]") {
    // MEMO: load-bearing — mutual exclusion across all three artifact
    // kinds. The "selected artifact" is a single concept the dock
    // surfaces; only one slot is populated at a time.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));

    auto barlines = std::make_shared<BarlineModel>();
    barlines->add(1000);
    w.setBarlineModel(barlines);

    const std::int64_t stamps[] = { 2000 };
    auto markers = installMarkers(w, std::span<const std::int64_t>{stamps});

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto loops = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    // Seed both barline + marker selections, then setSelectedLoopId
    // must clear both.
    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline().has_value());
    w.setSelectedMarkerId(*markers->idAt(0));
    REQUIRE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedBarline().has_value());   // marker cleared barline

    QSignalSpy markerSpy(&w, &WaveformWidget::markerSelectionChanged);
    QSignalSpy loopSpy  (&w, &WaveformWidget::loopSelectionChanged);

    w.setSelectedLoopId(*loops->idAt(0));

    REQUIRE(w.selectedLoopId().has_value());
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(markerSpy.count() == 1);    // marker cleared as side effect
    REQUIRE(loopSpy.count()   == 1);    // loop set
}

TEST_CASE("WaveformWidget: setSelectedBarline clears an active loop selection",
          "[waveform-widget][gui][loops]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    auto barlines = std::make_shared<BarlineModel>();
    barlines->add(1000);
    w.setBarlineModel(barlines);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto loops = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.setSelectedLoopId(*loops->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    QSignalSpy loopSpy(&w, &WaveformWidget::loopSelectionChanged);
    w.setSelectedBarline(0);

    REQUIRE(w.selectedBarline().has_value());
    REQUIRE_FALSE(w.selectedLoopId().has_value());
    REQUIRE(loopSpy.count() == 1);
}

TEST_CASE("WaveformWidget: setSelectedMarkerId clears an active loop selection",
          "[waveform-widget][gui][loops]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));

    const std::int64_t stamps[] = { 2000 };
    auto markers = installMarkers(w, std::span<const std::int64_t>{stamps});

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto loops = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.setSelectedLoopId(*loops->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    QSignalSpy loopSpy(&w, &WaveformWidget::loopSelectionChanged);
    w.setSelectedMarkerId(*markers->idAt(0));

    REQUIRE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedLoopId().has_value());
    REQUIRE(loopSpy.count() == 1);
}

TEST_CASE("WaveformWidget: loop setRange keeps selection alive across re-sort",
          "[waveform-widget][gui][loops]") {
    // MEMO: this is the analog of the marker setPosition test —
    // exactly why loops carry stable IDs. After a setRange that
    // crosses a neighbour, an index-based selection would now point
    // at a different loop; ID-based selection survives.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));

    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1000}, {2000, 2500}
    };
    auto model = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    const auto firstId = *model->idAt(0);
    w.setSelectedLoopId(firstId);

    // Move first loop past the second.
    REQUIRE(model->setRange(firstId, 3000, 3500));

    REQUIRE(w.selectedLoopId() == firstId);
    REQUIRE(*model->indexOf(firstId) == 1);
}

TEST_CASE("WaveformWidget: removing the selected loop clears the selection",
          "[waveform-widget][gui][loops]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1000}, {2000, 2500}
    };
    auto model = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    const auto secondId = *model->idAt(1);
    w.setSelectedLoopId(secondId);
    REQUIRE(w.selectedLoopId().has_value());

    QSignalSpy selSpy(&w, &WaveformWidget::loopSelectionChanged);
    REQUIRE(model->remove(secondId));
    REQUIRE_FALSE(w.selectedLoopId().has_value());
    REQUIRE(selSpy.count() >= 1);
}

TEST_CASE("WaveformWidget: setSelectedLoopId rejects dangling IDs",
          "[waveform-widget][gui][loops]") {
    qtApp();
    WaveformWidget w;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.setSelectedLoopId(99999);    // ID never issued
    REQUIRE_FALSE(w.selectedLoopId().has_value());
}

TEST_CASE("WaveformWidget: setLoopModel(nullptr) detaches and clears selection",
          "[waveform-widget][gui][loops]") {
    qtApp();
    WaveformWidget w;
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto model = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    w.setSelectedLoopId(*model->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    w.setLoopModel(nullptr);
    REQUIRE(w.loopModel() == nullptr);
    REQUIRE_FALSE(w.selectedLoopId().has_value());
}

// ---------------------------------------------------------------------------
// Secondary anchor (Ctrl+click multi-select for loop creation)
//
// MEMO[refactor]: each TEST_CASE pins one rule of the Ctrl+click
// gesture. The widget's job is narrow: capture the previous primary
// selection's ms when Ctrl+click hits a new artifact, and clear it
// on a plain click or Esc. MainWindow consumes this in the L
// shortcut (covered by integration tests against MainWindow).
// ---------------------------------------------------------------------------

TEST_CASE("WaveformWidget: primaryAnchorMs returns nullopt with no selection",
          "[waveform-widget][gui][secondary-anchor]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    REQUIRE_FALSE(w.primaryAnchorMs().has_value());
}

TEST_CASE("WaveformWidget: primaryAnchorMs resolves selectedBarline to its ms",
          "[waveform-widget][gui][secondary-anchor]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    auto barlines = std::make_shared<BarlineModel>();
    barlines->add(1000);
    barlines->add(2500);
    w.setBarlineModel(barlines);

    w.setSelectedBarline(1);
    REQUIRE(w.primaryAnchorMs() == 2500);
}

TEST_CASE("WaveformWidget: primaryAnchorMs resolves selectedMarkerId to its ms",
          "[waveform-widget][gui][secondary-anchor]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1500 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});

    w.setSelectedMarkerId(*model->idAt(0));
    REQUIRE(w.primaryAnchorMs() == 1500);
}

TEST_CASE("WaveformWidget: Ctrl+click on a marker promotes prior primary to secondary",
          "[waveform-widget][gui][secondary-anchor]") {
    // MEMO: this is the load-bearing rule of the Ctrl+click gesture.
    // The widget captures the *previous* primary's ms into the
    // secondary slot, then installs the clicked marker as the new
    // primary. Without this, MainWindow's L shortcut couldn't see
    // two anchors at once.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 120);

    // Two markers at 1000 ms and 3000 ms — given a 4-second file
    // and an 800-pixel widget, those land at x=200 and x=600.
    const std::int64_t stamps[] = { 1000, 3000 };
    installMarkers(w, std::span<const std::int64_t>{stamps});

    QSignalSpy secondarySpy(&w, &WaveformWidget::secondaryAnchorChanged);

    // Click marker 1 (no Ctrl) → primary at 1000 ms, no secondary.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(200, 60));
    REQUIRE(w.primaryAnchorMs() == 1000);
    REQUIRE_FALSE(w.secondaryAnchorMs().has_value());

    // Ctrl+click marker 2 → primary at 3000, secondary at 1000.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::ControlModifier,
                      QPoint(600, 60));
    REQUIRE(w.primaryAnchorMs() == 3000);
    REQUIRE(w.secondaryAnchorMs() == 1000);
    REQUIRE(secondarySpy.count() >= 1);   // at least one signal emitted
}

TEST_CASE("WaveformWidget: plain click clears any active secondary anchor",
          "[waveform-widget][gui][secondary-anchor]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 120);

    // Pre-seed a secondary anchor; then click an empty patch.
    w.setSecondaryAnchorMs(1500);
    REQUIRE(w.secondaryAnchorMs().has_value());

    QSignalSpy spy(&w, &WaveformWidget::secondaryAnchorChanged);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(700, 60));

    REQUIRE_FALSE(w.secondaryAnchorMs().has_value());
    REQUIRE(spy.count() == 1);
}

TEST_CASE("WaveformWidget: Ctrl+click on empty space leaves secondary intact",
          "[waveform-widget][gui][secondary-anchor]") {
    // MEMO: a Ctrl+click that doesn't land on an artifact should not
    // disturb the user's already-captured anchors. Otherwise a
    // missed click would silently abort the loop-creation gesture.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 120);

    w.setSecondaryAnchorMs(1500);
    REQUIRE(w.secondaryAnchorMs() == 1500);

    QTest::mouseClick(&w, Qt::LeftButton, Qt::ControlModifier,
                      QPoint(700, 60));   // nothing here
    REQUIRE(w.secondaryAnchorMs() == 1500);
}

TEST_CASE("WaveformWidget: Esc clears secondary anchor along with primary",
          "[waveform-widget][gui][secondary-anchor]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    const std::int64_t stamps[] = { 1500 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.setSelectedMarkerId(*model->idAt(0));
    w.setSecondaryAnchorMs(2500);
    REQUIRE(w.secondaryAnchorMs().has_value());

    QTest::keyClick(&w, Qt::Key_Escape);
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.secondaryAnchorMs().has_value());
}

TEST_CASE("WaveformWidget: paint survives secondary anchor on a barline ms",
          "[waveform-widget][gui][secondary-anchor]") {
    // MEMO: regression test — when the secondary anchor's ms equals
    // an existing barline's ms (the common Ctrl+click case), the
    // widget must paint the barline in dashed style instead of
    // drawing a separate dashed tick over the solid one. The bug
    // before the fix: the standalone tick drew over the solid
    // barline, the gaps were filled by the underlying line, and
    // the dashing was invisible to the user.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 120);

    auto barlines = std::make_shared<BarlineModel>();
    barlines->add(1000);
    w.setBarlineModel(barlines);
    w.setSelectedBarline(0);

    // Set the secondary anchor at the same ms as the barline.
    // After the fix, the barline itself renders dashed; before the
    // fix, a separate dashed tick was drawn (which the user
    // couldn't see).
    w.setSecondaryAnchorMs(1000);
    REQUIRE(w.secondaryAnchorMs() == 1000);

    w.show();
    (void)QTest::qWaitForWindowExposed(&w);
    SUCCEED();
}

TEST_CASE("WaveformWidget: paint survives secondary anchor on a marker ms",
          "[waveform-widget][gui][secondary-anchor]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 120);

    const std::int64_t stamps[] = { 1500 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.setSelectedMarkerId(*model->idAt(0));
    w.setSecondaryAnchorMs(1500);   // same ms as the marker

    w.show();
    (void)QTest::qWaitForWindowExposed(&w);
    SUCCEED();
}

TEST_CASE("WaveformWidget: dashed barline at secondary anchor renders distinctly",
          "[waveform-widget][gui][secondary-anchor]") {
    // MEMO: pixel-level regression — render the widget to a QImage
    // and inspect the column at the barline's x. The fix replaced
    // the standalone tick with an artifact-painted-dashed rendering;
    // verify the column is dashed (mix of yellow + non-yellow rows)
    // rather than solid yellow.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 200);

    auto barlines = std::make_shared<BarlineModel>();
    barlines->add(1000);   // x=200 in an 800-px / 4-second view
    w.setBarlineModel(barlines);

    // No primary selection on the bar (mirrors the user's bug
    // scenario: after Ctrl+click in dock, mutual exclusion clears
    // selectedBarline_ on the waveform).
    w.setSecondaryAnchorMs(1000);
    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(w.secondaryAnchorMs() == 1000);

    // Render to image without showing the widget.
    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    // Walk down the column at x=200 and bin pixels into "bright
    // yellow" (dashed pen color 255,220,130 ± slack) vs "dark/other"
    // (gap or no line). A dashed line should produce both kinds in
    // roughly equal measure; a solid line produces only bright
    // yellow rows.
    const int x = 200;
    int yellowRows = 0;
    int otherRows  = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QColor c = image.pixelColor(x, y);
        const bool isYellow = c.red() > 200 && c.green() > 180
                           && c.blue() < 180;
        if (isYellow) ++yellowRows;
        else          ++otherRows;
    }

    // Both kinds must appear — that's what makes the line "dashed".
    // The exact ratio depends on Qt's dash pattern at width 2, but
    // we should see plenty of both. Pick a generous threshold so
    // tiny rendering changes don't flap the test.
    REQUIRE(yellowRows > 20);
    REQUIRE(otherRows  > 20);
}

TEST_CASE("WaveformWidget: dashed indicator pierces through cursor at same x",
          "[waveform-widget][gui][secondary-anchor]") {
    // MEMO: regression for the user-reported bug — when the cursor
    // and the secondary anchor share an x (the common case right
    // after tap-place because the cursor sits at the new artifact),
    // the cursor was painting on top of the artifact-as-dashed
    // tick, hiding the dashing entirely. The fix moves the
    // secondary indicator to AFTER the cursor so the dashes
    // pierce through the cursor's red.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 200);

    auto barlines = std::make_shared<BarlineModel>();
    barlines->add(1000);
    w.setBarlineModel(barlines);

    // Cursor and secondary at the SAME ms — repro condition.
    w.setPositionMs(1000);
    w.setSecondaryAnchorMs(1000);

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    // At x=200 (1000ms in 800/4000 mapping), we should see BOTH
    // yellow rows (dashed bar pierces through) AND red rows
    // (cursor in dash gaps). Without the fix, the column would be
    // pure red — cursor covers the whole height with no yellow
    // showing through.
    const int x = 200;
    int yellowRows = 0;
    int redRows    = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QColor c = image.pixelColor(x, y);
        const bool isYellow = c.red() > 200 && c.green() > 180
                           && c.blue() < 180;
        const bool isRed    = c.red() > 200 && c.green() < 150
                           && c.blue() < 150;
        if (isYellow) ++yellowRows;
        if (isRed)    ++redRows;
    }

    // Both must appear. Without the fix yellowRows would be 0 and
    // redRows would be ~120.
    REQUIRE(yellowRows > 20);
    REQUIRE(redRows    > 20);
}

TEST_CASE("WaveformWidget: setSecondaryAnchorMs is idempotent",
          "[waveform-widget][gui][secondary-anchor]") {
    qtApp();
    WaveformWidget w;
    QSignalSpy spy(&w, &WaveformWidget::secondaryAnchorChanged);

    w.setSecondaryAnchorMs(1000);
    REQUIRE(spy.count() == 1);
    w.setSecondaryAnchorMs(1000);    // same value
    REQUIRE(spy.count() == 1);        // no re-emit
    w.setSecondaryAnchorMs(std::nullopt);
    REQUIRE(spy.count() == 2);
    w.setSecondaryAnchorMs(std::nullopt);
    REQUIRE(spy.count() == 2);
}

// ---------------------------------------------------------------------------
// Drag-to-nudge — markers and loop boundaries (issue #11)
//
// MEMO: drag uses real Qt mouse events (press → moves → release)
// rather than the simpler QTest::mouseClick helper. mouseClick
// emits press + release at the same point; we need intermediate
// moves to cross startDragDistance() and trigger drag mode.
// ---------------------------------------------------------------------------

namespace {

// Helper: drive a press → series of moves → release sequence on the
// widget. Each move sits at (x, 50) so the drag is purely
// horizontal — vertical position is irrelevant for the source-time
// translation.
void dragSequence(WaveformWidget& w, int xPress, int xRelease) {
    const QPoint pressPt {xPress,   50};
    const QPoint endPt   {xRelease, 50};
    QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, pressPt);
    // Two intermediate moves: one inside the threshold (no-op), one
    // past it (commits to drag mode and starts emitting).
    QMouseEvent mvNear(QEvent::MouseMove,
                       QPointF{pressPt + QPoint{2, 0}},
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &mvNear);
    QMouseEvent mvFinal(QEvent::MouseMove,
                        QPointF{endPt},
                        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &mvFinal);
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, endPt);
}

} // namespace

TEST_CASE("WaveformWidget: press marker without moving stays a click (no drag)",
          "[waveform-widget][gui][drag]") {
    // MEMO: regression — the click-vs-drag threshold means a
    // press + release at the same point must still behave like a
    // click (select + seek). No drag-commit signal fires.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::int64_t stamps[] = { 1000 };
    auto markers = installMarkers(
        w, std::span<const std::int64_t>{stamps});
    QSignalSpy commitSpy(&w, &WaveformWidget::markerDragCommitted);

    const int x = w.msToX(1000);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(x, 50));

    REQUIRE(w.selectedMarkerId().has_value());     // click still selects
    REQUIRE(commitSpy.count() == 0);               // but no drag fired
    REQUIRE(markers->markers()[0].sourceMs == 1000); // model unchanged
}

TEST_CASE("WaveformWidget: dragging a marker emits live + commit signals",
          "[waveform-widget][gui][drag]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::int64_t stamps[] = { 1000 };
    auto markers = installMarkers(
        w, std::span<const std::int64_t>{stamps});

    QSignalSpy liveSpy  (&w, &WaveformWidget::markerDragRequested);
    QSignalSpy commitSpy(&w, &WaveformWidget::markerDragCommitted);

    // Wire the live signal to actually mutate the model — that's
    // the contract MainWindow fulfils in production. Without it,
    // the release-time commit reads the model's stale pre-drag
    // position because nothing wrote to it.
    QObject::connect(&w, &WaveformWidget::markerDragRequested,
                     &w, [&markers](std::int64_t id,
                                    std::int64_t newMs) {
                         markers->setPosition(id, newMs);
                     });

    const int xStart = w.msToX(1000);
    const int xEnd   = w.msToX(2000);
    dragSequence(w, xStart, xEnd);

    REQUIRE(liveSpy.count()   >= 1);   // at least one live update
    REQUIRE(commitSpy.count() == 1);   // one commit on release

    const auto args = commitSpy.takeFirst();
    REQUIRE(args[0].toLongLong() == *markers->idAt(0));
    REQUIRE(args[1].toLongLong() == 1000);   // fromMs
    // toMs is whatever the final cursor x maps to. Allow 5 ms slop
    // for the integer rounding in xToMs.
    REQUIRE(std::abs(args[2].toLongLong() - 2000) < 5);
}

TEST_CASE("WaveformWidget: marker drag emits seekRequested per move (cursor follow)",
          "[waveform-widget][gui][drag]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::int64_t stamps[] = { 1000 };
    installMarkers(w, std::span<const std::int64_t>{stamps});
    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);

    const int xStart = w.msToX(1000);
    const int xEnd   = w.msToX(2500);
    dragSequence(w, xStart, xEnd);

    // press fires one seek (existing click behavior) + at least one
    // more on a drag move.
    REQUIRE(seekSpy.count() >= 2);
}

TEST_CASE("WaveformWidget: pressing on a loop edge arms a drag without seeking",
          "[waveform-widget][gui][drag][loops]") {
    // MEMO: loop edges are a NEW hit-test target (#11). Selecting a
    // loop via its edge must not seek — the user is editing region
    // geometry, not navigating playback.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);

    const int xLeftEdge = w.msToX(1000);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xLeftEdge, 50));

    REQUIRE(w.selectedLoopId().has_value());
    REQUIRE(*w.selectedLoopId() == *loops->idAt(0));
    REQUIRE(seekSpy.count() == 0);   // no seek on edge click
}

TEST_CASE("WaveformWidget: dragging a loop's left edge updates startMs",
          "[waveform-widget][gui][drag][loops]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    QSignalSpy liveSpy  (&w, &WaveformWidget::loopDragRequested);
    QSignalSpy commitSpy(&w, &WaveformWidget::loopDragCommitted);

    // Wire the live signal to actually mutate the model — that's
    // what MainWindow does in production. Without this the
    // mouseRelease commit reads stale model state.
    QObject::connect(&w, &WaveformWidget::loopDragRequested,
                     &w, [&loops](std::int64_t id,
                                  std::int64_t newStart,
                                  std::int64_t newEnd) {
                         loops->setRange(id, newStart, newEnd);
                     });

    const int xLeftEdge  = w.msToX(1000);
    const int xLeftFinal = w.msToX(1300);
    dragSequence(w, xLeftEdge, xLeftFinal);

    REQUIRE(liveSpy.count() >= 1);
    REQUIRE(commitSpy.count() == 1);
    const auto args = commitSpy.takeFirst();
    REQUIRE(args[1].toBool() == true);          // isStartEdge
    REQUIRE(args[2].toLongLong() == 1000);      // fromMs
    REQUIRE(std::abs(args[3].toLongLong() - 1300) < 5);
    // Partner edge stayed put.
    REQUIRE(loops->loops()[0].endMs == 2000);
}

TEST_CASE("WaveformWidget: dragging a loop's right edge updates endMs only",
          "[waveform-widget][gui][drag][loops]") {
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    QObject::connect(&w, &WaveformWidget::loopDragRequested,
                     &w, [&loops](std::int64_t id,
                                  std::int64_t newStart,
                                  std::int64_t newEnd) {
                         loops->setRange(id, newStart, newEnd);
                     });
    QSignalSpy commitSpy(&w, &WaveformWidget::loopDragCommitted);

    const int xRightEdge  = w.msToX(2000);
    const int xRightFinal = w.msToX(2700);
    dragSequence(w, xRightEdge, xRightFinal);

    REQUIRE(commitSpy.count() == 1);
    const auto args = commitSpy.takeFirst();
    REQUIRE(args[1].toBool() == false);         // isStartEdge=false → end
    REQUIRE(loops->loops()[0].startMs == 1000); // partner unchanged
    REQUIRE(std::abs(loops->loops()[0].endMs - 2700) < 5);
}

TEST_CASE("WaveformWidget: dragging loop edge near a barline snaps to it",
          "[waveform-widget][gui][drag][loops][snap]") {
    // MEMO: the magnet feel — drag the edge to within ~6 px of a
    // barline, on release the edge ms exactly equals the barline ms.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    auto barlines = std::make_shared<BarlineModel>();
    barlines->add(1500);   // snap target
    w.setBarlineModel(barlines);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    QObject::connect(&w, &WaveformWidget::loopDragRequested,
                     &w, [&loops](std::int64_t id,
                                  std::int64_t newStart,
                                  std::int64_t newEnd) {
                         loops->setRange(id, newStart, newEnd);
                     });

    // Drag the right edge from xMsToX(2000) toward the barline.
    // Land 2 px away from the barline so we're inside the snap
    // tolerance (6 px) — the final ms should equal 1500 exactly.
    const int xRightEdge   = w.msToX(2000);
    const int xBarline     = w.msToX(1500);
    const int xLandNearbar = xBarline + 2;
    dragSequence(w, xRightEdge, xLandNearbar);

    REQUIRE(loops->loops()[0].endMs == 1500);   // snapped
}

TEST_CASE("WaveformWidget: marker takes priority over loop edge when no loop selected",
          "[waveform-widget][gui][drag][loops]") {
    // MEMO: per #11 — "Drag a marker NEAR a loop boundary — they
    // move independently". The marker wins the default hit-test so
    // dragging moves the marker, not the edge. Exception when a
    // loop is selected — see the next test.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::int64_t stamps[] = { 1000 };
    auto markers = installMarkers(
        w, std::span<const std::int64_t>{stamps});

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    // No loop selected — default priority applies.
    REQUIRE_FALSE(w.selectedLoopId().has_value());

    QSignalSpy markerSpy(&w, &WaveformWidget::markerDragRequested);
    QSignalSpy loopSpy  (&w, &WaveformWidget::loopDragRequested);

    const int xCoincident = w.msToX(1000);
    const int xEnd        = w.msToX(1200);
    dragSequence(w, xCoincident, xEnd);

    REQUIRE(markerSpy.count() >= 1);
    REQUIRE(loopSpy.count()   == 0);
    (void)markers; (void)loops;
}

TEST_CASE("WaveformWidget: empty-space click preserves loop selection",
          "[waveform-widget][gui][drag][loops][selected-z-order]") {
    // MEMO[smoke #11]: confirmed during smoke testing — clicking
    // empty space on the waveform must NOT clear the loop
    // selection. The user might want to scrub-seek while keeping
    // the loop in "edit mode" so they can come back and nudge an
    // edge afterwards. Loop selection is cleared only by clicking
    // a different artifact (cross-kind mutex) or by the dock.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    w.setSelectedLoopId(*loops->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    QSignalSpy seekSpy(&w, &WaveformWidget::seekRequested);

    // Click far from any artifact (and far from the loop edges).
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(w.msToX(3500), 50));

    REQUIRE(seekSpy.count() == 1);                    // empty click → seek
    REQUIRE(w.selectedLoopId().has_value());          // loop still selected
}

TEST_CASE("WaveformWidget: selected loop's edges win over coincident markers",
          "[waveform-widget][gui][drag][loops][selected-z-order]") {
    // MEMO[smoke #11]: a loop created from markers had its edges
    // permanently unreachable by drag — the marker at the edge's
    // x always won. After this fix, when the loop is the selected
    // artifact (e.g. immediately after creation), its edges take
    // hit-test priority over coincident markers. The marker is
    // still draggable: click it (cross-kind mutex switches
    // selection), then drag.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::int64_t stamps[] = { 1000, 2000 };
    auto markers = installMarkers(
        w, std::span<const std::int64_t>{stamps});

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    // Auto-select the loop (mirrors what onCreateLoop does in
    // MainWindow after the user presses L).
    w.setSelectedLoopId(*loops->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    // Wire live-mutate so the drag actually moves the model.
    QObject::connect(&w, &WaveformWidget::loopDragRequested,
                     &w, [&loops](std::int64_t id,
                                  std::int64_t newStart,
                                  std::int64_t newEnd) {
                         loops->setRange(id, newStart, newEnd);
                     });

    QSignalSpy markerSpy(&w, &WaveformWidget::markerDragRequested);
    QSignalSpy loopSpy  (&w, &WaveformWidget::loopDragRequested);

    // Press at the loop's right edge — which coincides with the
    // marker at 2000 — and drag. With the new rule the loop edge
    // wins despite the marker being there.
    const int xRightEdge = w.msToX(2000);
    const int xEndDrag   = w.msToX(2400);
    dragSequence(w, xRightEdge, xEndDrag);

    REQUIRE(loopSpy.count()   >= 1);
    REQUIRE(markerSpy.count() == 0);
    // Marker stayed put.
    REQUIRE(markers->markers()[1].sourceMs == 2000);
}

TEST_CASE("WaveformWidget: loop edge drag past partner edge is rejected",
          "[waveform-widget][gui][drag][loops]") {
    // MEMO: the start < end invariant — the widget refuses to emit
    // a drag request that would invert the loop. No live updates
    // for the offending move, no commit signal at release (because
    // dragActive_ stayed unset — we never crossed threshold while
    // pointing somewhere valid... actually we do cross threshold,
    // we just don't emit live updates for invalid positions).
    //
    // The artifact's range stays at its original value, and the
    // commit signal still fires (dragActive_ flips on the first
    // threshold-crossing move regardless of whether it produced an
    // emission). The toMs in the commit reflects the original
    // value because nothing moved.
    qtApp();
    WaveformWidget w;
    w.setOverview(makeOverview(/*seconds=*/4));
    w.resize(800, 100);

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    QObject::connect(&w, &WaveformWidget::loopDragRequested,
                     &w, [&loops](std::int64_t id,
                                  std::int64_t newStart,
                                  std::int64_t newEnd) {
                         loops->setRange(id, newStart, newEnd);
                     });

    // Press on the left edge, drag PAST the right edge.
    const int xLeftEdge = w.msToX(1000);
    const int xPastEnd  = w.msToX(2500);
    dragSequence(w, xLeftEdge, xPastEnd);

    // Range is unchanged — invariant held.
    REQUIRE(loops->loops()[0].startMs == 1000);
    REQUIRE(loops->loops()[0].endMs   == 2000);
}
