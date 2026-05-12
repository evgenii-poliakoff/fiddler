// GUI tests for StaffWidget. The widget pattern mirrors WaveformWidget
// (click → seek + select, key-nav, Esc, Del, model-driven repaint),
// so these tests follow the same shape — drive real Qt events via
// QTest, observe outputs via QSignalSpy, run headless against the
// "offscreen" platform plugin.

#include "qt_test_app.h"
#include "score/BarlineModel.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "score/NoteModel.h"
#include "ui/StaffWidget.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <utility>

using fiddler::score::BarlineModel;
using fiddler::score::LoopModel;
using fiddler::score::MarkerModel;
using fiddler::score::NoteModel;
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
                 int                            heightPx   = 140)
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

    // xToMs clamps to [0, dur] — values outside the widget map
    // to the file's bounds (so a click off the left/right edge
    // still resolves to a sane source-ms).
    REQUIRE(w.xToMs(-100)   == 0);
    REQUIRE(w.xToMs(99'999) == 3000);
    // msToX deliberately does NOT clamp: it returns out-of-range
    // values for out-of-range source-ms so paint code's
    // `if (x<0 || x>=width()) continue` bounds check can cull
    // off-screen artifacts (#49 follow-up — the prior clamp had
    // off-screen markers drawing as a column stack at x=0).
    // Only ms == durationMs is nudged to width-1 so the exact
    // right edge still paints.
    REQUIRE(w.msToX(-500)   <  0);
    REQUIRE(w.msToX(99'999) >  599);
    REQUIRE(w.msToX(3000)   == 599);   // exact right-edge nudge
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

// ---------------------------------------------------------------------------
// Secondary anchor (Ctrl+click multi-select for loop creation)
//
// MEMO[refactor]: the StaffWidget mirrors the WaveformWidget rules
// for the loop-creation gesture; canonical commentary lives in the
// waveform tests. These cases verify the staff implementation
// follows the same shape, since both widgets feed MainWindow's L
// shortcut.
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: Ctrl+click on a marker promotes prior primary to secondary",
          "[staff-widget][gui][secondary-anchor]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::int64_t stamps[] = { 1000, 3000 };
    installMarkers(w, std::span<const std::int64_t>{stamps});

    // 4-second file, 800-pixel widget: 1000ms → x=200, 3000ms → x=600.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(200, 50));
    REQUIRE(w.primaryAnchorMs() == 1000);
    REQUIRE_FALSE(w.secondaryAnchorMs().has_value());

    QTest::mouseClick(&w, Qt::LeftButton, Qt::ControlModifier,
                      QPoint(600, 50));
    REQUIRE(w.primaryAnchorMs() == 3000);
    REQUIRE(w.secondaryAnchorMs() == 1000);
}

TEST_CASE("StaffWidget: Esc clears the secondary anchor",
          "[staff-widget][gui][secondary-anchor]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    w.setSecondaryAnchorMs(1500);
    REQUIRE(w.secondaryAnchorMs().has_value());

    const std::int64_t stamps[] = { 1500 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});
    w.setSelectedMarkerId(*model->idAt(0));

    QTest::keyClick(&w, Qt::Key_Escape);
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
    REQUIRE_FALSE(w.secondaryAnchorMs().has_value());
}

// ---------------------------------------------------------------------------
// Drag-to-nudge (issue #11) — confirm the base-class plumbing reaches
// the staff subclass too. The detailed gesture matrix (snap, marker-
// over-edge priority, invariant rejection) is pinned in
// test_waveform_widget.cpp; here we only sanity-check that the
// signals fire on the staff so the base/subclass split holds.
// ---------------------------------------------------------------------------

namespace {

void dragSequence(StaffWidget& w, int xPress, int xRelease) {
    const QPoint pressPt {xPress,   50};
    const QPoint endPt   {xRelease, 50};
    QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, pressPt);
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

TEST_CASE("StaffWidget: dragging a marker emits live + commit signals",
          "[staff-widget][gui][drag]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());

    const std::int64_t stamps[] = { 1000 };
    auto markers = installMarkers(
        w, std::span<const std::int64_t>{stamps});
    QObject::connect(&w, &StaffWidget::markerDragRequested,
                     &w, [&markers](std::int64_t id,
                                    std::int64_t newMs) {
                         markers->setPosition(id, newMs);
                     });

    QSignalSpy commitSpy(&w, &StaffWidget::markerDragCommitted);

    const int xStart = w.msToX(1000);
    const int xEnd   = w.msToX(2000);
    dragSequence(w, xStart, xEnd);

    REQUIRE(commitSpy.count() == 1);
    const auto args = commitSpy.takeFirst();
    REQUIRE(args[1].toLongLong() == 1000);
    REQUIRE(std::abs(args[2].toLongLong() - 2000) < 5);
}

TEST_CASE("StaffWidget: dragging a loop's left edge updates startMs",
          "[staff-widget][gui][drag][loops]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());

    const std::pair<std::int64_t, std::int64_t> ranges[] = { {1000, 2000} };
    auto loops = installLoops(
        w, std::span<const std::pair<std::int64_t, std::int64_t>>{ranges});
    QObject::connect(&w, &StaffWidget::loopDragRequested,
                     &w, [&loops](std::int64_t id,
                                  std::int64_t newStart,
                                  std::int64_t newEnd) {
                         loops->setRange(id, newStart, newEnd);
                     });
    QSignalSpy commitSpy(&w, &StaffWidget::loopDragCommitted);

    const int xLeftEdge  = w.msToX(1000);
    const int xLeftFinal = w.msToX(1300);
    dragSequence(w, xLeftEdge, xLeftFinal);

    REQUIRE(commitSpy.count() == 1);
    const auto args = commitSpy.takeFirst();
    REQUIRE(args[1].toBool() == true);          // isStartEdge
    REQUIRE(loops->loops()[0].endMs == 2000);   // partner intact
    REQUIRE(std::abs(loops->loops()[0].startMs - 1300) < 5);
}

// ---------------------------------------------------------------------------
// Viewport / zoom (#49)
//
// These tests pin the ScoreOverlayBase viewport contract via StaffWidget
// because the base class isn't instantiable on its own. The same math
// covers WaveformWidget (it lives in the base now).
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: default viewport maps full duration to widget width",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    REQUIRE_FALSE(w.isZoomed());
    REQUIRE(w.viewportSpanMs() == 0);   // unset = fit-to-window

    // 4000 ms across 800 px = 5 ms/px.
    REQUIRE(w.xToMs(0)   == 0);
    REQUIRE(w.xToMs(400) == 2000);
    REQUIRE(w.msToX(2000) == 400);
}

TEST_CASE("StaffWidget: setting a viewport maps that range to the full width",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    QSignalSpy vpSpy(&w, &StaffWidget::viewportChanged);
    w.setViewport(1000, 2000);     // 1-second window across 800 px

    REQUIRE(vpSpy.count() == 1);
    REQUIRE(w.isZoomed());
    REQUIRE(w.viewportStartMs() == 1000);
    REQUIRE(w.viewportEndMs()   == 2000);

    // 1000 ms / 800 px = 1.25 ms/px.
    REQUIRE(w.xToMs(0)   == 1000);
    REQUIRE(w.xToMs(400) == 1500);
    REQUIRE(w.msToX(1500) == 400);
    // ms == viewportEnd lands at exact width — nudged to w-1
    // so the right-edge column still paints. Anything strictly
    // outside the viewport is returned unclamped so paint code
    // can cull (#49 follow-up).
    REQUIRE(w.msToX(2000) == 799);
    REQUIRE(w.msToX(0)    <  0);     // off-screen left
    REQUIRE(w.msToX(3000) >= 800);   // off-screen right
}

TEST_CASE("StaffWidget: setViewport clamps to duration and enforces min span",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    // Out-of-range pair gets clamped to [0, dur].
    w.setViewport(-500, 5000);
    REQUIRE(w.viewportStartMs() == 0);
    REQUIRE(w.viewportEndMs()   == 4000);

    // Sub-minimum span (kMinViewportSpanMs = 50) gets extended.
    w.setViewport(1000, 1010);
    REQUIRE(w.viewportEndMs() - w.viewportStartMs() >= 50);
}

TEST_CASE("StaffWidget: setViewport(0,0) restores fit-to-window",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    w.setViewport(500, 1500);
    REQUIRE(w.isZoomed());

    QSignalSpy vpSpy(&w, &StaffWidget::viewportChanged);
    w.setViewport(0, 0);

    REQUIRE(vpSpy.count() == 1);
    REQUIRE_FALSE(w.isZoomed());
    REQUIRE(w.xToMs(400) == 2000);  // back to full-range math
}

TEST_CASE("StaffWidget: zoomBy preserves the anchor's pixel position",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/10000, /*widthPx=*/1000);

    // Pre-zoom, ms=2500 sits at x=250.
    const std::int64_t anchorMs = 2500;
    const int          anchorPx = w.msToX(anchorMs);
    REQUIRE(anchorPx == 250);

    // Zoom in 2×. Anchor should stay at (approximately) the same x.
    w.zoomBy(0.5, anchorMs);
    REQUIRE(w.isZoomed());

    const int anchorPxAfter = w.msToX(anchorMs);
    // ±1 px slack for integer truncation in span/anchor math.
    REQUIRE(std::abs(anchorPxAfter - anchorPx) <= 1);
}

TEST_CASE("StaffWidget: zoomBy beyond fit collapses to fit-to-window",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    w.setViewport(1000, 2000);   // zoomed in
    REQUIRE(w.isZoomed());

    w.zoomBy(10.0, 1500);        // way past fit
    REQUIRE_FALSE(w.isZoomed());
    REQUIRE(w.viewportSpanMs() == 0);
}

TEST_CASE("StaffWidget: panBy slides the viewport, clamped to bounds",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    w.setViewport(1000, 2000);
    w.panBy(300);
    REQUIRE(w.viewportStartMs() == 1300);
    REQUIRE(w.viewportEndMs()   == 2300);

    // Clamped at the right edge — start stays inside [0, dur - span].
    w.panBy(5000);
    REQUIRE(w.viewportEndMs() == 4000);
    REQUIRE(w.viewportStartMs() == 3000);

    // Clamped at the left edge.
    w.panBy(-10000);
    REQUIRE(w.viewportStartMs() == 0);
    REQUIRE(w.viewportEndMs()   == 1000);
}

TEST_CASE("StaffWidget: panBy is a no-op when not zoomed",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    QSignalSpy vpSpy(&w, &StaffWidget::viewportChanged);
    w.panBy(500);
    REQUIRE(vpSpy.count() == 0);
    REQUIRE_FALSE(w.isZoomed());
}

// ---------------------------------------------------------------------------
// Notes (Step 6.1)
//
// The fixture 4 s / 800 px gives 1 ms = 0.2 px, so a 400 ms note from
// 1000 ms to 1400 ms spans pixels [200, 280). The treble-staff
// geometry constants are mirrored from StaffWidget.cpp.
// ---------------------------------------------------------------------------

namespace {

// Mirror of the constants in StaffWidget.cpp. Bumped in #step6.1
// when the top margin grew to leave room for E7 ledger lines.
constexpr int kStaffTopMargin = 72;
constexpr int kStaffSpacing   = 8;
constexpr int kBottomLineY    = kStaffTopMargin + 4 * kStaffSpacing; // 104
constexpr int kPxPerHalfStep  = kStaffSpacing / 2;                   // 4

std::shared_ptr<NoteModel> installNotes(StaffWidget& w) {
    auto model = std::make_shared<NoteModel>();
    w.setNoteModel(model);
    return model;
}

bool isNoteGreen(QColor c) {
    // Note fill is (180, 220, 140, 200) over background (20,20,24).
    // After alpha-blend the on-screen colour lands around (95, 140, 90).
    return c.green() > c.red() && c.green() > c.blue()
        && c.green() > 100;
}

} // namespace

TEST_CASE("StaffWidget: paints note bar at expected y for E4 (bottom line)",
          "[staff-widget][gui][notes]") {
    // MEMO: load-bearing — staff-Y geometry. E4 sits ON the bottom
    // staff line; if the formula drifts, every painted note moves.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);   // E4

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    // Bar centre-x is around (200 + 280) / 2 = 240. Pick a column
    // inside the bar but away from edges so the pen border doesn't
    // dominate the probe.
    const int probeX = 240;
    REQUIRE(isNoteGreen(image.pixelColor(probeX, kBottomLineY)));

    // Above and below the bar (4+ px away from y=50), the column
    // should NOT read as note-green — we just see staff background
    // or a staff line. Probe well above the bar to dodge the line
    // and any selection ring.
    REQUIRE_FALSE(isNoteGreen(image.pixelColor(probeX, kBottomLineY - 12)));
    REQUIRE_FALSE(isNoteGreen(image.pixelColor(probeX, kBottomLineY + 12)));
}

TEST_CASE("StaffWidget: A4 sits in the second-from-bottom space",
          "[staff-widget][gui][notes]") {
    // E4 = bottom line; F4 = first space; G4 = 2nd line; A4 = 2nd
    // space. Bottom line is y=50; A4 (3 diatonic steps up) is at
    // y=50 - 3*4 = 38.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 69) != 0);   // A4

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    REQUIRE(isNoteGreen(image.pixelColor(240, kBottomLineY - 3 * kPxPerHalfStep)));
}

TEST_CASE("StaffWidget: middle-C note draws a ledger line below the staff",
          "[staff-widget][gui][notes][ledger]") {
    // MEMO: load-bearing — middle C (C4 = midi 60) is dia 28, one
    // diatonic step (2*4 = 8 px) below the bottom line. A ledger
    // line must appear at that y, painted in the note's border
    // colour. If the ledger walk regresses, this fails first.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 60) != 0);   // C4

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    // Middle-C y = bottomY + 2 * kPxPerHalfStep = 58.
    const int middleCY = kBottomLineY + 2 * kPxPerHalfStep;
    // The note bar itself spans middleCY ± 3 px. The ledger stub
    // extends ±8 px around the bar centre x=240; check just outside
    // the bar's x range (xLeft=200, xRight=280) for a border-colour
    // pixel that can only be the ledger stub. Bar mid is 240; stub
    // covers 232..248. Bar covers 200..280. So an x INSIDE the bar
    // (say 236) at exactly middleCY is inside the bar AND on the
    // ledger — the pixel reads as note-green there.
    //
    // To prove the ledger specifically, probe just below the bar's
    // bottom edge (middleCY + 4) — without a ledger this would be
    // staff background; we expect background because the ledger
    // stub is only one pixel thick at middleCY. So instead, probe
    // the ledger overhang region: x between 236 and 247 sits inside
    // the ledger stub but OUTSIDE the bar's vertical range — no,
    // that's not right either.
    //
    // Simplest check: the ledger stub at x=232..247 lies UNDER the
    // bar (same x range as the bar). The bar covers y in (middleCY-3,
    // middleCY+3). The ledger stub is at exactly y=middleCY, so it
    // sits under the bar — we don't see it through the bar fill.
    //
    // To make the ledger visible we need to probe OUTSIDE the bar's
    // horizontal range. The bar's xLeft is 200 (msToX(1000)). The
    // ledger stub centred at barMidX=240 extends to x=232..247 — all
    // inside the bar. So in this fixture the ledger isn't visible
    // outside the bar.
    //
    // Use a SHORT note (50 ms = 10 px wide) so the bar is narrower
    // than the ledger stub. Then the ledger overhang is visible.
    notes->clear();
    REQUIRE(notes->add(1000, 1050, 60) != 0);   // 50 ms note at C4

    image.fill(Qt::transparent);
    w.render(&image);

    // Bar centre x = (msToX(1000) + msToX(1050))/2 = (200 + 210)/2 = 205.
    // Ledger stub extends ±8 px → 197..213. Bar covers 200..210.
    // So x=215 is OUTSIDE the bar but… outside the ledger too.
    // Adjust: x=212 is at the ledger's right edge AND outside the
    // bar (which ends at 210). Probe there.
    const int outsideBarLedgerX = 212;
    // The ledger-only pixel reads as the border colour (140,200,100).
    const QColor c = image.pixelColor(outsideBarLedgerX, middleCY);
    REQUIRE(c.green() > 150);     // ledger uses the same border green
    REQUIRE(c.green() > c.red());
    REQUIRE(c.green() > c.blue());
}

TEST_CASE("StaffWidget: paints multiple notes (chord) at the same interval",
          "[staff-widget][gui][notes][chord]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);

    // Three-note chord on the same interval — C4 + E4 + G4.
    REQUIRE(notes->add(1000, 1400, 60) != 0);
    REQUIRE(notes->add(1000, 1400, 64) != 0);
    REQUIRE(notes->add(1000, 1400, 67) != 0);

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    // Each note should paint at its own y.
    const int probeX = 240;
    REQUIRE(isNoteGreen(image.pixelColor(probeX, kBottomLineY)));               // E4 line
    REQUIRE(isNoteGreen(image.pixelColor(probeX, kBottomLineY - 2 * kPxPerHalfStep)));  // G4 (2nd line)
    REQUIRE(isNoteGreen(image.pixelColor(probeX, kBottomLineY + 2 * kPxPerHalfStep)));  // C4 (1 ledger below)
}

TEST_CASE("StaffWidget: setSelectedNoteId clears other-kind selections",
          "[staff-widget][gui][notes]") {
    qtApp();
    StaffWidget w;
    const std::int64_t bars[] = { 500 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{bars}));
    const std::int64_t markers[] = { 1000 };
    auto markerModel = installMarkers(w, std::span<const std::int64_t>{markers});
    auto noteModel   = installNotes(w);
    REQUIRE(noteModel->add(2000, 2400, 64) != 0);
    const auto noteId = *noteModel->idAt(0);

    w.setSelectedBarline(0);
    REQUIRE(w.selectedBarline().has_value());
    w.setSelectedMarkerId(*markerModel->idAt(0));
    REQUIRE_FALSE(w.selectedBarline().has_value());

    QSignalSpy markerSpy(&w, &StaffWidget::markerSelectionChanged);
    QSignalSpy noteSpy  (&w, &StaffWidget::noteSelectionChanged);

    w.setSelectedNoteId(noteId);

    REQUIRE(w.selectedNoteId() == noteId);
    REQUIRE_FALSE(w.selectedMarkerId().has_value());
    REQUIRE(markerSpy.count() == 1);
    REQUIRE(noteSpy.count()   == 1);
}

