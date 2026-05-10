#include "score/LoopModel.h"

#include <algorithm>
#include <utility>

namespace fiddler::score {

namespace {

// Sort loops by startMs ascending. Ties broken by ID (smaller =
// older = first) so two loops starting at the same ms keep a
// deterministic order. We do NOT tie-break by endMs — startMs+ID
// is already total-order.
bool loopLess(const Loop& a, const Loop& b) {
    if (a.startMs != b.startMs) return a.startMs < b.startMs;
    return a.id < b.id;
}

} // namespace

LoopModel::LoopModel(QObject* parent) : QObject(parent) {}

std::int64_t LoopModel::add(std::int64_t startMs,
                            std::int64_t endMs,
                            QString      name) {
    if (endMs <= startMs) return 0;   // reject degenerate range

    Loop loop;
    loop.id      = nextId_++;
    loop.startMs = startMs;
    loop.endMs   = endMs;
    if (name.isEmpty()) {
        loop.name = QString("Loop %1").arg(nextNameNumber_++);
    } else {
        loop.name = std::move(name);
    }

    const auto it = std::lower_bound(loops_.begin(), loops_.end(),
                                     loop, loopLess);
    loops_.insert(it, loop);
    emit changed();
    return loop.id;
}

bool LoopModel::addWithId(std::int64_t id,
                          std::int64_t startMs,
                          std::int64_t endMs,
                          QString      name) {
    if (id <= 0) return false;
    if (endMs <= startMs) return false;
    if (indexOf(id).has_value()) return false;
    Loop loop;
    loop.id      = id;
    loop.startMs = startMs;
    loop.endMs   = endMs;
    loop.name    = std::move(name);
    const auto it = std::lower_bound(loops_.begin(), loops_.end(),
                                     loop, loopLess);
    loops_.insert(it, loop);
    // Keep nextId_ ahead of every existing id so future add()
    // allocations don't collide with the restored loop.
    if (id >= nextId_) nextId_ = id + 1;
    emit changed();
    return true;
}

bool LoopModel::rename(std::int64_t id, QString name) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    if (loops_[*idx].name == name) return true;   // no-op, no emit
    loops_[*idx].name = std::move(name);
    emit changed();
    return true;
}

bool LoopModel::setRange(std::int64_t id,
                         std::int64_t newStartMs,
                         std::int64_t newEndMs) {
    if (newEndMs <= newStartMs) return false;
    const auto idx = indexOf(id);
    if (!idx) return false;
    auto& l = loops_[*idx];
    if (l.startMs == newStartMs && l.endMs == newEndMs) return true;  // no-op
    l.startMs = newStartMs;
    l.endMs   = newEndMs;
    // Re-sort. Same reasoning as MarkerModel::setPosition: N is small,
    // so a fresh sort beats a careful in-place shuffle.
    std::sort(loops_.begin(), loops_.end(), loopLess);
    emit changed();
    return true;
}

bool LoopModel::remove(std::int64_t id) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    loops_.erase(loops_.begin()
                 + static_cast<std::ptrdiff_t>(*idx));
    emit changed();
    return true;
}

void LoopModel::clear() {
    if (loops_.empty()
        && nextId_ == 1 && nextNameNumber_ == 1) {
        return;   // nothing to do
    }
    loops_.clear();
    nextId_         = 1;
    nextNameNumber_ = 1;
    emit changed();
}

std::optional<std::size_t>
LoopModel::indexOf(std::int64_t id) const noexcept {
    for (std::size_t i = 0; i < loops_.size(); ++i) {
        if (loops_[i].id == id) return i;
    }
    return std::nullopt;
}

std::optional<std::int64_t>
LoopModel::idAt(std::size_t index) const noexcept {
    if (index >= loops_.size()) return std::nullopt;
    return loops_[index].id;
}

} // namespace fiddler::score
