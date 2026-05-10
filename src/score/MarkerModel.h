// score::MarkerModel — user-placed named timestamps (cue points,
// "interesting bits", section starts). Distinct from BarlineModel:
// markers are sparse and named; barlines are dense and structural.
//
// Two design choices that diverge from BarlineModel and matter:
//
//   * MEMO: invariant — markers carry stable int64_t IDs, not just
//     positions. setPosition() can move a marker to a new ms which
//     re-sorts the vector and changes its index, so widgets and the
//     project viewer track selection by ID, not index. The ID is
//     allocated once on add() and never reused, even after remove.
//
//   * Markers can sit at the same source-ms (no duplicate rejection).
//     A user might legitimately mark the same instant with two
//     different concepts ("Section A start" and "Theme entry"). Each
//     gets its own ID and its own auto-name.
//
// MEMO: see memory/feedback_logs_drive_tests.md — the model-level
// FLOG calls live in MainWindow's slots, not here. The model itself
// stays UI-agnostic so it's easy to test in isolation and would be
// reusable from a CLI tool / exporter.

#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fiddler::score {

struct Marker {
    std::int64_t id        = 0;   // stable; never zero for valid markers
    std::int64_t sourceMs  = 0;   // anchor in source-time milliseconds
    QString      name;            // auto-named on add() if empty
};

class MarkerModel : public QObject {
    Q_OBJECT
public:
    explicit MarkerModel(QObject* parent = nullptr);

    [[nodiscard]] std::span<const Marker> markers() const noexcept {
        return { markers_.data(), markers_.size() };
    }
    [[nodiscard]] std::size_t size()  const noexcept { return markers_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return markers_.empty(); }

    // Add a marker. If `name` is empty, the model auto-names it as
    // "Mark N" where N is a monotonic counter that never decreases
    // (so renames + deletes don't introduce duplicate auto-names).
    // Returns the new marker's stable ID. Multiple markers at the
    // same source-ms are allowed.
    std::int64_t add(std::int64_t sourceMs, QString name = {});

    // Re-insert a marker that was previously deleted, restoring its
    // original ID and name. Used by the unified undo path so a
    // delete-undo round-trips ID-stable selection / dock state.
    // Returns true on success; false if `id` is already in use or
    // not positive. nextId_ is bumped so future `add()` calls don't
    // collide with the restored id.
    //
    // MEMO: deliberately a thin extension of add() — same insertion
    // logic, just with a caller-supplied id and no auto-naming.
    // Intended for the undo-of-delete path; safe to call from
    // anywhere, but `add()` is the right gesture for normal
    // placements because it allocates a fresh id.
    bool addWithId(std::int64_t id,
                   std::int64_t sourceMs,
                   QString      name);

    // Rename by ID. Returns true if the marker existed. The new name
    // can be empty — we don't enforce non-empty names since the
    // property editor needs to allow temporary empty edits.
    bool rename(std::int64_t id, QString name);

    // Move a marker to a new source-ms. May change its sort position
    // (and therefore its index, but not its ID). Returns true if the
    // marker existed. Idempotent for a no-op move.
    bool setPosition(std::int64_t id, std::int64_t newSourceMs);

    // Remove a marker by ID. Returns true if anything was removed.
    bool remove(std::int64_t id);

    // Drop everything: markers and counters reset to their
    // default-constructed state. Used on file load.
    void clear();

    // Find the index (in sorted order) of the marker with the given
    // ID. Used by widgets to translate an ID-based selection into
    // the index they need for paint loops.
    [[nodiscard]] std::optional<std::size_t> indexOf(std::int64_t id) const noexcept;

    // Inverse: ID at sorted index. Useful when UI iterates by index.
    [[nodiscard]] std::optional<std::int64_t> idAt(std::size_t index) const noexcept;

    // ID of the marker closest to sourceMs within toleranceMs, or
    // std::nullopt if none. Mirrors BarlineModel::nearest in
    // semantics but returns an ID (stable) rather than an index.
    [[nodiscard]] std::optional<std::int64_t>
        nearest(std::int64_t sourceMs,
                std::int64_t toleranceMs) const noexcept;

signals:
    void changed();

private:
    // Sorted ascending by sourceMs. Ties broken by ID (older
    // first) so the order is fully deterministic.
    std::vector<Marker> markers_;

    // MEMO: monotonic counters — never reused across this model's
    // lifetime. clear() resets them so a fresh project starts
    // numbering from 1 again. Across-session persistence (step 7
    // export) will need to choose how to handle restored markers'
    // IDs and the counter; not our problem yet.
    std::int64_t nextId_         = 1;
    std::int64_t nextNameNumber_ = 1;
};

} // namespace fiddler::score