TEST_CASE("StaffWidget: removing the selected note clears the selection",
          "[staff-widget][gui][notes]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);
    const auto noteId = *notes->idAt(0);
    w.setSelectedNoteId(noteId);
    REQUIRE(w.selectedNoteId() == noteId);

    QSignalSpy selSpy(&w, &StaffWidget::noteSelectionChanged);
    REQUIRE(notes->remove(noteId));
    REQUIRE_FALSE(w.selectedNoteId().has_value());
    REQUIRE(selSpy.count() == 1);
}

TEST_CASE("StaffWidget: setNoteModel(nullptr) detaches and clears selection",
          "[staff-widget][gui][notes]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);
    w.setSelectedNoteId(*notes->idAt(0));
    REQUIRE(w.selectedNoteId().has_value());

    w.setNoteModel(nullptr);
    REQUIRE_FALSE(w.selectedNoteId().has_value());
    REQUIRE(w.noteModel() == nullptr);
}

TEST_CASE("StaffWidget: accidental note paints at natural-below y with cool-tint fill",
          "[staff-widget][gui][notes][accidentals]") {
    // MEMO: load-bearing — A♯4 (midi 70) renders at A4's y (the
    // natural one semitone below), but the bar is filled with the
    // cool-shifted accidental tint instead of the warm natural
    // green. Following the piano-roll convention every surveyed
    // DAW uses (Logic, Pro Tools, Ableton, FL, Cubase, …): no
    // per-note ♯ glyph; pitch and accidental state are conveyed by
    // position + colour alone. The contiguous-rectangle layout
    // makes engraving-style glyphs impractical.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 70) != 0);   // A#4 — accidental

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    // A4's y = bottomY - 3 * kPxPerHalfStep (A4 is 3 dia steps
    // above E4 — line E4, space F4, line G4, space A4).
    const int a4Y = kBottomLineY - 3 * kPxPerHalfStep;
    const QColor barPixel = image.pixelColor(240, a4Y);
    // Accidental fill is (140,200,195,200) alpha-blended over the
    // dark background (20,20,24). The blue channel ends up
    // comparable to the green channel (cool tint), unlike naturals
    // where green dominates strongly over blue. Pin the inequality
    // that distinguishes the two tints.
    REQUIRE(barPixel.green() > 100);                  // bar is painted (not bg)
    REQUIRE(barPixel.blue()  > barPixel.red() + 30);  // blue-shifted from natural
}

