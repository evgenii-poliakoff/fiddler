// GUI tests for StaffWidget. The widget pattern mirrors WaveformWidget
// (click → seek + select, key-nav, Esc, Del, model-driven repaint),
// so these tests follow the same shape — drive real Qt events via
// QTest, observe outputs via QSignalSpy, run headless against the
// "offscreen" platform plugin.

#include "qt_test_app.h"
#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "ui/StaffWidget.h"

#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <span>

using fiddler::score::BarlineModel;
using fiddler::score::LoopModel;
using fiddler::score::MarkerModel;
using fiddler::test::qtApp;
using fiddler::ui::StaffWidget;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Construct a model with the given barlines pre-loaded. Returned as
// shared_ptr so the widget can take a const view; the test still
// holds a non-const handle to mutate it later.
std::shared_ptr<BarlineModel>
makeModel(std::span<const std::int64_t> stamps = {}) {
    auto model = std::make_shared<BarlineModel>();
    for (auto ms : stamps) {
        model->add(ms);
    }
    return model;
}

// Configure a StaffWidget with a typical "loaded file" state:
//   - durationMs as given (default 4000 = 4 s)
//   - the supplied barline model attached
//   - resized to a width that makes msToX easy to reason about
//     (1 ms per pixel when w=4000, etc.)
void setUpWidget(StaffWidget&                   w,
                 std::shared_ptr<BarlineModel>  model,
                 std::int64_t                   durationMs = 4000,
                 int                            widthPx    = 800,
                 int                            heightPx   = 100)
{
    w.setBarlineModel(model);
    w.setDurationMs(durationMs);
    w.resize(widthPx, heightPx);
}

} // namespace

// ---------------------------------------------------------------------------
// Empty / degenerate state
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: paints without crashing in the empty default state",
          "[staff-widget][gui]") {
    qtApp();
    StaffWidget w;
    w.resize(400, 100);
    w.show();
    QTest::qWait(20);     // let any pending paint event flush
    SUCCEED();
}

TEST_CASE("StaffWidget: coord transforms return 0 when no duration is set",
          "[staff-widget][gui][coords]") {
    qtApp();
    StaffWidget w;
    w.resize(800, 100);
    REQUIRE(w.xToMs(0)    == 0);
    REQUIRE(w.xToMs(400)  == 0);
    REQUIRE(w.msToX(0)    == 0);
    REQUIRE(w.msToX(5000) == 0);
}

TEST_CASE("StaffWidget: click without a duration emits no signals",
          "[staff-widget][gui]") {
    qtApp();
    StaffWidget w;
    w.resize(400, 100);

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(200, 50));
    REQUIRE(seekSpy.count() == 0);
}

// ---------------------------------------------------------------------------
// Coordinate transforms
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: xToMs / msToX round-trip across the width",
          "[staff-widget][gui][coords]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/10'000,
                /*widthPx=*/800, /*heightPx=*/80);

    for (int x = 0; x < 800; x += 37) {
        const auto ms        = w.xToMs(x);
        const int  roundTrip = w.msToX(ms);
        // ±1 px slack for integer truncation on either side.
        REQUIRE(roundTrip >= x - 1);
        REQUIRE(roundTrip <= x + 1);
    }
}

