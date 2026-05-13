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

// ---- piano-roll geometry mirrors (step 6.2) ---------------------
// Mirror of the constants in StaffWidget.cpp. The y axis is chromatic
// (E7 at top, G3 at bottom) and the leftmost kKeyboardWidth pixels
// are reserved for the piano keyboard column — xToMs / msToX in the
// base offset the time axis past that. Used across the barline /
// marker / loop / viewport / note tests below so click positions land
// on the actual artifact x and not in the keyboard column.
constexpr int kKeyboardWidth      = 56;
constexpr int kGridTopMargin      = 18;
constexpr int kSemitoneRowHeight  = 8;
constexpr int kRowMidiHigh        = 100;   // E7 — top of range
constexpr int kRowMidiLow         = 55;    // G3 — bottom of range
constexpr int kRowCount           = kRowMidiHigh - kRowMidiLow + 1;
constexpr int kGridHeightPx       = kRowCount * kSemitoneRowHeight;
constexpr int kGridBottomMargin   = 18;
constexpr int kStaffHeightPx      =
    kGridTopMargin + kGridHeightPx + kGridBottomMargin;     // 404

// Row CENTER y for the given midi value.
constexpr int rowYForMidiTest(int midi) {
    const int rowIndex = kRowMidiHigh - midi;
    return kGridTopMargin
         + rowIndex * kSemitoneRowHeight
         + kSemitoneRowHeight / 2;
}
// Pixel x corresponding to the given source-ms, honouring the
// keyboard-column left margin.
constexpr int xForMsTest(int ms, int widthPx = 800, int durationMs = 4000) {
    const int gridW = widthPx - kKeyboardWidth;
    return kKeyboardWidth + ms * gridW / durationMs;
}

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
//   - resized so the chromatic grid fits at its natural height —
//     the piano-roll layout is taller than the old diatonic staff
void setUpWidget(StaffWidget&                   w,
                 std::shared_ptr<BarlineModel>  model,
                 std::int64_t                   durationMs = 4000,
                 int                            widthPx    = 800,
                 int                            heightPx   = kStaffHeightPx)
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

TEST_CASE("StaffWidget: xToMs / msToX round-trip across the grid",
          "[staff-widget][gui][coords]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/10'000,
                /*widthPx=*/800, /*heightPx=*/420);

    // Iterate over the grid x range — the keyboard column [0, kKbWidth)
    // is reserved (xToMs returns 0 there, msToX never produces those x).
    for (int x = kKeyboardWidth + 1; x < 800; x += 37) {
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
    REQUIRE(w.msToX(-500)   <  kKeyboardWidth);
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
    // 4 s file, 800 px wide; the time axis starts after the keyboard
    // column so a barline at 1000 ms maps to x = xForMsTest(1000).
    const std::int64_t stamps[] = { 1000 };
    setUpWidget(w, makeModel(std::span<const std::int64_t>{stamps}));

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    QSignalSpy selSpy (&w, &StaffWidget::barlineSelectionChanged);

    // Click 2 px right of the tick — within the 5 px hit tolerance.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(1000) + 2, 50));

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

    // Click at x for 3000 ms — far from the only barline at 1000 ms.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(3000), 50));

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
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(1000), 50));
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy selSpy(&w, &StaffWidget::barlineSelectionChanged);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(3000), 50));

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

    const QPoint barX{xForMsTest(1000), 50};
    QTest::mouseClick(&w, Qt::RightButton,  Qt::NoModifier, barX);
    QTest::mouseClick(&w, Qt::MiddleButton, Qt::NoModifier, barX);

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

    // Pre-select the second barline at 1500 ms.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(1500), 50));
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
    // 4 s file across 800 px — marker at 1000 ms is at xForMsTest(1000).
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel());
    const std::int64_t stamps[] = { 1000 };
    auto model = installMarkers(w, std::span<const std::int64_t>{stamps});

    QSignalSpy seekSpy(&w, &StaffWidget::seekRequested);
    QSignalSpy selSpy (&w, &StaffWidget::markerSelectionChanged);

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(1000) + 2, 40));

    REQUIRE(w.selectedMarkerId() == *model->idAt(0));
    REQUIRE(seekSpy.count() == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 1000);
    REQUIRE(selSpy.count() == 1);
}

