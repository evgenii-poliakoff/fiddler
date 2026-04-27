// Tests for score::BarlineModel. Pure data layer — no widgets — but
// the model is a QObject (so its `changed()` signal fits the rest of
// the Qt UI), which means the tests need a QApplication. We piggyback
// on the existing qt_test_app.cpp bootstrap.

#include "qt_test_app.h"
#include "score/BarlineModel.h"

#include <QSignalSpy>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using fiddler::score::BarlineModel;
using fiddler::score::TimeSignature;
using fiddler::test::qtApp;

namespace {

std::vector<std::int64_t> toVec(std::span<const std::int64_t> s) {
    return { s.begin(), s.end() };
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / time signature
// ---------------------------------------------------------------------------

TEST_CASE("BarlineModel: default-constructed is empty with 4/4 time-sig",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    REQUIRE(m.empty());
    REQUIRE(m.size() == 0);
    REQUIRE(m.barlines().empty());
    REQUIRE(m.timeSignature().numerator   == 4);
    REQUIRE(m.timeSignature().denominator == 4);
    REQUIRE(m.timeSignature().tuneType.isEmpty());
}

TEST_CASE("BarlineModel: setTimeSignature emits changed once on real change",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    QSignalSpy spy(&m, &BarlineModel::changed);

    m.setTimeSignature({6, 8, "Jig"});
    REQUIRE(spy.count() == 1);
    REQUIRE(m.timeSignature().numerator   == 6);
    REQUIRE(m.timeSignature().denominator == 8);
    REQUIRE(m.timeSignature().tuneType    == "Jig");

    // Re-applying the same value must not emit again.
    m.setTimeSignature({6, 8, "Jig"});
    REQUIRE(spy.count() == 1);

    // Even the tuneType label is part of identity.
    m.setTimeSignature({6, 8, "Slip Jig"});
    REQUIRE(spy.count() == 2);
}

// ---------------------------------------------------------------------------
// add / sorted invariant / duplicates
// ---------------------------------------------------------------------------

TEST_CASE("BarlineModel: add inserts in sorted order regardless of insert sequence",
          "[barline-model]") {
    qtApp();
    BarlineModel m;

    m.add(2000);
    m.add(500);
    m.add(1500);
    m.add(3000);

    REQUIRE(toVec(m.barlines()) ==
            std::vector<std::int64_t>{500, 1500, 2000, 3000});
}

TEST_CASE("BarlineModel: add returns the inserted index in sorted order",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    REQUIRE(m.add(1000) == 0);
    REQUIRE(m.add(500)  == 0);    // prepended
    REQUIRE(m.add(2000) == 2);    // appended
    REQUIRE(m.add(1200) == 2);    // middle
}

TEST_CASE("BarlineModel: add at an existing ms is rejected silently",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    QSignalSpy spy(&m, &BarlineModel::changed);

    REQUIRE(m.add(1000) == 0);
    REQUIRE(spy.count() == 1);
    REQUIRE(m.size() == 1);

    // Duplicate add: returns the existing index, no insert, no emit.
    const auto dup = m.add(1000);
    REQUIRE(dup.has_value());
    REQUIRE(*dup == 0);
    REQUIRE(m.size() == 1);
    REQUIRE(spy.count() == 1);
}

TEST_CASE("BarlineModel: add emits changed exactly once per real insert",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    QSignalSpy spy(&m, &BarlineModel::changed);
    m.add(100);
    m.add(200);
    m.add(150);
    REQUIRE(spy.count() == 3);
}

// ---------------------------------------------------------------------------
// removeAt
// ---------------------------------------------------------------------------

TEST_CASE("BarlineModel: removeAt erases the right entry",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    m.add(100);
    m.add(200);
    m.add(300);
    QSignalSpy spy(&m, &BarlineModel::changed);

    m.removeAt(1);
    REQUIRE(toVec(m.barlines()) == std::vector<std::int64_t>{100, 300});
    REQUIRE(spy.count() == 1);
}

TEST_CASE("BarlineModel: removeAt out-of-range is a silent no-op",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    m.add(100);
    QSignalSpy spy(&m, &BarlineModel::changed);

    m.removeAt(99);
    m.removeAt(static_cast<std::size_t>(-1));   // wraps to huge unsigned
    REQUIRE(m.size() == 1);
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------

TEST_CASE("BarlineModel: clear empties barlines, preserves time signature",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    m.setTimeSignature({6, 8, "Jig"});
    m.add(100);
    m.add(200);
    QSignalSpy spy(&m, &BarlineModel::changed);

    m.clear();
    REQUIRE(m.empty());
    REQUIRE(spy.count() == 1);
    // Time signature survives clear.
    REQUIRE(m.timeSignature().tuneType == "Jig");

    // Clearing an already-empty model is a no-op (no second emit).
    m.clear();
    REQUIRE(spy.count() == 1);
}

// ---------------------------------------------------------------------------
// undoLastAdd — LIFO, not sorted-last
// ---------------------------------------------------------------------------

TEST_CASE("BarlineModel: undoLastAdd removes the most-recently-added entry",
          "[barline-model]") {
    qtApp();
    BarlineModel m;

    // Add in non-monotonic order; undo should peel from the end of the
    // *placement* history, not the end of the sorted vector.
    m.add(2000);
    m.add(500);     // sorted-prepend, but most-recently added
    m.add(1500);

    REQUIRE(toVec(m.barlines()) == std::vector<std::int64_t>{500, 1500, 2000});

    REQUIRE(m.undoLastAdd());                            // removes 1500
    REQUIRE(toVec(m.barlines()) == std::vector<std::int64_t>{500, 2000});

    REQUIRE(m.undoLastAdd());                            // removes 500
    REQUIRE(toVec(m.barlines()) == std::vector<std::int64_t>{2000});

    REQUIRE(m.undoLastAdd());                            // removes 2000
    REQUIRE(m.empty());

    // Stack drained — no further undo possible.
    REQUIRE_FALSE(m.undoLastAdd());
}

TEST_CASE("BarlineModel: undoLastAdd on empty returns false, no emit",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    QSignalSpy spy(&m, &BarlineModel::changed);
    REQUIRE_FALSE(m.undoLastAdd());
    REQUIRE(spy.count() == 0);
}

TEST_CASE("BarlineModel: removeAt also drops entry from add-history",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    m.add(1000);
    m.add(2000);
    m.add(3000);

    // Remove the most-recently-added by index. Subsequent undo should
    // now peel the *previous* most-recent (2000), not 3000 again.
    m.removeAt(2);                                       // erases 3000
    REQUIRE(toVec(m.barlines()) == std::vector<std::int64_t>{1000, 2000});

    REQUIRE(m.undoLastAdd());                            // peels 2000
    REQUIRE(toVec(m.barlines()) == std::vector<std::int64_t>{1000});

    REQUIRE(m.undoLastAdd());                            // peels 1000
    REQUIRE(m.empty());

    REQUIRE_FALSE(m.undoLastAdd());
}

TEST_CASE("BarlineModel: clear also drains add-history",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    m.add(100);
    m.add(200);
    m.clear();
    REQUIRE_FALSE(m.undoLastAdd());
}

// ---------------------------------------------------------------------------
// nearest — used by click-to-select hit-testing
// ---------------------------------------------------------------------------

TEST_CASE("BarlineModel: nearest returns nullopt when empty",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    REQUIRE_FALSE(m.nearest(1000, 100).has_value());
}

TEST_CASE("BarlineModel: nearest finds the closest within tolerance",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    m.add(1000);
    m.add(2000);
    m.add(5000);

    REQUIRE(m.nearest(1000, 50)  == 0);                  // exact
    REQUIRE(m.nearest(1040, 50)  == 0);                  // within tol of 1000
    REQUIRE(m.nearest(1960, 50)  == 1);                  // within tol of 2000
    REQUIRE(m.nearest(3500, 100) == std::nullopt);       // outside tol of any
    // 3500 is exactly equidistant from 2000 and 5000 — tie-break
    // picks the earlier one.
    REQUIRE(m.nearest(3500, 1500) == 1);
    // 3501 is strictly closer to 5000 (dist 1499) than to 2000 (1501).
    REQUIRE(m.nearest(3501, 1500) == 2);
}

TEST_CASE("BarlineModel: nearest with negative tolerance returns nullopt",
          "[barline-model]") {
    qtApp();
    BarlineModel m;
    m.add(1000);
    REQUIRE_FALSE(m.nearest(1000, -1).has_value());
}