TEST_CASE("StaffWidget: natural and accidental at same y use distinguishable colors",
          "[staff-widget][gui][notes][accidentals]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 69) != 0);   // A4 (natural)
    REQUIRE(notes->add(2000, 2400, 70) != 0);   // A#4 (accidental — same y)

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    const int a4Y = kBottomLineY - 3 * kPxPerHalfStep;
    // A4 spans 1000-1400 ms → ~200-280 px. Probe x=240 (inside
    // first bar).
    const QColor naturalPx = image.pixelColor(240, a4Y);
    // A#4 spans 2000-2400 ms → ~400-480 px. Probe x=440.
    const QColor accidentalPx = image.pixelColor(440, a4Y);

    // Both pixels painted (not background).
    REQUIRE(naturalPx.green()    > 100);
    REQUIRE(accidentalPx.green() > 100);
    // They are clearly different in blue channel — the
    // accidental's blue is higher.
    REQUIRE(accidentalPx.blue() > naturalPx.blue() + 30);
}

TEST_CASE("StaffWidget: empty-space click clears any active note selection",
          "[staff-widget][gui][notes]") {
    // MEMO: load-bearing rule — a click on the waveform / staff at a
    // location that doesn't hit any artifact (the "plain seek" path)
    // clears the current note selection. Mirrors the DAW convention
    // and — critically — prevents the "stack on previous" trap:
    // after editing a note's pitch, the user clicks the waveform to
    // browse, and the dock's note property page closes so the next
    // Add Note is unambiguous. See onMousePressEvent's empty-space
    // branch.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);
    const auto id = *notes->idAt(0);
    w.setSelectedNoteId(id);
    REQUIRE(w.selectedNoteId() == id);

    QSignalSpy selSpy(&w, &StaffWidget::noteSelectionChanged);
    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);

    // Click far from the note (x=600 ≈ 3000 ms) — no barline / marker /
    // loop hit either, so the plain-seek branch runs.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(600, 40));

    REQUIRE_FALSE(w.selectedNoteId().has_value());
    REQUIRE(selSpy.count() == 1);
    REQUIRE(seekSpy.count() == 1);
}
