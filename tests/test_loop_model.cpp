// Tests for score::LoopModel.
//
// MEMO[refactor]: each TEST_CASE pins one behavioural rule and the
// rule is named in the comment. When refactoring the model, the
// specific values (1000, 2000, "Loop 1", etc.) are illustrative —
// the property the assertion expresses is what's load-bearing.
//
// MEMO: model is a QObject so these tests need a QApplication; we
// piggyback on qt_test_app the same way MarkerModel tests do.

#include "qt_test_app.h"
#include "score/LoopModel.h"

#include <QSignalSpy>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using fiddler::score::Loop;
using fiddler::score::LoopModel;
using fiddler::test::qtApp;

namespace {

// Helper: extract just the (start, end) pairs in current sort order.
std::vector<std::pair<std::int64_t, std::int64_t>>
ranges(std::span<const Loop> loops) {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    out.reserve(loops.size());
    for (const auto& l : loops) out.emplace_back(l.startMs, l.endMs);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction + auto-naming + range validity
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: default-constructed is empty",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    REQUIRE(m.empty());
    REQUIRE(m.size() == 0);
    REQUIRE(m.loops().empty());
}

TEST_CASE("LoopModel: add returns a non-zero stable ID",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id1 = m.add(1000, 2000);
    const auto id2 = m.add(3000, 4000);

    // MEMO: IDs are part of the public contract. Zero is reserved as
    // "no ID / invalid" — see the inverted-range case below.
    REQUIRE(id1 != 0);
    REQUIRE(id2 != 0);
    REQUIRE(id1 != id2);
}

TEST_CASE("LoopModel: add rejects degenerate or inverted ranges",
          "[loop-model]") {
    // MEMO: load-bearing rule — the model never silently fixes up
    // bad ranges. UI is responsible for keeping the spinboxes valid;
    // if it slips through, add() returns 0 and the loop is not
    // inserted. setRange() has the same rule.
    qtApp();
    LoopModel m;
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.add(1000, 1000) == 0);   // empty range rejected
    REQUIRE(m.add(2000, 1000) == 0);   // inverted range rejected
    REQUIRE(m.empty());
    REQUIRE(spy.count() == 0);          // no insert → no signal
}

TEST_CASE("LoopModel: empty name auto-names 'Loop 1', 'Loop 2'",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 2000);
    m.add(3000, 4000);

    REQUIRE(m.loops()[0].name == "Loop 1");
    REQUIRE(m.loops()[1].name == "Loop 2");
}

TEST_CASE("LoopModel: user-provided name is preserved",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 2000, "Bars 5-8");
    REQUIRE(m.loops()[0].name == "Bars 5-8");
}

TEST_CASE("LoopModel: auto-name counter doesn't reuse numbers after remove",
          "[loop-model]") {
    // MEMO: same monotonic-counter rule as MarkerModel. After
    // deleting Loop 2 and adding another, we expect Loop 4 — not a
    // recycled "Loop 2" that would clash with surviving renames.
    qtApp();
    LoopModel m;
    const auto id1 = m.add(1000, 2000);
    const auto id2 = m.add(3000, 4000);
    const auto id3 = m.add(5000, 6000);
    REQUIRE(m.loops()[0].name == "Loop 1");
    REQUIRE(m.loops()[1].name == "Loop 2");
    REQUIRE(m.loops()[2].name == "Loop 3");

    REQUIRE(m.remove(id2));
    m.add(7000, 8000);

    const auto names = std::vector<QString>{
        m.loops()[0].name, m.loops()[1].name, m.loops()[2].name
    };
    REQUIRE(names == std::vector<QString>{"Loop 1", "Loop 3", "Loop 4"});
    (void)id1;
    (void)id3;
}

TEST_CASE("LoopModel: default pause-between-repeats is 500ms",
          "[loop-model]") {
    // MEMO: load-bearing — 500ms is the user-confirmed default.
    // Changing this value should be a deliberate, documented choice;
    // the test guards against an accidental change to 0.
    qtApp();
    LoopModel m;
    m.add(1000, 2000);
    REQUIRE(m.loops()[0].pauseMs == 500);
    REQUIRE(LoopModel::kDefaultPauseMs == 500);
}

TEST_CASE("LoopModel: explicit pauseMs override on add",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 2000, {}, 0);
    m.add(3000, 4000, {}, 1500);
    REQUIRE(m.loops()[0].pauseMs == 0);
    REQUIRE(m.loops()[1].pauseMs == 1500);
}

