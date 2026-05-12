// score::NoteModel — user-placed notes, the fourth project-artifact
// model alongside barlines, markers, and loops.
//
// A note is the result of MANUAL TRANSCRIPTION inference from the
// recording: an interval of source time over which the fiddler
// perceived a particular pitch. That's why the shape is paired
// (startMs, endMs) + a perceived MIDI pitch — not a single anchor
// like a barline or marker.
//
// MEMO: industry-convergent shape. AnthemScore stores (pitch,
// duration, velocity); ScoreCloud detects (onsets + pitches);
// MusicXML and ABC both encode notes as (pitch, duration); MIDI
// itself is note_on/note_off intervals. The notation layer
// (step + alter, requires key) and rhythmic layer (note-value in
// beats, requires meter + tempo) are DERIVED at render / export
// time — not stored as fields here. See project_step6_plan.md in
// the assistant's memory for the workflow rationale.
//
// MEMO: invariant — notes carry stable int64_t IDs, allocated once
// on add() and never reused. setInterval() / setPitch() can re-sort
// the vector, so widgets and the project viewer track selection
// by ID, not by index. Same reasoning as MarkerModel / LoopModel.
//
// MEMO: invariant — endMs > startMs strict (mirrors LoopModel).
// add() rejects degenerate or inverted intervals by returning 0;
// setInterval() returns false.
//
// MEMO: invariant — midi ∈ [55, 100] (G3 to E7) — the violin's
// playable range. Pitches outside that span (bass-clef registers,
// extreme harmonics) are rejected by add() / setPitch() because
// they can't be played on a violin and would silently extend the
// staff Y axis beyond the widget. Adjust the constants
// kViolinLowestMidi / kViolinHighestMidi in NoteModel.cpp if the
// range needs to grow.
//
// MEMO[#step6.1]: accidentals (sharps / flats) are accepted. They
// render at the natural-below's staff line with a ♯ glyph to the
// left of the bar (sharp spelling is the only display form for now
// — midi 70 is shown as "A#4", not "Bb4"). A future key-signature
// feature will let the user choose flat spelling per key.

#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fiddler::score {

struct Note {
    std::int64_t id      = 0;   // stable; never zero for valid notes
    std::int64_t startMs = 0;   // perceived onset
    std::int64_t endMs   = 0;   // perceived offset; endMs > startMs
    int          midi    = 0;   // perceived pitch, MIDI int [21, 108]
    QString      name;          // optional free-text label
};

class NoteModel : public QObject {
    Q_OBJECT
public:
    explicit NoteModel(QObject* parent = nullptr);

    [[nodiscard]] std::span<const Note> notes() const noexcept {
        return { notes_.data(), notes_.size() };
    }
    [[nodiscard]] std::size_t size()  const noexcept { return notes_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return notes_.empty(); }

    // Add a note. Returns the new note's stable ID, or 0 if the
    // interval is degenerate (endMs <= startMs), the pitch is out
    // of [21, 108], or the pitch is not a natural (see the
    // class-level MEMO for the 6.1-only restriction). If `name` is
    // empty the model auto-names it "Note N" via a monotonic
    // counter.
    std::int64_t add(std::int64_t startMs,
                     std::int64_t endMs,
                     int          midi,
                     QString      name = {});

    // Re-insert a note with a caller-supplied ID. Used by load and
    // undo-of-delete. Same validation as add(); also rejects
    // non-positive ID and ID collisions. nextId_ is bumped so
    // future add() calls don't collide with the restored id.
    //
    // MEMO: deliberately a thin extension of add() — same insertion
    // logic, just with a caller-supplied id and no auto-naming.
    bool addWithId(std::int64_t id,
                   std::int64_t startMs,
                   std::int64_t endMs,
                   int          midi,
                   QString      name);

    // Rename by ID. Empty name accepted (UI may need a transient
    // empty edit). Returns true if the note existed.
    bool rename(std::int64_t id, QString name);

    // Move both endpoints. May re-sort (the vector is sorted by
    // startMs). Returns true if the note existed and the new
    // interval is valid. Idempotent for a no-op move.
    bool setInterval(std::int64_t id,
                     std::int64_t newStartMs,
                     std::int64_t newEndMs);

    // Change pitch only. Validates the new midi value (range +
    // naturals-only). Returns true on a real change; false on a
    // missing id or invalid midi. True for a no-op (no emit).
    bool setPitch(std::int64_t id, int newMidi);

    // Remove by ID. Returns true if anything was removed.
    bool remove(std::int64_t id);

    // Reset everything; emit changed() unless already empty.
    void clear();

    // ID → sorted index. Used by widgets to translate ID-based
    // selection into the index they need for paint loops.
    [[nodiscard]] std::optional<std::size_t>
        indexOf(std::int64_t id) const noexcept;

    // sorted index → ID. Inverse of indexOf.
    [[nodiscard]] std::optional<std::int64_t>
        idAt(std::size_t index) const noexcept;

    // True iff `midi` lies in [21,108] AND is a natural (white-key)
    // pitch. Exposed as a static helper so the dock can pre-validate
    // a property-page spinbox edit without round-tripping through
    // the model's silent rejection.
    [[nodiscard]] static bool isAcceptedPitch(int midi) noexcept;

signals:
    void changed();

private:
    // Sorted ascending by startMs. Ties broken by ID (older first)
    // so two notes starting at the same ms keep deterministic order.
    std::vector<Note> notes_;

    // MEMO: monotonic counters — reset only by clear(). Same pattern
    // as MarkerModel / LoopModel.
    std::int64_t nextId_         = 1;
    std::int64_t nextNameNumber_ = 1;
};

} // namespace fiddler::score
