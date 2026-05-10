// ui::undo — entry types for the unified Ctrl+Z history.
//
// MainWindow owns a single std::vector<undo::Entry> covering every
// user-driven model mutation (place, drag, edit, rename, delete) so
// Ctrl+Z reverses the last action regardless of kind. Each variant
// arm captures exactly the data needed to undo that one mutation:
//
//   * Add*       — the artifact's id (or sourceMs for barlines, which
//                  have no id). Undo = remove by id / index.
//   * Edit*      — the previous value(s). Undo = setPosition / setRange
//                  back to the snapshot.
//   * Rename*    — the previous name. Undo = rename back.
//   * Delete*    — the full pre-delete snapshot (id, position, name).
//                  Undo = re-add via addWithId so the original id and
//                  name are restored ID-stably.
//
// MEMO: barlines carry no id (BarlineModel keys by sourceMs), so
// AddBarline / DeleteBarline use sourceMs as the identity. Markers
// and loops have stable ids.
//
// MEMO: this is the simple-first implementation per feedback_simple_first.md.
// We deliberately do NOT use QUndoStack/QUndoCommand: redo and
// command grouping are out of scope for #20 (see issue's "Out of
// scope" section). If redo is ever wanted, the variant arms map
// 1:1 to QUndoCommand subclasses for a future migration.

#pragma once

#include <QString>

#include <cstdint>
#include <variant>

namespace fiddler::ui::undo {

struct AddBarline    { std::int64_t sourceMs; };
struct AddMarker     { std::int64_t id; };
struct AddLoop       { std::int64_t id; };

struct EditMarkerPos { std::int64_t id; std::int64_t prevSourceMs; };
struct EditLoopRange { std::int64_t id;
                       std::int64_t prevStartMs;
                       std::int64_t prevEndMs; };

struct RenameMarker  { std::int64_t id; QString prevName; };
struct RenameLoop    { std::int64_t id; QString prevName; };

struct DeleteBarline { std::int64_t sourceMs; };
struct DeleteMarker  { std::int64_t id;
                       std::int64_t sourceMs;
                       QString      name; };
struct DeleteLoop    { std::int64_t id;
                       std::int64_t startMs;
                       std::int64_t endMs;
                       QString      name; };

// MEMO: pre-roll is a session preference, not document state — it
// persists across launches via QSettings. Including its edits in
// the undo history is a deliberate UX call: the user reaches for
// Ctrl+Z after any spinbox / checkbox change, and treating a
// pre-roll tweak differently from a marker drag was confusing.
// The undo stack still resets per session, so closing the app
// commits the latest value through saveLayout.
struct EditPrerollMs      { int  prevMs; };
struct EditPrerollEnabled { bool prevEnabled; };

using Entry = std::variant<
    AddBarline,    AddMarker,     AddLoop,
    EditMarkerPos, EditLoopRange,
    RenameMarker,  RenameLoop,
    DeleteBarline, DeleteMarker,  DeleteLoop,
    EditPrerollMs, EditPrerollEnabled>;

} // namespace fiddler::ui::undo