TEST_CASE("LoopModel: negative pauseMs on add clamps to 0",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 2000, {}, -10);
    REQUIRE(m.loops()[0].pauseMs == 0);
}

// ---------------------------------------------------------------------------
// Sort invariant
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: insertion sort is by startMs ascending",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(2000, 2500);
    m.add(500,  1000);
    m.add(1500, 1800);
    m.add(3000, 3500);

    REQUIRE(ranges(m.loops()) == std::vector<std::pair<std::int64_t, std::int64_t>>{
        {500, 1000}, {1500, 1800}, {2000, 2500}, {3000, 3500}
    });
}

TEST_CASE("LoopModel: ties on startMs break by ID (older first)",
          "[loop-model]") {
    // MEMO: deterministic ordering for two loops starting at the same
    // ms — older-placed comes first. We ignore endMs in the tie-break
    // (startMs+ID is already total-order; mixing endMs in would just
    // hide bugs where two loops claim the same start).
    qtApp();
    LoopModel m;
    const auto id1 = m.add(1000, 2000);
    const auto id2 = m.add(1000, 1500);    // same start, different end
    REQUIRE(id1 < id2);

    REQUIRE(m.loops()[0].id == id1);
    REQUIRE(m.loops()[1].id == id2);
}

// ---------------------------------------------------------------------------
// rename
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: rename by valid ID updates name and emits changed",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.rename(id, "Hard turn"));
    REQUIRE(m.loops()[0].name == "Hard turn");
    REQUIRE(spy.count() == 1);
}

TEST_CASE("LoopModel: rename to the existing name is a quiet no-op",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.rename(id, "Loop 1"));
    REQUIRE(spy.count() == 0);
}

TEST_CASE("LoopModel: rename of unknown ID returns false, no emit",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    QSignalSpy spy(&m, &LoopModel::changed);
    REQUIRE_FALSE(m.rename(999, "anywhere"));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// setRange — re-sort + validity
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: setRange without reorder updates ms and emits once",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.setRange(id, 1100, 2100));   // still first in sort
    REQUIRE(m.loops()[0].startMs == 1100);
    REQUIRE(m.loops()[0].endMs   == 2100);
    REQUIRE(spy.count() == 1);
}

TEST_CASE("LoopModel: setRange that crosses a neighbour re-sorts",
          "[loop-model]") {
    // MEMO: this is exactly why loops carry stable IDs. The widget
    // armed-loop selection must survive a reorder triggered by an
    // edit in the property page.
    qtApp();
    LoopModel m;
    const auto id1 = m.add(1000, 2000);
    const auto id2 = m.add(3000, 4000);

    REQUIRE(m.loops()[0].id == id1);
    REQUIRE(m.loops()[1].id == id2);

    REQUIRE(m.setRange(id1, 5000, 6000));   // move first past second

    REQUIRE(m.loops()[0].id == id2);
    REQUIRE(m.loops()[1].id == id1);
}

TEST_CASE("LoopModel: setRange to the same range is a quiet no-op",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.setRange(id, 1000, 2000));
    REQUIRE(spy.count() == 0);
}

TEST_CASE("LoopModel: setRange rejects inverted ranges",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE_FALSE(m.setRange(id, 2000, 1000));
    REQUIRE_FALSE(m.setRange(id, 1000, 1000));   // empty range
    REQUIRE(m.loops()[0].startMs == 1000);
    REQUIRE(m.loops()[0].endMs   == 2000);
    REQUIRE(spy.count() == 0);
}

TEST_CASE("LoopModel: setRange of unknown ID returns false, no emit",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    QSignalSpy spy(&m, &LoopModel::changed);
    REQUIRE_FALSE(m.setRange(999, 1000, 2000));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// setPauseMs
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: setPauseMs updates and emits",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.setPauseMs(id, 750));
    REQUIRE(m.loops()[0].pauseMs == 750);
    REQUIRE(spy.count() == 1);
}

TEST_CASE("LoopModel: setPauseMs clamps negative to 0",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);

    REQUIRE(m.setPauseMs(id, -100));
    REQUIRE(m.loops()[0].pauseMs == 0);
}

TEST_CASE("LoopModel: setPauseMs to the same value is a quiet no-op",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id = m.add(1000, 2000);   // pause defaults to 500
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.setPauseMs(id, 500));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// remove + clear
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: remove by valid ID erases the right entry",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 1500);
    const auto id2 = m.add(2000, 2500);
    m.add(3000, 3500);
    QSignalSpy spy(&m, &LoopModel::changed);

    REQUIRE(m.remove(id2));
    REQUIRE(ranges(m.loops()) == std::vector<std::pair<std::int64_t, std::int64_t>>{
        {1000, 1500}, {3000, 3500}
    });
    REQUIRE(spy.count() == 1);
}

