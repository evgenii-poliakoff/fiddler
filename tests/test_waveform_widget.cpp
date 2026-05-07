// GUI tests for WaveformWidget. Driven through real Qt event
// machinery (QTest::mouseClick + QSignalSpy), running on the
// "offscreen" platform plugin so the suite stays headless on CI.

#include "audio/WaveformOverview.h"
#include "qt_test_app.h"
#include "score/BarlineModel.h"
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
