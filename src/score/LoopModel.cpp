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
                            QString      name,
                            int          pauseMs) {
    if (endMs <= startMs) return 0;   // reject degenerate range

    Loop loop;
    loop.id      = nextId_++;
    loop.startMs = startMs;
    loop.endMs   = endMs;
    loop.pauseMs = (pauseMs < 0) ? 0 : pauseMs;
    if (name.isEmpty()) {
        loop.name = QString("Loop %1").arg(nextNameNumber_++);
    } else {
        loop.name = std::move(name);
    }

    const auto it = std::lower_bound(loops_.begin(), loops_.end(),
                                     loop, loopLess);
    loops_.insert(it, loop);
    addHistory_.push_back(loop.id);
    emit changed();
    return loop.id;
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

bool LoopModel::setPauseMs(std::int64_t id, int newPauseMs) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    const int clamped = (newPauseMs < 0) ? 0 : newPauseMs;
    if (loops_[*idx].pauseMs == clamped) return true;
    loops_[*idx].pauseMs = clamped;
    emit changed();
    return true;
}

bool LoopModel::remove(std::int64_t id) {
    const auto idx = indexOf(id);
    if (!idx) return false;
    loops_.erase(loops_.begin()
                 + static_cast<std::ptrdiff_t>(*idx));

    // Same trick as MarkerModel: walk addHistory_ backwards and drop
    // the matching ID so undoLastAdd() doesn't try to re-remove it.
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

void LoopModel::clear() {
    if (loops_.empty() && addHistory_.empty()
        && nextId_ == 1 && nextNameNumber_ == 1) {
        return;   // nothing to do
    }
    loops_.clear();
    addHistory_.clear();
    nextId_         = 1;
    nextNameNumber_ = 1;
    emit changed();
}

bool LoopModel::undoLastAdd() {
    if (addHistory_.empty()) return false;
    const auto id = addHistory_.back();
    addHistory_.pop_back();
    const auto idx = indexOf(id);
    if (!idx) return false;   // defensive — see MarkerModel::undoLastAdd
    loops_.erase(loops_.begin()
                 + static_cast<std::ptrdiff_t>(*idx));
    emit changed();
    return true;
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
