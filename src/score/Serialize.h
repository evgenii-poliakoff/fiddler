// score::serialize — JSON round-trip for the three score models +
// the TimeSignature record. Lives in `score/` (not `ui/`) so the
// models stay UI-agnostic; the actual file I/O happens in MainWindow.
//
// MEMO: free functions rather than methods on the models — keeps
// the model headers small and the JSON shape easy to evolve in one
// place. The Qt JSON types (QJsonArray / QJsonObject) are the
// interchange medium; MainWindow wraps them with QJsonDocument and
// the QFile I/O.
//
// Schema version 2 (current; bumped from 1 in Step 6.1 to add notes):
//
//   {
//     "version": 2,
//     "audioPath": "/abs/path/to/tune.mp3",
//     "timeSignature": { "numerator": 4, "denominator": 4,
//                        "tuneType": "Reel" },
//     "preroll": { "enabled": true, "ms": 500 },
//     "barlines": [ { "sourceMs": 1000 }, { "sourceMs": 2000 } ],
//     "markers":  [ { "id": 1, "sourceMs": 1500, "name": "Hard turn" } ],
//     "loops":    [ { "id": 1, "startMs": 500, "endMs": 1500,
//                     "name": "Loop 1" } ],
//     "notes":    [ { "id": 1, "startMs": 1100, "endMs": 1400,
//                     "midi": 69, "name": "Note 1" } ]
//   }
//
// Backward compatibility: a v1 file (no "notes" key) loads fine — a
// missing array is equivalent to an empty notes list. We do NOT
// auto-rewrite v1 files on load; the version is bumped only when
// the user saves through the v2 build. Forward compatibility (v2
// read by v1 build) is intentionally NOT supported; v1 has been
// pre-release.
//
// IDs round-trip exactly via MarkerModel::addWithId / LoopModel::addWithId
// / NoteModel::addWithId. Audio path is absolute; cross-machine
// portability is a future concern.

#pragma once

#include "score/BarlineModel.h"

#include <QJsonArray>
#include <QJsonObject>

namespace fiddler::score {

class LoopModel;
class MarkerModel;
class NoteModel;

// Schema version this build writes and accepts on load. Bumped from
// 1 to 2 in Step 6.1 to add the notes array. v1 files load (notes
// stays empty); v2 files are NOT readable by a v1 build.
//
// MEMO: bump in lockstep with any breaking JSON change. Additive
// changes (a new optional key whose absence is well-defined) do NOT
// require a bump — only changes that older readers would mis-handle.
constexpr int kProjectFormatVersion = 2;

QJsonArray  toJson(const BarlineModel& m);
QJsonArray  toJson(const MarkerModel&  m);
QJsonArray  toJson(const LoopModel&    m);
QJsonArray  toJson(const NoteModel&    m);
QJsonObject toJson(const TimeSignature& ts);

// Each loadFrom clears the target model first, then re-adds every
// entry from the array. Marker / loop ids round-trip via
// addWithId; barlines key by sourceMs so no id concept. Returns
// false on malformed input (e.g. an entry missing required fields)
// — the caller should reject the whole load to avoid a partial
// state. The model is left empty on rejection.
bool loadFrom(BarlineModel& m, const QJsonArray& arr);
bool loadFrom(MarkerModel&  m, const QJsonArray& arr);
bool loadFrom(LoopModel&    m, const QJsonArray& arr);
bool loadFrom(NoteModel&    m, const QJsonArray& arr);

// Parse a TimeSignature out of a JSON object. Defaults (4/4, empty
// tuneType) on missing fields — TimeSignature has no required
// keys; a project saved by a future schema with extra fields will
// still load the basics.
TimeSignature timeSignatureFromJson(const QJsonObject& obj);

} // namespace fiddler::score