TEST_CASE("StaffWidget: out-of-range x and ms clamp to file bounds",
          "[staff-widget][gui][coords]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/3000,
                /*widthPx=*/600, /*heightPx=*/80);

    REQUIRE(w.xToMs(-100)   == 0);
    REQUIRE(w.xToMs(99'999) == 3000);
    REQUIRE(w.msToX(-500)   == 0);
    REQUIRE(w.msToX(99'999) == 599);   // width - 1
}

// ---------------------------------------------------------------------------
// Click → select + seek
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: click on a barline selects it and seeks to its ms",
          "[staff-widget][gui][barlines]") {
    qtApp();
    StaffWidget w;
    // 4 s file, 800 px wide → 1 ms per 0.2 px. A barline at 1000 ms
    // maps to x = 200.
    const std::int64_t stamps[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    QSignalSpy selSpy (&w, &StaffWidget::barlineSelectionChanged);

    // Click at x=202 — within the 5 px tolerance of the tick at 200.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(202, 50));

    REQUIRE(w.selectedBarline() == 0);

    // The seek emission carries the barline's exact ms (1000), not
    // the click's ms (~1010), so the cursor lands on the tick.
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 1000);

    REQUIRE(selSpy.count() == 1);
    REQUIRE(selSpy.takeFirst().at(0).value<std::optional<std::size_t>>()
            == std::optional<std::size_t>{0});
}

TEST_CASE("StaffWidget: click far from any barline seeks without selecting",
          "[staff-widget][gui][barlines]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    QSignalSpy selSpy (&w, &StaffWidget::barlineSelectionChanged);

    // Click at x=600 (3000 ms) — nowhere near the only barline at 200.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(600, 50));

    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(seekSpy.count() == 1);
    const auto seekedMs = seekSpy.takeFirst().at(0).toLongLong();
    REQUIRE(seekedMs >= 2980);
    REQUIRE(seekedMs <= 3020);

    // No selection change to report (selection was already empty).
    REQUIRE(selSpy.count() == 0);
}

TEST_CASE("StaffWidget: clicking elsewhere clears an existing selection",
          "[staff-widget][gui][barlines]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));

    // Pre-select via a click on the bar.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(200, 50));
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy selSpy(&w, &StaffWidget::barlineSelectionChanged);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(600, 50));

    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(selSpy.count() == 1);
    REQUIRE(selSpy.takeFirst().at(0).value<std::optional<std::size_t>>()
            == std::optional<std::size_t>{});
}

TEST_CASE("StaffWidget: right click does not emit any signals",
          "[staff-widget][gui][barlines]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    QSignalSpy selSpy (&w, &StaffWidget::barlineSelectionChanged);

    QTest::mouseClick(&w, Qt::RightButton,  Qt::NoModifier, QPoint(200, 50));
    QTest::mouseClick(&w, Qt::MiddleButton, Qt::NoModifier, QPoint(200, 50));

    REQUIRE(seekSpy.count() == 0);
    REQUIRE(selSpy.count()  == 0);
}

// ---------------------------------------------------------------------------
// Keyboard nav
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: arrow keys navigate selection between barlines",
          "[staff-widget][gui][barlines][keys]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 500, 1500, 2500, 3500 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);

    // Pre-select the second barline (1500 ms → x=300).
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(300, 50));
    REQUIRE(w.selectedBarline() == 1);

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    seekSpy.clear();

    QTest::keyClick(&w, Qt::Key_Right);
    REQUIRE(w.selectedBarline() == 2);
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 2500);

    QTest::keyClick(&w, Qt::Key_Left);
    QTest::keyClick(&w, Qt::Key_Left);
    REQUIRE(w.selectedBarline() == 0);
}

TEST_CASE("StaffWidget: arrow keys saturate cleanly at first / last entry",
          "[staff-widget][gui][barlines][keys]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);

    w.setSelectedBarline(2);                   // last
    QTest::keyClick(&w, Qt::Key_Right);
    REQUIRE(w.selectedBarline() == 2);          // unchanged at end

    w.setSelectedBarline(0);                   // first
    QTest::keyClick(&w, Qt::Key_Left);
    REQUIRE(w.selectedBarline() == 0);          // unchanged at start
}

TEST_CASE("StaffWidget: Esc clears the selection",
          "[staff-widget][gui][barlines][keys]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);
    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy selSpy(&w, &StaffWidget::barlineSelectionChanged);
    QTest::keyClick(&w, Qt::Key_Escape);

    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(selSpy.count() == 1);
}

TEST_CASE("StaffWidget: Del fires barlineDeleteRequested with the selected index",
          "[staff-widget][gui][barlines][keys]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);
    w.setSelectedBarline(1);

    QSignalSpy delSpy(&w, &StaffWidget::barlineDeleteRequested);
    QTest::keyClick(&w, Qt::Key_Delete);

    REQUIRE(delSpy.count() == 1);
    REQUIRE(delSpy.takeFirst().at(0).value<std::size_t>() == 1u);
}

TEST_CASE("StaffWidget: Del with no selection does nothing",
          "[staff-widget][gui][barlines][keys]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);

    QSignalSpy delSpy(&w, &StaffWidget::barlineDeleteRequested);
    QTest::keyClick(&w, Qt::Key_Delete);
    REQUIRE(delSpy.count() == 0);
}

// ---------------------------------------------------------------------------
// Model-driven state
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: model removeAt of selected entry clears the selection",
          "[staff-widget][gui][barlines]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    auto model = makeModel(std::span<const std::int64_t>{stamps});
    setUpWidget(w, model);

    w.setSelectedBarline(2);
    REQUIRE(w.selectedBarline() == 2);

    QSignalSpy selSpy(&w, &StaffWidget::barlineSelectionChanged);
    model->removeAt(2);                  // makes index 2 invalid

    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(selSpy.count() >= 1);
}

