// score::LoopModel — practice loops: named ranges of source-time
// that the player can wrap around for repeated drilling. Distinct
// from MarkerModel and BarlineModel because a loop is *paired*
// (start_ms + end_ms) rather than a single anchor.
//
// MEMO: invariant — loops carry stable int64_t IDs, same reasoning
// as MarkerModel. setRange() can re-sort the vector (sorted by
// startMs ascending), so widgets and the project viewer track the
// armed-loop selection by ID, not index. IDs allocated once on
// add(), never reused, even after remove.
//
// MEMO: invariant — endMs > startMs (strict). add() rejects
// degenerate or inverted ranges by returning 0, and setRange()
// returns false. The model never silently fixes user input — UI
// validates at the boundary (the property page disables the spin
// boxes' commit when invalid).
//
// Loops are sparse (typical session: a handful) and the only
// access pattern is "iterate to draw" + "find by id". No nearest()
// helper here because loops are selected via dock or by clicking
// on the rendered band, not by hit-testing a tick.

#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fiddler::score {

struct Loop {
    std::int64_t id        = 0;     // stable; never zero for valid loops
    std::int64_t startMs   = 0;     // inclusive
    std::int64_t endMs     = 0;     // exclusive (wrap-trigger boundary)
    int          pauseMs   = 0;     // pause between repeats; 0 = none
    QString      name;              // auto-named on add() if empty
};

class LoopModel : public QObject {
    Q_OBJECT
public:
    // Default pause-between-repeats for newly-added loops. The user
    // explicitly asked for ~500 ms — a short breath that lets the ear
    // reset before the next pass without dragging the practice tempo.
    // MEMO: see memory/feedback_simple_first.md — this lives at the
    // model layer (single source of truth) rather than being baked
    // into the L-press handler in MainWindow. UI may override per-loop.
    static constexpr int kDefaultPauseMs = 500;

    explicit LoopModel(QObject* parent = nullptr);

    [[nodiscard]] std::span<const Loop> loops() const noexcept {
        return { loops_.data(), loops_.size() };
    }
    [[nodiscard]] std::size_t size()  const noexcept { return loops_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return loops_.empty(); }

    // Add a loop spanning [startMs, endMs). Returns the new loop's
    // stable ID, or 0 if the range is invalid (endMs <= startMs).
    // If `name` is empty, auto-named "Loop N" (monotonic counter).
    // pauseMs defaults to kDefaultPauseMs; pass a different value to
    // override (UI uses the default; tests sometimes want 0).
    std::int64_t add(std::int64_t startMs,
                     std::int64_t endMs,
                     QString      name    = {},
                     int          pauseMs = kDefaultPauseMs);

    // Rename by ID. Empty name accepted (UI may need a transient
    // empty edit). Returns true if the loop existed.
    bool rename(std::int64_t id, QString name);

    // Move both endpoints. Returns true if the loop existed and the
    // new range is valid (endMs > startMs). May change sort position.
    bool setRange(std::int64_t id,
                  std::int64_t newStartMs,
                  std::int64_t newEndMs);

    // Set pause-between-repeats in ms. Negative values clamp to 0.
    // Returns true if the loop existed.
    bool setPauseMs(std::int64_t id, int newPauseMs);

    // Remove a loop by ID. Drops the entry from addHistory_.
    bool remove(std::int64_t id);

    // Reset everything; emit changed() unless already empty.
    void clear();

    // Remove the most-recently-added loop still present (LIFO).
    bool undoLastAdd();

    // ID → sorted index. Used by widgets to translate ID-based
    // selection into the index they need for paint loops.
    [[nodiscard]] std::optional<std::size_t> indexOf(std::int64_t id) const noexcept;

    // sorted index → ID.
    [[nodiscard]] std::optional<std::int64_t> idAt(std::size_t index) const noexcept;

signals:
    void changed();

private:
    // Sorted ascending by startMs. Ties broken by ID (older first)
    // so the order is fully deterministic.
    std::vector<Loop> loops_;

    // LIFO of IDs in placement order; kept consistent with loops_.
    std::vector<std::int64_t> addHistory_;

    // MEMO: monotonic counters reset only by clear(). See
    // MarkerModel.h for the same pattern + rationale.
    std::int64_t nextId_         = 1;
    std::int64_t nextNameNumber_ = 1;
};

} // namespace fiddler::score
