#include "score/MarkerModel.h"

#include <algorithm>
#include <cstdlib>

namespace fiddler::score {

namespace {

// Sort markers by sourceMs ascending. Ties broken by ID (smaller =
// older = first) so two markers at the same ms keep a deterministic
// order that mirrors placement.
bool markerLess(const Marker& a, const Marker& b) {
    if (a.sourceMs != b.sourceMs) return a.sourceMs < b.sourceMs;
    return a.id < b.id;
}

} // namespace

MarkerModel::MarkerModel(QObject* parent) : QObject(parent) {}

std::int64_t MarkerModel::add(std::int64_t sourceMs, QString name) {
    Marker m;
    m.id       = nextId_++;
    m.sourceMs = sourceMs;
    if (name.isEmpty()) {
        m.name = QString("Mark %1").arg(nextNameNumber_++);
    } else {
        m.name = std::move(name);
    }

    // Sorted insertion using lower_bound + the comparator. Using the
    // same comparator as the sort keeps invariants consistent.
    const auto it = std::lower_bound(markers_.begin(), markers_.end(),
                                     m, markerLess);
    markers_.insert(it, m);
    addHistory_.push_back(m.id);
    emit changed();
    return m.id;
}

bool MarkerModel::rename(std::int64_t id, QString name) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    if (markers_[*idx].name == name) return true;   // no-op, no emit
    markers_[*idx].name = std::move(name);
    emit changed();
    return true;
}

bool MarkerModel::setPosition(std::int64_t id, std::int64_t newSourceMs) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    if (markers_[*idx].sourceMs == newSourceMs) return true;  // no-op
    markers_[*idx].sourceMs = newSourceMs;
    // Re-sort. O(N log N) but N is small (a handful per session) so
    // this beats a more elaborate move-and-shift that's harder to
    // get right. If marker counts ever explode we can revisit.
    std::sort(markers_.begin(), markers_.end(), markerLess);
    emit changed();
    return true;
}

bool MarkerModel::remove(std::int64_t id) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    markers_.erase(markers_.begin()
                   + static_cast<std::ptrdiff_t>(*idx));

    // MEMO: keep addHistory_ in sync with markers_. Walk backwards
    // because the most-recent occurrence is the one most likely to
    // be the entry we want to drop here (a quick Del following a
    // tap-to-place). Same trick as BarlineModel::removeAt.
    for (std::size_t i = addHistory_.size(); i > 0; --i) {
        if (addHistory_[i - 1] == id) {
            addHistory_.erase(addHistory_.begin()
                              + static_cast<std::ptrdiff_t>(i - 1));
            break;
        }
    }
    emit changed();
    return true;
}

void MarkerModel::clear() {
    if (markers_.empty() && addHistory_.empty()
        && nextId_ == 1 && nextNameNumber_ == 1) {
        return;   // nothing to do, don't emit
    }
    markers_.clear();
    addHistory_.clear();
    nextId_         = 1;
    nextNameNumber_ = 1;
    emit changed();
}

bool MarkerModel::undoLastAdd() {
    if (addHistory_.empty()) return false;
    const auto id = addHistory_.back();
    addHistory_.pop_back();
    const auto idx = indexOf(id);
    if (!idx) {
        // Defensive: should be unreachable. addHistory_ is kept
        // consistent with markers_ by remove() / clear(). If we ever
        // get here, the LIFO has been drained but produced no
        // observable change — be honest about that.
        return false;
    }
    markers_.erase(markers_.begin()
                   + static_cast<std::ptrdiff_t>(*idx));
    emit changed();
    return true;
}

std::optional<std::size_t>
MarkerModel::indexOf(std::int64_t id) const noexcept {
    for (std::size_t i = 0; i < markers_.size(); ++i) {
        if (markers_[i].id == id) return i;
    }
    return std::nullopt;
}

std::optional<std::int64_t>
MarkerModel::idAt(std::size_t index) const noexcept {
    if (index >= markers_.size()) return std::nullopt;
    return markers_[index].id;
}

std::optional<std::int64_t>
MarkerModel::nearest(std::int64_t sourceMs,
                     std::int64_t toleranceMs) const noexcept
{
    if (markers_.empty() || toleranceMs < 0) return std::nullopt;

    // Linear scan because markers carry IDs and ties on ms are
    // possible — binary search alone wouldn't tell us which of two
    // tied markers to prefer. Marker counts are small (handful per
    // session) so this is fine.
    std::optional<std::int64_t> bestId;
    std::int64_t                bestDist = 0;
    for (const auto& m : markers_) {
        const auto dist = std::llabs(m.sourceMs - sourceMs);
        if (dist > toleranceMs) continue;
        if (!bestId || dist <= bestDist) {
            // MEMO: `<=` so the *later* marker (higher ID at same
            // distance) wins ties when iterating in sort order.
            // Combined with the ascending iteration, this means a
            // user clicking exactly between two markers selects the
            // later-placed one. Reasonable; barlines tie-break to
            // earlier (different convention but each is internally
            // consistent).
            bestId   = m.id;
            bestDist = dist;
        }
    }
    return bestId;
}

} // namespace fiddler::score