TEST_CASE("StaffWidget: setBarlineModel(nullptr) detaches and clears selection",
          "[staff-widget][gui][barlines]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));
    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline() == 0);

    w.setBarlineModel(nullptr);

    REQUIRE(w.barlineModel() == nullptr);
    REQUIRE_FALSE(w.selectedBarline().has_value());
}

TEST_CASE("StaffWidget: setSelectedBarline coerces out-of-range to nullopt",
          "[staff-widget][gui][barlines]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 500, 1500 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));

    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline() == 0);

    w.setSelectedBarline(99);            // out of range
    REQUIRE_FALSE(w.selectedBarline().has_value());
}

TEST_CASE("StaffWidget: paint survives extreme widget sizes",
          "[staff-widget][gui]") {
    qtApp();
    StaffWidget w;
    const std::int64_t stamps[] = { 500, 1500 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));

    for (const auto& sz : { QSize(1, 1),  QSize(1, 200),
                            QSize(2000, 40), QSize(50, 4) }) {
        w.resize(sz);
        w.show();
        QTest::qWait(5);
    }
    SUCCEED();
}

TEST_CASE("StaffWidget: paint with custom time signature + tune-type label",
          "[staff-widget][gui]") {
    qtApp();
    StaffWidget w;
    auto model = makeModel();
    model->setTimeSignature({6, 8, "Jig"});
    setUpWidget(w, model);
    w.show();
    QTest::qWait(20);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Marker overlay
// ---------------------------------------------------------------------------

namespace {

// Build a fresh MarkerModel and attach it to the widget. Returns
// the model handle so the test can mutate it later.
std::shared_ptr<MarkerModel>
installMarkers(StaffWidget& w, std::span<const std::int64_t> stamps) {
    auto model = std::make_shared<MarkerModel>();
    for (auto ms : stamps) (void)model->add(ms);
    w.setMarkerModel(model);
    return model;
}

} // namespace

TEST_CASE("StaffWidget: paints marker ticks + label flags without crashing",
          "[staff-widget][gui][markers]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());                  // tune-type label active
    const std::int64_t stamps[] = { 500, 1500, 2500 };
    installMarkers(w, std::span<const std::int64_t>{stamps});
    w.show();
    QTest::qWait(20);
    SUCCEED();
}

TEST_CASE("StaffWidget: click on a marker selects by ID and seeks",
          "[staff-widget][gui][markers]") {
    // 4 s file across 800 px → marker at 1000 ms is at x=200.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::int64_t stamps[] = { 1000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    QSignalSpy selSpy (&w, &StaffWidget::markerSelectionChanged);

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(202, 40));

    REQUIRE(w.selectedMarkerId() == *model->idAt(0));
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 1000);
    REQUIRE(selSpy.count() == 1);
}

TEST_CASE("StaffWidget: marker click clears barline selection (mutual exclusion)",
          "[staff-widget][gui][markers]") {
    qtApp();
    StaffWidget w;
    const std::int64_t bars[] = { 500 };           // x=100
    auto barModel = makeModel(std::span<const std::int64_t>{bars});
    setUpWidget(w, barModel);
    const std::int64_t markers[] = { 2000 };       // x=400
    installMarkers(w, std::span<const std::int64_t>{markers});

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(102, 40));
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy barSelSpy(&w, &StaffWidget::barlineSelectionChanged);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(402, 40));

    REQUIRE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(barSelSpy.count() == 1);
}

TEST_CASE("StaffWidget: Del with marker selection fires markerDeleteRequested",
          "[staff-widget][gui][markers][keys]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::int64_t stamps[] = { 1000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.show();
    w.setFocus();
    (void)QTest::qWaitForWindowExposed(&w);
    w.setSelectedMarkerId(*model->idAt(0));

    QSignalSpy delSpy(&w, &StaffWidget::markerDeleteRequested);
    QTest::keyClick(&w, Qt::Key_Delete);
    REQUIRE(delSpy.count() == 1);
    REQUIRE(delSpy.takeFirst().at(0).value<std::int64_t>()
            == *model->idAt(0));
}

TEST_CASE("StaffWidget: marker setPosition keeps ID-based selection alive",
          "[staff-widget][gui][markers]") {
    // Stable-ID survival regression — see the equivalent waveform
    // test for the rationale.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::int64_t stamps[] = { 1000, 2000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    const auto firstId = *model->idAt(0);

    w.setSelectedMarkerId(firstId);
    REQUIRE(model->setPosition(firstId, 3000));   // moves past second
    REQUIRE(w.selectedMarkerId() == firstId);
    REQUIRE(*model->indexOf(firstId) == 1);
}

