// Tests for score::NoteModel.
//
// MEMO: each TEST_CASE pins one behavioural rule. NoteModel mirrors
// MarkerModel / LoopModel for the shape rules (stable IDs, monotonic
// auto-naming, ID-based selection survives re-sort), so we don't
// re-test those exhaustively — see test_marker_model.cpp for the
// canonical pattern. What we DO test exhaustively here is what's
// note-specific: the (startMs, endMs) interval invariant + the
// midi range + the naturals-only 6.1 restriction. Those are the
// rules a future refactor most needs to preserve.

#include "qt_test_app.h"
#include "score/NoteModel.h"

#include <QSignalSpy>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using fiddler::score::Note;
using fiddler::score::NoteModel;
using fiddler::test::qtApp;

namespace {

// Extract just the (startMs, midi) pairs in current sort order — the
// data we usually want to assert about. Names + IDs are tested
// separately where they're load-bearing.
std::vector<std::pair<std::int64_t, int>>
shape(std::span<const Note> notes) {
    std::vector<std::pair<std::int64_t, int>> out;
    out.reserve(notes.size());
    for (const auto& n : notes) out.push_back({ n.startMs, n.midi });
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction + auto-naming
// ---------------------------------------------------------------------------

TEST_CASE("NoteModel: default-constructed is empty",
          "[note-model]") {
    qtApp();
    NoteModel m;
    REQUIRE(m.empty());
    REQUIRE(m.size() == 0);
    REQUIRE(m.notes().empty());
}

TEST_CASE("NoteModel: add returns a non-zero stable ID",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id1 = m.add(1000, 1400, 69);   // A4
    const auto id2 = m.add(2000, 2400, 64);   // E4
    REQUIRE(id1 != 0);
    REQUIRE(id2 != 0);
    REQUIRE(id1 != id2);
}

TEST_CASE("NoteModel: empty name auto-names 'Note 1', 'Note 2'",
          "[note-model]") {
    qtApp();
    NoteModel m;
    m.add(1000, 1400, 69);
    m.add(2000, 2400, 64);

    REQUIRE(m.notes()[0].name == "Note 1");
    REQUIRE(m.notes()[1].name == "Note 2");
}

TEST_CASE("NoteModel: user-provided name is preserved",
          "[note-model]") {
    qtApp();
    NoteModel m;
    m.add(1000, 1400, 69, "Phrase opener");
    REQUIRE(m.notes()[0].name == "Phrase opener");
}

// ---------------------------------------------------------------------------
// Interval invariant (note-specific)
// ---------------------------------------------------------------------------

TEST_CASE("NoteModel: add rejects endMs == startMs (degenerate)",
          "[note-model]") {
    // MEMO: load-bearing — notes have non-zero duration by
    // definition. A zero-length interval has no perceived pitch
    // window and breaks paint code that divides by (endMs - startMs).
    qtApp();
    NoteModel m;
    QSignalSpy spy(&m, &NoteModel::changed);
    REQUIRE(m.add(1000, 1000, 69) == 0);
    REQUIRE(m.empty());
    REQUIRE(spy.count() == 0);
}

TEST_CASE("NoteModel: add rejects endMs < startMs (inverted)",
          "[note-model]") {
    qtApp();
    NoteModel m;
    QSignalSpy spy(&m, &NoteModel::changed);
    REQUIRE(m.add(1500, 1000, 69) == 0);
    REQUIRE(m.empty());
    REQUIRE(spy.count() == 0);
}

TEST_CASE("NoteModel: setInterval rejects inverted/degenerate ranges",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id = m.add(1000, 1400, 69);

    REQUIRE_FALSE(m.setInterval(id, 2000, 2000));
    REQUIRE_FALSE(m.setInterval(id, 2000, 1500));
    // Model unchanged on rejection.
    REQUIRE(m.notes()[0].startMs == 1000);
    REQUIRE(m.notes()[0].endMs   == 1400);
}

// ---------------------------------------------------------------------------
// Pitch invariant (note-specific): range + naturals-only
// ---------------------------------------------------------------------------

TEST_CASE("NoteModel: add rejects midi below G3 (55) or above E7 (100)",
          "[note-model]") {
    // MEMO: range was tightened to the violin's playable span in
    // Step 6.1 — see NoteModel.cpp's kViolinLowestMidi /
    // kViolinHighestMidi. Pitches outside this are rejected so the
    // staff stays a faithful violin-range view.
    qtApp();
    NoteModel m;
    REQUIRE(m.add(1000, 1400, 53)  == 0);     // F3 — below G3 (also natural)
    REQUIRE(m.add(1000, 1400, 101) == 0);     // F7 — above E7 (natural)
    REQUIRE(m.add(1000, 1400, 20)  == 0);     // way below
    REQUIRE(m.add(1000, 1400, 109) == 0);     // way above
    REQUIRE(m.empty());
}

TEST_CASE("NoteModel: add accepts boundary naturals G3 (55) and E7 (100)",
          "[note-model]") {
    qtApp();
    NoteModel m;
    REQUIRE(m.add(1000, 1400, 55)  != 0);     // G3 — open G string
    REQUIRE(m.add(2000, 2400, 100) != 0);     // E7 — top of standard range
}

TEST_CASE("NoteModel: add accepts accidental (sharp / flat) pitches in range",
          "[note-model]") {
    // MEMO: accidentals are accepted from Step 6.1 onward. They
    // render at the natural-below's staff line with a ♯ glyph
    // (sharp spelling). Bowed fiddle music uses them — F♯, C♯,
    // and B♭ are common — and rejecting them would force users to
    // mistranscribe.
    qtApp();
    NoteModel m;
    REQUIRE(m.add(1000, 1400, 61) != 0);   // C#4
    REQUIRE(m.add(2000, 2400, 66) != 0);   // F#4
    REQUIRE(m.add(3000, 3400, 70) != 0);   // A#4 / Bb4
    REQUIRE(m.size() == 3);
}

TEST_CASE("NoteModel: isAcceptedPitch matches add's contract",
          "[note-model]") {
    // The helper is exposed for dock pre-validation; this test pins
    // the contract that helper-true ⇔ add-accepts.
    REQUIRE(NoteModel::isAcceptedPitch(60));        // C4
    REQUIRE(NoteModel::isAcceptedPitch(64));        // E4
    REQUIRE(NoteModel::isAcceptedPitch(69));        // A4
    REQUIRE(NoteModel::isAcceptedPitch(70));        // A#4 — accidentals OK
    REQUIRE(NoteModel::isAcceptedPitch(66));        // F#4
    REQUIRE_FALSE(NoteModel::isAcceptedPitch(53));  // F3 — below G3
    REQUIRE_FALSE(NoteModel::isAcceptedPitch(101)); // F7 — above E7
    REQUIRE_FALSE(NoteModel::isAcceptedPitch(20));  // way below
    REQUIRE_FALSE(NoteModel::isAcceptedPitch(109)); // way above
}

TEST_CASE("NoteModel: setPitch accepts accidentals, rejects out-of-range",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id = m.add(1000, 1400, 69);
    REQUIRE(m.setPitch(id, 70));          // A#4 — accepted
    REQUIRE(m.notes()[0].midi == 70);
    REQUIRE_FALSE(m.setPitch(id, 101));   // F7 — above E7
    REQUIRE(m.notes()[0].midi == 70);     // unchanged on rejection
    REQUIRE_FALSE(m.setPitch(id, 53));    // F3 — below G3
    REQUIRE(m.notes()[0].midi == 70);
}

TEST_CASE("NoteModel: setPitch accepts a natural and emits once",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id = m.add(1000, 1400, 69);
    QSignalSpy spy(&m, &NoteModel::changed);

    REQUIRE(m.setPitch(id, 64));
    REQUIRE(m.notes()[0].midi == 64);
    REQUIRE(spy.count() == 1);
}

TEST_CASE("NoteModel: setPitch to current value is a no-op (no emit)",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id = m.add(1000, 1400, 69);
    QSignalSpy spy(&m, &NoteModel::changed);

    REQUIRE(m.setPitch(id, 69));
    REQUIRE(spy.count() == 0);
}

// ---------------------------------------------------------------------------
// Sort invariant
// ---------------------------------------------------------------------------

TEST_CASE("NoteModel: insertion sort is by startMs ascending",
          "[note-model]") {
    qtApp();
    NoteModel m;
    m.add(2000, 2400, 69);
    m.add(500,   900, 64);
    m.add(1500, 1900, 67);
    m.add(3000, 3400, 65);

    REQUIRE(shape(m.notes())
            == std::vector<std::pair<std::int64_t, int>>{
                   { 500, 64}, {1500, 67}, {2000, 69}, {3000, 65}
               });
}

TEST_CASE("NoteModel: ties on startMs break by ID (older first)",
          "[note-model]") {
    // MEMO: load-bearing — chord placement leaves multiple notes at
    // the same startMs (same loop interval, different pitches).
    // Order must be deterministic so paint, indexOf, and idAt are
    // predictable.
    qtApp();
    NoteModel m;
    const auto id1 = m.add(1000, 1400, 60);   // C4
    const auto id2 = m.add(1000, 1400, 64);   // E4 (same interval)
    const auto id3 = m.add(1000, 1400, 67);   // G4 (same interval)
    REQUIRE(id1 < id2);
    REQUIRE(id2 < id3);

    REQUIRE(m.notes()[0].id == id1);
    REQUIRE(m.notes()[1].id == id2);
    REQUIRE(m.notes()[2].id == id3);
}

TEST_CASE("NoteModel: setInterval that crosses a neighbour re-sorts",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id1 = m.add(1000, 1400, 69);
    const auto id2 = m.add(2000, 2400, 64);
    REQUIRE(m.notes()[0].id == id1);

    REQUIRE(m.setInterval(id1, 3000, 3400));
    REQUIRE(m.notes()[0].id == id2);
    REQUIRE(m.notes()[1].id == id1);
}

TEST_CASE("NoteModel: setPitch never re-sorts (sort key is startMs+ID)",
          "[note-model]") {
    // MEMO: this is what allows wheel-over-note pitch-tweak to be a
    // visually smooth gesture (#6.3) — the note doesn't jump
    // horizontally as you scroll the wheel, because pitch isn't part
    // of the sort key.
    qtApp();
    NoteModel m;
    const auto id1 = m.add(1000, 1400, 60);
    const auto id2 = m.add(2000, 2400, 64);

    REQUIRE(m.setPitch(id1, 81));   // A5; way above id2's E4
    REQUIRE(m.notes()[0].id == id1); // still first
    REQUIRE(m.notes()[1].id == id2);
}

// ---------------------------------------------------------------------------
// addWithId — used by load + undo-of-delete
// ---------------------------------------------------------------------------

TEST_CASE("NoteModel: addWithId restores id + name + interval + midi",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id1 = m.add(1000, 1400, 69, "Original");
    REQUIRE(m.remove(id1));

    REQUIRE(m.addWithId(id1, 1000, 1400, 69, "Original"));
    const auto idx = *m.indexOf(id1);
    REQUIRE(m.notes()[idx].id      == id1);
    REQUIRE(m.notes()[idx].startMs == 1000);
    REQUIRE(m.notes()[idx].endMs   == 1400);
    REQUIRE(m.notes()[idx].midi    == 69);
    REQUIRE(m.notes()[idx].name    == "Original");
}

TEST_CASE("NoteModel: addWithId rejects collisions, non-positive ids, "
          "and invalid interval / pitch",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id = m.add(1000, 1400, 69);

    REQUIRE_FALSE(m.addWithId(id,  2000, 2400, 64, "x"));   // collision
    REQUIRE_FALSE(m.addWithId(0,   2000, 2400, 64, "x"));   // zero id
    REQUIRE_FALSE(m.addWithId(-1,  2000, 2400, 64, "x"));   // negative id
    REQUIRE_FALSE(m.addWithId(99,  2000, 2000, 64, "x"));   // degenerate interval
    REQUIRE_FALSE(m.addWithId(99,  2000, 2400, 10, "x"));   // out-of-range midi
    REQUIRE_FALSE(m.addWithId(99,  2000, 2400, 110, "x"));  // out-of-range midi
}

TEST_CASE("NoteModel: addWithId bumps nextId_ to avoid future collisions",
          "[note-model]") {
    qtApp();
    NoteModel m;
    REQUIRE(m.addWithId(50, 1000, 1400, 69, "Restored"));
    const auto fresh = m.add(2000, 2400, 64);
    REQUIRE(fresh > 50);
}

// ---------------------------------------------------------------------------
// Auto-name counter — same monotonic rule as MarkerModel
// ---------------------------------------------------------------------------

TEST_CASE("NoteModel: auto-name counter doesn't reuse numbers after remove",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id1 = m.add(1000, 1400, 60);
    const auto id2 = m.add(2000, 2400, 62);
    const auto id3 = m.add(3000, 3400, 64);
    REQUIRE(m.notes()[0].name == "Note 1");
    REQUIRE(m.notes()[1].name == "Note 2");
    REQUIRE(m.notes()[2].name == "Note 3");

    REQUIRE(m.remove(id2));
    m.add(4000, 4400, 65);

    const auto names = std::vector<QString>{
        m.notes()[0].name, m.notes()[1].name, m.notes()[2].name
    };
    REQUIRE(names == std::vector<QString>{"Note 1", "Note 3", "Note 4"});

    (void)id1;
    (void)id3;
}

TEST_CASE("NoteModel: clear resets notes, counters, and emits once",
          "[note-model]") {
    qtApp();
    NoteModel m;
    m.add(1000, 1400, 60);
    m.add(2000, 2400, 62);
    QSignalSpy spy(&m, &NoteModel::changed);

    m.clear();
    REQUIRE(m.empty());
    REQUIRE(spy.count() == 1);

    // Counter reset: next add() is "Note 1" again.
    m.add(500, 900, 64);
    REQUIRE(m.notes()[0].name == "Note 1");

    // Clearing an already-empty model is a no-op.
    m.clear();
    m.clear();
    QSignalSpy spy2(&m, &NoteModel::changed);
    m.clear();
    REQUIRE(spy2.count() == 0);
}

// ---------------------------------------------------------------------------
// indexOf / idAt — index ↔ ID translation
// ---------------------------------------------------------------------------

TEST_CASE("NoteModel: indexOf and idAt are inverses",
          "[note-model]") {
    qtApp();
    NoteModel m;
    const auto id1 = m.add(2000, 2400, 69);   // becomes index 1 after id2
    const auto id2 = m.add(1000, 1400, 64);   // becomes index 0

    REQUIRE(m.indexOf(id2) == 0);
    REQUIRE(m.indexOf(id1) == 1);
    REQUIRE_FALSE(m.indexOf(999).has_value());

    REQUIRE(m.idAt(0) == id2);
    REQUIRE(m.idAt(1) == id1);
    REQUIRE_FALSE(m.idAt(2).has_value());
}