TEST_CASE("StaffWidget: marker click clears barline selection (mutual exclusion)",
          "[staff-widget][gui][markers]") {
    qtApp();
    StaffWidget w;
    const std::int64_t bars[] = { 500 };
    auto barModel = makeModel(std::span<const std::int64_t>{bars});
    setUpWidget(w, barModel);
    const std::int64_t markers[] = { 2000 };
    installMarkers(w, std::span<const std::int64_t>{markers});

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(500) + 2, 40));
    REQUIRE(w.selectedBarline() == 0);

    QSignalSpy barSelSpy(&w, &StaffWidget::barlineSelectionChanged);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(2000) + 2, 40));

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

    // 4-second file, 800-pixel widget: time axis starts after the
    // keyboard column; xForMsTest maps ms → widget-x.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(1000), 50));
    REQUIRE(w.primaryAnchorMs() == 1000);
    REQUIRE_FALSE(w.secondaryAnchorMs().has_value());

    QTest::mouseClick(&w, Qt::LeftButton, Qt::ControlModifier,
                      QPoint(xForMsTest(3000), 50));
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
    // ±6 ms slack: the chromatic grid loses ~56 px to the keyboard
    // column, so 1 px ≈ 5.37 ms (vs. 5 ms on the keyboard-less
    // WaveformWidget). A single pixel of integer-truncation slop in
    // msToX → xToMs round-trip can land just past 5 ms here.
    REQUIRE(std::abs(loops->loops()[0].startMs - 1300) <= 6);
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

    // 4000 ms across (800 - keyboard) px. xForMsTest mirrors the math.
    REQUIRE(w.xToMs(kKeyboardWidth)       == 0);
    REQUIRE(w.xToMs(xForMsTest(2000))     == 2000);
    REQUIRE(w.msToX(2000)                 == xForMsTest(2000));
}

TEST_CASE("StaffWidget: setting a viewport maps that range to the full width",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);

    QSignalSpy vpSpy(&w, &StaffWidget::viewportChanged);
    w.setViewport(1000, 2000);     // 1-second window across the grid

    REQUIRE(vpSpy.count() == 1);
    REQUIRE(w.isZoomed());
    REQUIRE(w.viewportStartMs() == 1000);
    REQUIRE(w.viewportEndMs()   == 2000);

    // 1000 ms across (800 - kKeyboardWidth = 744) px ≈ 1.34 ms/px.
    // x = kKeyboardWidth corresponds to viewportStart; halfway across
    // the grid lands at viewport mid (~1500 ms).
    const int gridMidX = kKeyboardWidth + (800 - kKeyboardWidth) / 2;
    REQUIRE(w.xToMs(kKeyboardWidth) == 1000);
    REQUIRE(std::abs(static_cast<int>(w.xToMs(gridMidX)) - 1500) <= 2);
    REQUIRE(std::abs(w.msToX(1500) - gridMidX) <= 2);
    // ms == viewportEnd lands at the exact right edge — nudged to
    // width-1 so the right-edge column still paints. Anything
    // strictly outside the viewport is returned unclamped so paint
    // code can cull (#49 follow-up).
    REQUIRE(w.msToX(2000) == 799);
    REQUIRE(w.msToX(0)    <  kKeyboardWidth); // off-screen left
    REQUIRE(w.msToX(3000) >= 800);            // off-screen right
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
    // Back to full-range math — x at grid midpoint maps to ms 2000.
    REQUIRE(w.xToMs(xForMsTest(2000)) == 2000);
}

TEST_CASE("StaffWidget: zoomBy preserves the anchor's pixel position",
          "[staff-widget][gui][coords][zoom]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/10000, /*widthPx=*/1000);

    // Pre-zoom, ms=2500 sits at xForMsTest(2500, 1000, 10000).
    const std::int64_t anchorMs = 2500;
    const int          anchorPx = w.msToX(anchorMs);
    REQUIRE(anchorPx == xForMsTest(2500, /*widthPx=*/1000,
                                         /*durationMs=*/10000));

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
// Notes (Step 6.2 — chromatic piano roll)
//
// The fixture 4 s / 800 px with a 56-px keyboard column gives a grid
// of 744 px for the 4000-ms axis, so 1 ms ≈ 0.186 px. A 400 ms note
// at 1000 ms spans pixels [xForMsTest(1000), xForMsTest(1400)). Pitch
// goes to its own chromatic row — `rowYForMidiTest(midi)` returns
// the row's centre y.
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<NoteModel> installNotes(StaffWidget& w) {
    auto model = std::make_shared<NoteModel>();
    w.setNoteModel(model);
    return model;
}

// Note fill is (180, 220, 140, ~215) alpha-blended over the
// white-key row background (34, 34, 38). The composite stays
// clearly green-dominant.
bool isNoteGreen(QColor c) {
    return c.green() > c.red() && c.green() > c.blue()
        && c.green() > 100;
}

// Centre x of the bar [startMs, endMs) under the test fixture.
constexpr int barMidXTest(int startMs, int endMs) {
    return (xForMsTest(startMs) + xForMsTest(endMs)) / 2;
}

} // namespace