// ---------------------------------------------------------------------------
// Loop overlay
//
// MEMO[refactor]: same scope as the WaveformWidget loop tests —
// render-only, with mutual-exclusion across all three artifact kinds.
// Click-to-select-loop and double-click-to-arm aren't part of this
// commit, so they aren't tested.
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<LoopModel>
installLoops(StaffWidget& w,
             std::span<const std::pair<std::int64_t, std::int64_t>> ranges) {
    auto model = std::make_shared<LoopModel>();
    for (const auto& r : ranges) (void)model->add(r.first, r.second);
    w.setLoopModel(model);
    return model;
}

} // namespace

TEST_CASE("StaffWidget: paints loop bands without crashing",
          "[staff-widget][gui][loops]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1500}, {2000, 3000}
    };
    installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.show();
    QTest::qWait(20);
    SUCCEED();
}

TEST_CASE("StaffWidget: setSelectedLoopId clears barline + marker selections",
          "[staff-widget][gui][loops]") {
    qtApp();
    StaffWidget w;
    const std::int64_t bars[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{bars}));

    const std::int64_t markers[] = { 2000 };
    auto markerModel = installMarkers(w, std::span<const std::int64_t>{markers});

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto loopModel = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline().has_value());
    w.setSelectedMarkerId(*markerModel->idAt(0));
    REQUIRE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedBarline().has_value());

    QSignalSpy markerSpy(&w, &StaffWidget::markerSelectionChanged);
    QSignalSpy loopSpy  (&w, &StaffWidget::loopSelectionChanged);

    w.setSelectedLoopId(*loopModel->idAt(0));

    REQUIRE(w.selectedLoopId().has_value());
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedBarline().has_value());
    REQUIRE(markerSpy.count() == 1);
    REQUIRE(loopSpy.count()   == 1);
}

TEST_CASE("StaffWidget: setSelectedBarline clears an active loop selection",
          "[staff-widget][gui][loops]") {
    qtApp();
    StaffWidget w;
    const std::int64_t bars[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{bars}));

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto loopModel = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.setSelectedLoopId(*loopModel->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    QSignalSpy loopSpy(&w, &StaffWidget::loopSelectionChanged);
    w.setSelectedBarline(0);

    REQUIRE(w.selectedBarline().has_value());
    REQUIRE_FALSE(w.selectedLoopId().has_value());
    REQUIRE(loopSpy.count() == 1);
}

TEST_CASE("StaffWidget: setSelectedMarkerId clears an active loop selection",
          "[staff-widget][gui][loops]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());

    const std::int64_t markers[] = { 2000 };
    auto markerModel = installMarkers(w, std::span<const std::int64_t>{markers});

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto loopModel = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});

    w.setSelectedLoopId(*loopModel->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    QSignalSpy loopSpy(&w, &StaffWidget::loopSelectionChanged);
    w.setSelectedMarkerId(*markerModel->idAt(0));

    REQUIRE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.selectedLoopId().has_value());
    REQUIRE(loopSpy.count() == 1);
}

TEST_CASE("StaffWidget: loop setRange keeps selection alive across re-sort",
          "[staff-widget][gui][loops]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());

    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1000}, {2000, 2500}
    };
    auto model = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    const auto firstId = *model->idAt(0);
    w.setSelectedLoopId(firstId);

    REQUIRE(model->setRange(firstId, 3000, 3500));
    REQUIRE(w.selectedLoopId() == firstId);
    REQUIRE(*model->indexOf(firstId) == 1);
}

TEST_CASE("StaffWidget: removing the selected loop clears the selection",
          "[staff-widget][gui][loops]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::pair<std::int64_t, std::int64_t> ranges[] = {
        {500, 1000}, {2000, 2500}
    };
    auto model = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    const auto secondId = *model->idAt(1);
    w.setSelectedLoopId(secondId);
    REQUIRE(w.selectedLoopId().has_value());

    QSignalSpy selSpy(&w, &StaffWidget::loopSelectionChanged);
    REQUIRE(model->remove(secondId));
    REQUIRE_FALSE(w.selectedLoopId().has_value());
    REQUIRE(selSpy.count() >= 1);
}

TEST_CASE("StaffWidget: setLoopModel(nullptr) detaches and clears selection",
          "[staff-widget][gui][loops]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::pair<std::int64_t, std::int64_t> ranges[] = { {500, 1500} };
    auto model = installLoops(w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    w.setSelectedLoopId(*model->idAt(0));
    REQUIRE(w.selectedLoopId().has_value());

    w.setLoopModel(nullptr);
    REQUIRE(w.loopModel() == nullptr);
    REQUIRE_FALSE(w.selectedLoopId().has_value());
}
