#include "score/NoteModel.h"

#include <algorithm>
#include <utility>

namespace fiddler::score {

namespace {

// MEMO[#step6.1]: the accepted pitch range is the violin's playable
// span — G3 (open G string, midi 55) to E7 (midi 100). Standard
// fiddle music sits well inside this; pitches outside it (e.g.
// bass-clef registers, harmonics above E7) are rejected at the
// model layer so the staff stays a faithful violin-range view.
// Tighten / widen this in one place; tests and dock validation
// flow through NoteModel::isAcceptedPitch which uses these bounds.
constexpr int kViolinLowestMidi  = 55;   // G3 (open G string)
constexpr int kViolinHighestMidi = 100;  // E7 (top of standard range)

// Sort notes by startMs ascending. Ties broken by ID (smaller =
// older = first) so two notes starting at the same ms keep a
// deterministic order. We do NOT tie-break by endMs or by pitch —
// (startMs, ID) is already total-order.
bool noteLess(const Note& a, const Note& b) {
    if (a.startMs != b.startMs) return a.startMs < b.startMs;
    return a.id < b.id;
}

} // namespace

NoteModel::NoteModel(QObject* parent) : QObject(parent) {}

bool NoteModel::isAcceptedPitch(int midi) noexcept {
    // Range check only. Naturals AND accidentals (sharps / flats)
    // are accepted — both are notes a fiddler can play and may
    // need to transcribe.
    return midi >= kViolinLowestMidi && midi <= kViolinHighestMidi;
}

std::int64_t NoteModel::add(std::int64_t startMs,
                            std::int64_t endMs,
                            int          midi,
                            QString      name) {
    if (endMs <= startMs)        return 0;
    if (!isAcceptedPitch(midi))  return 0;

    Note n;
    n.id      = nextId_++;
    n.startMs = startMs;
    n.endMs   = endMs;
    n.midi    = midi;
    if (name.isEmpty()) {
        n.name = QString("Note %1").arg(nextNameNumber_++);
    } else {
        n.name = std::move(name);
    }

    const auto it = std::lower_bound(notes_.begin(), notes_.end(),
                                     n, noteLess);
    notes_.insert(it, n);
    emit changed();
    return n.id;
}

bool NoteModel::addWithId(std::int64_t id,
                          std::int64_t startMs,
                          std::int64_t endMs,
                          int          midi,
                          QString      name) {
    if (id <= 0)                 return false;
    if (endMs <= startMs)        return false;
    if (!isAcceptedPitch(midi))  return false;
    if (indexOf(id).has_value()) return false;

    Note n;
    n.id      = id;
    n.startMs = startMs;
    n.endMs   = endMs;
    n.midi    = midi;
    n.name    = std::move(name);

    const auto it = std::lower_bound(notes_.begin(), notes_.end(),
                                     n, noteLess);
    notes_.insert(it, n);
    // Keep nextId_ ahead of every existing id so future add() calls
    // don't collide with the restored note.
    if (id >= nextId_) nextId_ = id + 1;
    emit changed();
    return true;
}

bool NoteModel::rename(std::int64_t id, QString name) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    if (notes_[*idx].name == name) return true;   // no-op, no emit
    notes_[*idx].name = std::move(name);
    emit changed();
    return true;
}

bool NoteModel::setInterval(std::int64_t id,
                            std::int64_t newStartMs,
                            std::int64_t newEndMs) {
    if (newEndMs <= newStartMs) return false;
    const auto idx = indexOf(id);
    if (!idx) return false;
    auto& n = notes_[*idx];
    if (n.startMs == newStartMs && n.endMs == newEndMs) return true;
    n.startMs = newStartMs;
    n.endMs   = newEndMs;
    // Re-sort. Same reasoning as the other models — note counts are
    // small so a fresh sort beats an in-place shuffle.
    std::sort(notes_.begin(), notes_.end(), noteLess);
    emit changed();
    return true;
}

bool NoteModel::setPitch(std::int64_t id, int newMidi) {
    if (!isAcceptedPitch(newMidi)) return false;
    const auto idx = indexOf(id);
    if (!idx) return false;
    if (notes_[*idx].midi == newMidi) return true;   // no-op, no emit
    notes_[*idx].midi = newMidi;
    // No re-sort: sort key is (startMs, id), pitch isn't part of it.
    emit changed();
    return true;
}

bool NoteModel::remove(std::int64_t id) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    notes_.erase(notes_.begin()
                 + static_cast<std::ptrdiff_t>(*idx));
    emit changed();
    return true;
}

void NoteModel::clear() {
    if (notes_.empty()
        && nextId_ == 1 && nextNameNumber_ == 1) {
        return;   // nothing to do
    }
    notes_.clear();
    nextId_         = 1;
    nextNameNumber_ = 1;
    emit changed();
}

std::optional<std::size_t>
NoteModel::indexOf(std::int64_t id) const noexcept {
    for (std::size_t i = 0; i < notes_.size(); ++i) {
        if (notes_[i].id == id) return i;
    }
    return std::nullopt;
}

std::optional<std::int64_t>
NoteModel::idAt(std::size_t index) const noexcept {
    if (index >= notes_.size()) return std::nullopt;
    return notes_[index].id;
}

} // namespace fiddler::score