TEST_CASE("StaffWidget: paints note bar on its chromatic row for E4",
          "[staff-widget][gui][notes]") {
    // MEMO: load-bearing — chromatic Y geometry. Every note bar is
    // centred on `rowYForMidi(midi)`; if the formula drifts every
    // painted note moves.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);   // E4

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    const int probeX = barMidXTest(1000, 1400);
    const int rowY   = rowYForMidiTest(64);
    REQUIRE(isNoteGreen(image.pixelColor(probeX, rowY)));

    // A row 6 semitones higher / lower has the row tint only — no
    // bar paint — so it should NOT read as note-green.
    REQUIRE_FALSE(isNoteGreen(image.pixelColor(probeX,
                                               rowYForMidiTest(64 + 6))));
    REQUIRE_FALSE(isNoteGreen(image.pixelColor(probeX,
                                               rowYForMidiTest(64 - 6))));
}

TEST_CASE("StaffWidget: A4 paints on its own row above E4",
          "[staff-widget][gui][notes]") {
    // A4 (midi 69) sits 5 semitones above E4 (midi 64) — five rows
    // higher on the piano roll (i.e. five rowHeights toward the
    // top of the widget).
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 69) != 0);   // A4

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    REQUIRE(isNoteGreen(image.pixelColor(barMidXTest(1000, 1400),
                                         rowYForMidiTest(69))));
}

TEST_CASE("StaffWidget: paints multiple notes (chord) at the same interval",
          "[staff-widget][gui][notes][chord]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);

    // Three-note chord on the same interval — C4 + E4 + G4. Each
    // sits on its own chromatic row.
    REQUIRE(notes->add(1000, 1400, 60) != 0);
    REQUIRE(notes->add(1000, 1400, 64) != 0);
    REQUIRE(notes->add(1000, 1400, 67) != 0);

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    const int probeX = barMidXTest(1000, 1400);
    REQUIRE(isNoteGreen(image.pixelColor(probeX, rowYForMidiTest(60))));
    REQUIRE(isNoteGreen(image.pixelColor(probeX, rowYForMidiTest(64))));
    REQUIRE(isNoteGreen(image.pixelColor(probeX, rowYForMidiTest(67))));
}

