// Tests for score::MarkerModel.
//
// MEMO[refactor]: each TEST_CASE pins one behavioural rule with a
// short comment naming the rule. When refactoring the model, read
// the comment first to know what's load-bearing — most of the
// specific values (1000, "Mark 1", etc.) are illustrative; the
// *property* the assertion expresses is what the test is here to
// guard. Comments call out which assertions are property-bearing
// and which are incidental.
//
// MEMO: model is a QObject (so it fits Qt's signal/slot wiring), so
// these tests need a QApplication. We piggyback on qt_test_app.

#include "qt_test_app.h"
#include "score/MarkerModel.h"

#include <QSignalSpy>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using fiddler::score::Marker;
using fiddler::score::MarkerModel;
using fiddler::test::qtApp;

namespace {

// Helper: extract just the source-ms values in current sort order.
// Used by many tests to assert sort-invariant behaviour without
// caring about IDs or names.
std::vector<std::int64_t>
positions(std::span<const Marker> markers) {
    std::vector<std::int64_t> out;
    out.reserve(markers.size());
    for (const auto& m : markers) out.push_back(m.sourceMs);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction + auto-naming
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: default-constructed is empty",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    REQUIRE(m.empty());
    REQUIRE(m.size() == 0);
    REQUIRE(m.markers().empty());
}

TEST_CASE("MarkerModel: add returns a non-zero stable ID",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(2000);

    // MEMO: IDs are part of the public contract — widgets store
    // selection by ID across re-sorts. Zero is reserved as "no ID",
    // and IDs must be unique within a session.
    REQUIRE(id1 != 0);
    REQUIRE(id2 != 0);
    REQUIRE(id1 != id2);
}

TEST_CASE("MarkerModel: empty name auto-names 'Mark 1', 'Mark 2'",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(1000);
    m.add(2000);

    REQUIRE(m.markers()[0].name == "Mark 1");
    REQUIRE(m.markers()[1].name == "Mark 2");
}

TEST_CASE("MarkerModel: user-provided name is preserved",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(1000, "Hard bit");
    REQUIRE(m.markers()[0].name == "Hard bit");
}

TEST_CASE("MarkerModel: auto-name counter doesn't reuse numbers after remove",
          "[marker-model]") {
    // MEMO: load-bearing rule — auto-naming is monotonic. If a user
    // taps M three times (Mark 1, 2, 3) then deletes Mark 2, the
    // next tap should be Mark 4, NOT Mark 2 (which would create a
    // duplicate-name surprise alongside the still-existing user
    // renames). The test pins this property.
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(2000);
    const auto id3 = m.add(3000);
    REQUIRE(m.markers()[0].name == "Mark 1");
    REQUIRE(m.markers()[1].name == "Mark 2");
    REQUIRE(m.markers()[2].name == "Mark 3");

    REQUIRE(m.remove(id2));
    m.add(4000);

    // Should be "Mark 4", not "Mark 2".
    const auto names = std::vector<QString>{
        m.markers()[0].name, m.markers()[1].name, m.markers()[2].name
    };
    REQUIRE(names == std::vector<QString>{"Mark 1", "Mark 3", "Mark 4"});

    (void)id1;
    (void)id3;
}

// ---------------------------------------------------------------------------
// Sort invariant
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: insertion sort is by sourceMs ascending",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(2000);
    m.add(500);
    m.add(1500);
    m.add(3000);

    REQUIRE(positions(m.markers())
            == std::vector<std::int64_t>{500, 1500, 2000, 3000});
}

TEST_CASE("MarkerModel: ties on sourceMs break by ID (older first)",
          "[marker-model]") {
    // MEMO: load-bearing rule — markers at the same ms must be
    // deterministic so paint order, indexOf(), and idAt() all
    // behave predictably. The "older first" rule keeps newly-placed
    // duplicates after their predecessors, which matches the visual
    // expectation if a user accidentally double-taps.
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(1000);
    REQUIRE(id1 < id2);

    REQUIRE(m.markers()[0].id == id1);
    REQUIRE(m.markers()[1].id == id2);
}

