#include "score/BarlineModel.h"

#include <algorithm>
#include <cstdlib>

namespace fiddler::score {

BarlineModel::BarlineModel(QObject* parent) : QObject(parent) {}

void BarlineModel::setTimeSignature(TimeSignature ts) {
    if (timeSig_ == ts) return;
    timeSig_ = std::move(ts);
    emit changed();
}

std::optional<std::size_t> BarlineModel::add(std::int64_t sourceMs) {
    const auto it = std::lower_bound(barlines_.begin(), barlines_.end(),
                                     sourceMs);
    if (it != barlines_.end() && *it == sourceMs) {
        // Exact duplicate — return the existing index without
        // inserting or emitting. Callers that care can detect this
        // by comparing size before/after, but rejecting silently is
        // the simpler contract for tap-to-place.
        return static_cast<std::size_t>(it - barlines_.begin());
    }
    const auto inserted = barlines_.insert(it, sourceMs);
    addHistory_.push_back(sourceMs);
    const auto index = static_cast<std::size_t>(inserted - barlines_.begin());
    emit changed();
    return index;
}

void BarlineModel::removeAt(std::size_t index) {
    if (index >= barlines_.size()) return;
    const auto removedMs = barlines_[index];
    barlines_.erase(barlines_.begin() + static_cast<std::ptrdiff_t>(index));
    // Drop the matching entry from the LIFO so undoLastAdd() doesn't
    // try to remove a value that's no longer in barlines_.
    auto histIt = std::find(addHistory_.rbegin(), addHistory_.rend(),
                            removedMs);
    if (histIt != addHistory_.rend()) {
        addHistory_.erase(std::next(histIt).base());
    }
    emit changed();
}

void BarlineModel::clear() {
    if (barlines_.empty() && addHistory_.empty()) return;
    barlines_.clear();
    addHistory_.clear();
    emit changed();
}

bool BarlineModel::undoLastAdd() {
    if (addHistory_.empty()) return false;
    const auto ms = addHistory_.back();
    addHistory_.pop_back();
    const auto it = std::lower_bound(barlines_.begin(), barlines_.end(), ms);
    if (it != barlines_.end() && *it == ms) {
        barlines_.erase(it);
        emit changed();
        return true;
    }
    // Defensive: history said this ms was placed but it's not in
    // barlines_ anymore. Should be unreachable thanks to removeAt's
    // bookkeeping, but if it ever happens we still need to emit so
    // observers aren't lied to about an unchanged state.
    return false;
}

std::optional<std::size_t>
BarlineModel::nearest(std::int64_t sourceMs,
                      std::int64_t toleranceMs) const noexcept {
    if (barlines_.empty() || toleranceMs < 0) return std::nullopt;

    // Find the first element >= sourceMs; the closest is either
    // that one or its predecessor.
    const auto it = std::lower_bound(barlines_.begin(), barlines_.end(),
                                     sourceMs);
    auto best     = barlines_.end();
    auto bestDist = std::int64_t{ 0 };

    auto consider = [&](std::vector<std::int64_t>::const_iterator candidate) {
        if (candidate == barlines_.end()) return;
        const auto dist = std::llabs(*candidate - sourceMs);
        // `<=` so the *last* candidate considered wins on tie. Combined
        // with the order below (upper neighbour first, lower second),
        // this picks the earlier barline when a click falls exactly
        // between two — the conventional choice.
        if (best == barlines_.end() || dist <= bestDist) {
            best     = candidate;
            bestDist = dist;
        }
    };

    consider(it);
    if (it != barlines_.begin()) consider(std::prev(it));

    if (best == barlines_.end() || bestDist > toleranceMs) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(best - barlines_.begin());
}

} // namespace fiddler::score