TEST_CASE("StaffWidget: accidental note (A#4) lands on its own chromatic row",
          "[staff-widget][gui][notes][piano-roll]") {
    // MEMO[#step6.2]: the diatonic 6.1 staff fused A#4 onto A4's y
    // and distinguished them with a tint. In the piano-roll layout
    // every accidental has its own row — A#4 (midi 70) sits one row
    // ABOVE A4 (midi 69), not the same row. Pin that.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 69) != 0);   // A4 (natural)
    REQUIRE(notes->add(2000, 2400, 70) != 0);   // A#4 (accidental)

    QImage image(w.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    w.render(&image);

    const int a4Y   = rowYForMidiTest(69);
    const int as4Y  = rowYForMidiTest(70);
    REQUIRE(as4Y != a4Y);
    REQUIRE(isNoteGreen(image.pixelColor(barMidXTest(1000, 1400), a4Y)));
    REQUIRE(isNoteGreen(image.pixelColor(barMidXTest(2000, 2400), as4Y)));
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

TEST_CASE("StaffWidget: empty-space click clears any active note selection "
          "but ALSO triggers note placement on the clicked row",
          "[staff-widget][gui][notes][piano-roll]") {
    // MEMO[#step6.2]: a click on an empty cell of the piano-roll
    // grid is BOTH a plain-seek (which clears any prior note
    // selection — same "stack on previous" trap rule as 6.1) AND a
    // note-placement gesture. The two effects compose: the prior
    // selection is cleared, the seek fires, and the staff emits
    // `placeNoteRequested(ms, midi)` for MainWindow to add a new
    // note. The new note's selection is set by MainWindow, so the
    // widget-level `selectedNoteId()` stays empty in this isolated
    // test.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    // Add a note WAY ABOVE A4 so the click target row sits in empty
    // grid (no artifact overlap, no note hit).
    REQUIRE(notes->add(1000, 1400, 88) != 0);
    const auto id = *notes->idAt(0);
    w.setSelectedNoteId(id);
    REQUIRE(w.selectedNoteId() == id);

    QSignalSpy selSpy  (&w, &StaffWidget::noteSelectionChanged);
    QSignalSpy seekSpy (&w, &StaffWidget::seekRequested);
    QSignalSpy placeSpy(&w, &StaffWidget::placeNoteRequested);

    // Click on the A4 (midi 69) row at ~3000 ms — far from any
    // barline / marker / loop / existing note.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(3000), rowYForMidiTest(69)));

    REQUIRE_FALSE(w.selectedNoteId().has_value());
    REQUIRE(selSpy.count()   == 1);
    REQUIRE(seekSpy.count()  == 1);
    REQUIRE(placeSpy.count() == 1);
    REQUIRE(placeSpy.takeFirst().at(1).toInt() == 69);
}

// ---------------------------------------------------------------------------
// Piano-roll geometry + placement gesture (Step 6.2)
// ---------------------------------------------------------------------------

TEST_CASE("StaffWidget: click in the keyboard column is ignored",
          "[staff-widget][gui][piano-roll]") {
    // MEMO[#step6.2]: a click anywhere inside the leftmost
    // kKeyboardWidth pixels is reserved (future preview-tone work).
    // It must NOT seek, NOT place a note, NOT change selection.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    installNotes(w);

    QSignalSpy seekSpy (&w, &StaffWidget::seekRequested);
    QSignalSpy placeSpy(&w, &StaffWidget::placeNoteRequested);

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(kKeyboardWidth / 2, rowYForMidiTest(69)));

    REQUIRE(seekSpy.count()  == 0);
    REQUIRE(placeSpy.count() == 0);
}

TEST_CASE("StaffWidget: click on the grid emits placeNoteRequested(ms, midi)",
          "[staff-widget][gui][piano-roll]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    installNotes(w);

    QSignalSpy placeSpy(&w, &StaffWidget::placeNoteRequested);

    // Click on E4's row (midi 64) at 2000 ms — no artifacts, no
    // notes, so this is a pure place-on-empty-cell case.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(2000), rowYForMidiTest(64)));

    REQUIRE(placeSpy.count() == 1);
    const auto args = placeSpy.takeFirst();
    REQUIRE(std::abs(args.at(0).toLongLong() - 2000) < 10);
    REQUIRE(args.at(1).toInt() == 64);
}

TEST_CASE("StaffWidget: click on an existing note bar selects it (does not place)",
          "[staff-widget][gui][piano-roll][notes]") {
    // MEMO[#step6.2]: a click that lands inside an existing note
    // bar's rect must SELECT that note — not fall through to the
    // empty-space placement branch. Without this guard, clicking a
    // bar would add a second overlapping note on the same row.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);   // E4
    const auto id = *notes->idAt(0);

    QSignalSpy noteSelSpy(&w, &StaffWidget::noteSelectionChanged);
    QSignalSpy placeSpy  (&w, &StaffWidget::placeNoteRequested);
    QSignalSpy seekSpy   (&w, &StaffWidget::seekRequested);

    // Click on the bar's centre — well inside the [200, 280) range,
    // on the E4 row.
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(barMidXTest(1000, 1400), rowYForMidiTest(64)));

    REQUIRE(w.selectedNoteId() == id);
    REQUIRE(noteSelSpy.count() == 1);
    REQUIRE(placeSpy.count()   == 0);     // NO new note
    REQUIRE(seekSpy.count()    == 1);
    REQUIRE(seekSpy.takeFirst().at(0).toLongLong() == 1000);
    REQUIRE(notes->size() == 1);          // still exactly one note
}