// ---------------------------------------------------------------------------
// rename
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: rename by valid ID updates name and emits changed",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id = m.add(1000);
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE(m.rename(id, "First downbeat"));
    REQUIRE(m.markers()[0].name == "First downbeat");
    REQUIRE(spy.count() == 1);
}

TEST_CASE("MarkerModel: rename to the existing name is a quiet no-op",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id = m.add(1000);
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE(m.rename(id, "Mark 1"));      // same as auto-name
    REQUIRE(spy.count() == 0);             // no emit for no-op
}

TEST_CASE("MarkerModel: rename of unknown ID returns false, no emit",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE_FALSE(m.rename(999, "anywhere"));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// setPosition (the API that makes the *stable IDs* concept earn its keep)
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: setPosition without reorder updates ms and emits once",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id  = m.add(1000);
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE(m.setPosition(id, 1100));      // still first in sort
    REQUIRE(m.markers()[0].sourceMs == 1100);
    REQUIRE(spy.count() == 1);
}

TEST_CASE("MarkerModel: setPosition that crosses a neighbour re-sorts",
          "[marker-model]") {
    // MEMO: load-bearing — this is exactly why markers carry IDs.
    // After this move, the marker that was at index 0 is now at
    // index 1; a widget that stored selection-by-index would now
    // be highlighting the wrong marker. ID-based selection survives.
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(2000);

    REQUIRE(m.markers()[0].id == id1);
    REQUIRE(m.markers()[1].id == id2);

    REQUIRE(m.setPosition(id1, 3000));     // move first past second

    REQUIRE(m.markers()[0].id == id2);     // order swapped
    REQUIRE(m.markers()[1].id == id1);
    REQUIRE(positions(m.markers())
            == std::vector<std::int64_t>{2000, 3000});
}

TEST_CASE("MarkerModel: setPosition to the same ms is a quiet no-op",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id = m.add(1000);
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE(m.setPosition(id, 1000));
    REQUIRE(spy.count() == 0);
}

TEST_CASE("MarkerModel: setPosition of unknown ID returns false, no emit",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE_FALSE(m.setPosition(999, 1234));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// remove + clear
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: remove by valid ID erases the right entry",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(1000);
    const auto id2 = m.add(2000);
    m.add(3000);
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE(m.remove(id2));
    REQUIRE(positions(m.markers())
            == std::vector<std::int64_t>{1000, 3000});
    REQUIRE(spy.count() == 1);
}

TEST_CASE("MarkerModel: remove of unknown ID returns false, no emit",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(1000);
    QSignalSpy spy(&m, &MarkerModel::changed);

    REQUIRE_FALSE(m.remove(999));
    REQUIRE(spy.count() == 0);
}

TEST_CASE("MarkerModel: clear empties markers + history + counters",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(1000);
    m.add(2000);
    QSignalSpy spy(&m, &MarkerModel::changed);

    m.clear();
    REQUIRE(m.empty());
    REQUIRE(spy.count() == 1);

    // After clear the auto-name counter resets, so the next add is
    // "Mark 1" again — important for fresh-project semantics on
    // file load.
    m.add(500);
    REQUIRE(m.markers()[0].name == "Mark 1");

    // And clearing an already-empty model is a no-op (no second emit).
    m.clear();
    m.clear();
    QSignalSpy spy2(&m, &MarkerModel::changed);
    m.clear();
    REQUIRE(spy2.count() == 0);
}