TEST_CASE("LoopModel: remove of unknown ID returns false, no emit",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 2000);
    QSignalSpy spy(&m, &LoopModel::changed);
    REQUIRE_FALSE(m.remove(999));
    REQUIRE(spy.count() == 0);
}

TEST_CASE("LoopModel: clear empties loops + history + counters",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 2000);
    m.add(3000, 4000);
    QSignalSpy spy(&m, &LoopModel::changed);

    m.clear();
    REQUIRE(m.empty());
    REQUIRE(spy.count() == 1);

    // Counter resets on clear → next add is "Loop 1" again.
    m.add(500, 800);
    REQUIRE(m.loops()[0].name == "Loop 1");

    // Clearing already-empty model is a no-op (no second emit).
    m.clear();
    QSignalSpy spy2(&m, &LoopModel::changed);
    m.clear();
    REQUIRE(spy2.count() == 0);
}

// ---------------------------------------------------------------------------
// undoLastAdd — LIFO of placements
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: undoLastAdd peels in placement order",
          "[loop-model]") {
    // MEMO: this is what makes Ctrl+Z mean "remove the most-recently-
    // created loop", regardless of where it sorted in the loops_
    // vector. (When we wire combined-Ctrl+Z in MainWindow we'll
    // dispatch on the placement-history kind; this is the per-model
    // building block.)
    qtApp();
    LoopModel m;

    m.add(2000, 2500);    // sorted index 0 at this point
    m.add(500,  1000);    // becomes sorted index 0; pushed last to LIFO
    m.add(1500, 1800);    // sorted index 1 now

    REQUIRE(ranges(m.loops()) == std::vector<std::pair<std::int64_t, std::int64_t>>{
        {500, 1000}, {1500, 1800}, {2000, 2500}
    });

    REQUIRE(m.undoLastAdd());                // removes the (1500,1800) loop
    REQUIRE(ranges(m.loops()) == std::vector<std::pair<std::int64_t, std::int64_t>>{
        {500, 1000}, {2000, 2500}
    });

    REQUIRE(m.undoLastAdd());                // removes the (500,1000) loop
    REQUIRE(ranges(m.loops()) == std::vector<std::pair<std::int64_t, std::int64_t>>{
        {2000, 2500}
    });

    REQUIRE(m.undoLastAdd());                // removes the (2000,2500) loop
    REQUIRE(m.empty());

    REQUIRE_FALSE(m.undoLastAdd());          // empty LIFO → false
}

TEST_CASE("LoopModel: remove of latest entry keeps undoLastAdd in sync",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id1 = m.add(1000, 2000);
    const auto id2 = m.add(3000, 4000);
    const auto id3 = m.add(5000, 6000);

    REQUIRE(m.remove(id3));                  // most-recent gone via Del

    REQUIRE(m.undoLastAdd());                // peels id2, not id3
    REQUIRE(m.size() == 1);
    REQUIRE(m.loops()[0].id == id1);
    (void)id2;
}

TEST_CASE("LoopModel: clear drains the placement LIFO",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    m.add(1000, 2000);
    m.add(3000, 4000);
    m.clear();
    REQUIRE_FALSE(m.undoLastAdd());
}

// ---------------------------------------------------------------------------
// indexOf / idAt — index ↔ ID translation
// ---------------------------------------------------------------------------

TEST_CASE("LoopModel: indexOf maps ID to current sorted index",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id1 = m.add(2000, 3000);
    const auto id2 = m.add(1000, 1500);   // becomes index 0

    REQUIRE(m.indexOf(id2) == 0);
    REQUIRE(m.indexOf(id1) == 1);
    REQUIRE_FALSE(m.indexOf(999).has_value());
}

TEST_CASE("LoopModel: idAt is the inverse of indexOf",
          "[loop-model]") {
    qtApp();
    LoopModel m;
    const auto id1 = m.add(1000, 2000);
    const auto id2 = m.add(3000, 4000);

    REQUIRE(m.idAt(0) == id1);
    REQUIRE(m.idAt(1) == id2);
    REQUIRE_FALSE(m.idAt(2).has_value());
    REQUIRE_FALSE(m.idAt(static_cast<std::size_t>(-1)).has_value());
    (void)id1;
    (void)id2;
}