TEST_CASE("StaffWidget: click above or below the chromatic grid does not place",
          "[staff-widget][gui][piano-roll]") {
    // Top margin (tune-type banner) and bottom margin (loop labels)
    // sit outside the chromatic rows. A click there should seek
    // but NOT emit placeNoteRequested — there is no row to map.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    installNotes(w);

    QSignalSpy placeSpy(&w, &StaffWidget::placeNoteRequested);
    QSignalSpy seekSpy (&w, &StaffWidget::seekRequested);

    // Click in the top banner (y < kGridTopMargin).
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(2000), 4));
    REQUIRE(seekSpy.count()  == 1);
    REQUIRE(placeSpy.count() == 0);
}

// ---------------------------------------------------------------------------
// Note drag / resize / drag-to-create (issue #60)
// ---------------------------------------------------------------------------

namespace {

// Drive a press → move-near-press → move-to-end → release sequence
// on the staff. Mirrors the dragSequence helper above but lets the
// caller pass the y coordinate per gesture (note drags are y-bound
// to the chromatic row, not arbitrary as for markers / loops).
void noteDragSequence(StaffWidget&             w,
                      QPoint                   press,
                      QPoint                   release,
                      Qt::KeyboardModifiers    mods = Qt::NoModifier)
{
    QTest::mousePress(&w, Qt::LeftButton, mods, press);
    QMouseEvent mvNear(QEvent::MouseMove,
                       QPointF{press + QPoint{2, 0}},
                       Qt::NoButton, Qt::LeftButton, mods);
    QApplication::sendEvent(&w, &mvNear);
    QMouseEvent mvFinal(QEvent::MouseMove,
                       QPointF{release},
                       Qt::NoButton, Qt::LeftButton, mods);
    QApplication::sendEvent(&w, &mvFinal);
    QTest::mouseRelease(&w, Qt::LeftButton, mods, release);
}

} // namespace

TEST_CASE("StaffWidget: drag note body moves it in time and pitch",
          "[staff-widget][gui][notes][drag]") {
    // E4 bar at [1000..1400]. Drag the body 800 ms right and one
    // row up (E4 → F4). The commit signal carries the post-clamp
    // post-row final values; the model is untouched here — tests
    // pin the SIGNAL contract, MainWindow tests pin the model
    // write.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);
    const auto id = *notes->idAt(0);

    QSignalSpy spy(&w, &StaffWidget::noteDragCommitted);

    const QPoint press  (barMidXTest(1000, 1400), rowYForMidiTest(64));
    const QPoint release(barMidXTest(1800, 2200), rowYForMidiTest(65));
    noteDragSequence(w, press, release);

    REQUIRE(spy.count() == 1);
    const auto args = spy.takeFirst();
    REQUIRE(args.at(0).toLongLong() == id);
    REQUIRE(args.at(1).toLongLong() == 1000);   // from start
    REQUIRE(args.at(2).toLongLong() == 1400);   // from end
    REQUIRE(args.at(3).toInt()       == 64);    // from midi
    REQUIRE(std::abs(args.at(4).toLongLong() - 1800) <= 6);  // to start
    REQUIRE(std::abs(args.at(5).toLongLong() - 2200) <= 6);  // to end
    REQUIRE(args.at(6).toInt()       == 65);    // to midi (F4)
}

TEST_CASE("StaffWidget: Shift+drag locks the note to its original row",
          "[staff-widget][gui][notes][drag]") {
    // Same setup but Shift held → vertical movement ignored.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);

    QSignalSpy spy(&w, &StaffWidget::noteDragCommitted);

    const QPoint press  (barMidXTest(1000, 1400), rowYForMidiTest(64));
    const QPoint release(barMidXTest(1800, 2200), rowYForMidiTest(70));
    noteDragSequence(w, press, release, Qt::ShiftModifier);

    REQUIRE(spy.count() == 1);
    const auto args = spy.takeFirst();
    REQUIRE(args.at(6).toInt() == 64);   // midi unchanged
}

TEST_CASE("StaffWidget: drag the right edge resizes endMs only",
          "[staff-widget][gui][notes][drag]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);

    QSignalSpy spy(&w, &StaffWidget::noteDragCommitted);

    const QPoint press  (xForMsTest(1400), rowYForMidiTest(64));
    const QPoint release(xForMsTest(1800), rowYForMidiTest(64));
    noteDragSequence(w, press, release);

    REQUIRE(spy.count() == 1);
    const auto args = spy.takeFirst();
    REQUIRE(args.at(1).toLongLong() == 1000);
    REQUIRE(args.at(2).toLongLong() == 1400);
    REQUIRE(std::abs(args.at(4).toLongLong() - 1000) <= 1);  // start untouched
    REQUIRE(std::abs(args.at(5).toLongLong() - 1800) <= 6);  // end → 1800
    REQUIRE(args.at(6).toInt() == 64);
}