// ---------------------------------------------------------------------------
// undoLastAdd — LIFO of placements
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: undoLastAdd peels in placement order",
          "[marker-model]") {
    // MEMO: this is what makes Ctrl+Z mean "remove the most-recently-
    // tapped marker", regardless of where it sorted in the markers_
    // vector. The placement-order LIFO is what gives us the right
    // semantics; sort order alone wouldn't.
    qtApp();
    MarkerModel m;

    m.add(2000);    // sorted index 0 at this point
    m.add(500);     // becomes sorted index 0; pushed last to LIFO
    m.add(1500);    // sorted index 1 now

    REQUIRE(positions(m.markers())
            == std::vector<std::int64_t>{500, 1500, 2000});

    REQUIRE(m.undoLastAdd());                // removes the 1500 marker
    REQUIRE(positions(m.markers())
            == std::vector<std::int64_t>{500, 2000});

    REQUIRE(m.undoLastAdd());                // removes the 500 marker
    REQUIRE(positions(m.markers())
            == std::vector<std::int64_t>{2000});

    REQUIRE(m.undoLastAdd());                // removes the 2000 marker
    REQUIRE(m.empty());

    REQUIRE_FALSE(m.undoLastAdd());          // empty LIFO → false
}

TEST_CASE("MarkerModel: remove of latest entry keeps undoLastAdd in sync",
          "[marker-model]") {
    // MEMO: load-bearing — undoLastAdd must not try to remove an
    // already-gone entry. remove() must clean up its corresponding
    // history entry; this test guards that invariant.
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(2000);            // referenced in a comment below
    const auto id3 = m.add(3000);

    REQUIRE(m.remove(id3));                  // most-recent gone via Del

    REQUIRE(m.undoLastAdd());                // peels id2, not id3
    REQUIRE(m.size() == 1);
    REQUIRE(m.markers()[0].id == id1);
    (void)id2;                                // narrative-only, silences -Wunused
}

TEST_CASE("MarkerModel: clear drains the placement LIFO",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(1000);
    m.add(2000);
    m.clear();
    REQUIRE_FALSE(m.undoLastAdd());
}

// ---------------------------------------------------------------------------
// nearest — used by widget click hit-testing
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: nearest returns nullopt on empty",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    REQUIRE_FALSE(m.nearest(1000, 100).has_value());
}

TEST_CASE("MarkerModel: nearest finds the closest within tolerance",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(2000);
    const auto id3 = m.add(5000);

    REQUIRE(m.nearest(1000, 50)   == id1);    // exact
    REQUIRE(m.nearest(1040, 50)   == id1);    // within tol of id1
    REQUIRE(m.nearest(1960, 50)   == id2);    // within tol of id2
    REQUIRE(m.nearest(3500, 100)  == std::nullopt);  // outside any tol
    (void)id3;
}

TEST_CASE("MarkerModel: nearest tie-break — later-placed wins",
          "[marker-model]") {
    // MEMO: this is the convention this model picked, and it differs
    // from BarlineModel's earlier-wins. Both are internally
    // consistent. If both behaviours need to be the same someday,
    // change the iteration / comparator in *both* models in
    // lockstep.
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(3000);

    // sourceMs=2000 is exactly equidistant from 1000 and 3000.
    REQUIRE(m.nearest(2000, 1500) == id2);   // later-placed wins
    (void)id1;
}

TEST_CASE("MarkerModel: nearest with negative tolerance returns nullopt",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    m.add(1000);
    REQUIRE_FALSE(m.nearest(1000, -1).has_value());
}

// ---------------------------------------------------------------------------
// indexOf / idAt — index ↔ ID translation
// ---------------------------------------------------------------------------

TEST_CASE("MarkerModel: indexOf maps ID to current sorted index",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(2000);
    const auto id2 = m.add(1000);   // becomes index 0

    REQUIRE(m.indexOf(id2) == 0);
    REQUIRE(m.indexOf(id1) == 1);
    REQUIRE_FALSE(m.indexOf(999).has_value());
}

TEST_CASE("MarkerModel: idAt is the inverse of indexOf",
          "[marker-model]") {
    qtApp();
    MarkerModel m;
    const auto id1 = m.add(1000);
    const auto id2 = m.add(2000);

    REQUIRE(m.idAt(0) == id1);
    REQUIRE(m.idAt(1) == id2);
    REQUIRE_FALSE(m.idAt(2).has_value());      // out of range
    REQUIRE_FALSE(m.idAt(static_cast<std::size_t>(-1)).has_value());
    (void)id1;
    (void)id2;
}