TEST_CASE("StaffWidget: drag the left edge resizes startMs only",
          "[staff-widget][gui][notes][drag]") {
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(1000, 1400, 64) != 0);

    QSignalSpy spy(&w, &StaffWidget::noteDragCommitted);

    // Press on the start edge; drag right by 200 ms.
    const QPoint press  (xForMsTest(1000), rowYForMidiTest(64));
    const QPoint release(xForMsTest(1200), rowYForMidiTest(64));
    noteDragSequence(w, press, release);

    REQUIRE(spy.count() == 1);
    const auto args = spy.takeFirst();
    REQUIRE(args.at(1).toLongLong() == 1000);
    REQUIRE(args.at(2).toLongLong() == 1400);
    REQUIRE(std::abs(args.at(4).toLongLong() - 1200) <= 6);  // start → 1200
    REQUIRE(args.at(5).toLongLong() == 1400);                // end untouched
}

TEST_CASE("StaffWidget: drag on empty grid creates a note of the dragged length",
          "[staff-widget][gui][notes][drag][piano-roll]") {
    // Empty grid → drag-to-create. Signal carries press-ms / release-ms
    // and the press-row's midi. MainWindow turns it into a model add.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    installNotes(w);

    QSignalSpy createSpy(&w, &StaffWidget::noteCreateCommitted);
    QSignalSpy placeSpy (&w, &StaffWidget::placeNoteRequested);

    const QPoint press  (xForMsTest(2000), rowYForMidiTest(72));
    const QPoint release(xForMsTest(2600), rowYForMidiTest(72));
    noteDragSequence(w, press, release);

    REQUIRE(createSpy.count() == 1);
    REQUIRE(placeSpy.count()  == 0);   // drag overrides click-to-place
    const auto args = createSpy.takeFirst();
    REQUIRE(std::abs(args.at(0).toLongLong() - 2000) <= 6);
    REQUIRE(std::abs(args.at(1).toLongLong() - 2600) <= 6);
    REQUIRE(args.at(2).toInt() == 72);
}

TEST_CASE("StaffWidget: plain click on empty grid still emits placeNoteRequested "
          "(no drag-create)",
          "[staff-widget][gui][notes][piano-roll]") {
    // Regression check for the press → release deferral added with
    // drag-to-create. A press + release at the same point (no drag
    // past threshold) must still fire placeNoteRequested — the
    // default-span placement path is preserved.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    installNotes(w);

    QSignalSpy createSpy(&w, &StaffWidget::noteCreateCommitted);
    QSignalSpy placeSpy (&w, &StaffWidget::placeNoteRequested);

    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xForMsTest(2000), rowYForMidiTest(72)));

    REQUIRE(placeSpy.count()  == 1);
    REQUIRE(createSpy.count() == 0);
}

TEST_CASE("StaffWidget: resize past partner edge clamps to partner − 1",
          "[staff-widget][gui][notes][drag]") {
    // End-edge drag past startMs (going leftward) must NOT collapse
    // the note to zero or negative duration. The ghost (and the
    // committed value) clamps to startMs + 1 instead.
    qtApp();
    StaffWidget w;
    setUpWidget(w, makeModel(), /*durationMs=*/4000, /*widthPx=*/800);
    auto notes = installNotes(w);
    REQUIRE(notes->add(2000, 2400, 64) != 0);

    QSignalSpy spy(&w, &StaffWidget::noteDragCommitted);

    const QPoint press  (xForMsTest(2400), rowYForMidiTest(64));
    const QPoint release(xForMsTest(1500), rowYForMidiTest(64));  // way past start
    noteDragSequence(w, press, release);

    REQUIRE(spy.count() == 1);
    const auto args = spy.takeFirst();
    REQUIRE(args.at(4).toLongLong() == 2000);                 // start untouched
    REQUIRE(args.at(5).toLongLong() == 2001);                 // end clamped
}
