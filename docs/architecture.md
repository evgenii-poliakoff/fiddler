# Architecture & engineering rules

This document is the project's design memory. If you're a new contributor
(human or AI assistant) starting work on this codebase, read this end-to-end
before touching anything. It captures both *what* we chose and *why*, plus
several non-obvious rules we paid for in debugging time and don't want to
relearn.

## Project context

Fiddler is a desktop transcription aid for self-taught Irish fiddle
players. The user opens an audio recording, slows it down without changing
pitch, and transcribes the tune onto a synchronised musical staff. The
staff cursor and audio playback position track each other bidirectionally;
placing a note on the staff plays a reference tone for pitch-matching.

Target platform: **Ubuntu 24.04 desktop, x86_64**. Other platforms are not
goals at this stage.

Language: **C++20**. CMake build system, Ninja generator preferred.

## Stack choices

Each subsection records what we picked and why, plus the alternatives we
considered. When evolving the stack, revisit these notes — the alternatives
that didn't win the first round are often the right answer the second.

### Build: CMake + Ninja

Standard, works everywhere we need it. CMake 3.22+. Compile commands are
exported (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`) so clangd / IDE tooling works
out of the box.

### GUI: Qt 6 (Widgets)

First-class on Ubuntu, ships in 24.04 as `qt6-base-dev`. `QPainter` and
`QGraphicsView` are well-suited to the staff editor that arrives in steps
4–5. Considered: GTK 4 (less ergonomic from C++), Dear ImGui (great for
prototyping, weaker for a polished score editor), Slint (immature for
this use case).

### Audio decoding: FFmpeg (`libavformat` + `libavcodec`)

One API covers WAV, FLAC, MP3, OGG/Vorbis, M4A/AAC, OPUS, and more. The
alternative — libsndfile + a separate MP3 decoder (mpg123) — has simpler
ergonomics but needs format-specific glue. We hide all FFmpeg types
behind `Decoder` so the rest of the app is insulated from FFmpeg ABI
churn.

### Audio playback: PortAudio

Direct buffer-callback model is what we need for time-stretching: the
callback pulls from a ring buffer fed by Rubber Band. Qt Multimedia
abstracts too much for this and is harder to hook the stretcher into.
RtAudio is a viable alternative with similar ergonomics; we picked
PortAudio because it's more widely deployed.

### Time stretching: Rubber Band Library

State-of-the-art open option. Default in Ardour and Mixxx. License:
GPL or commercial. We adopt GPL-3.0 in step 0; switch to a commercial
Rubber Band licence if a closed-source build is ever needed.
SoundTouch (WSOLA, LGPL) is the lighter fallback, but quality drops
below ~75 % tempo, which is exactly where fiddlers want to listen.

Configured for realtime use with `OptionProcessRealTime` so changes
take effect on the next process call. Engine is `OptionEngineFiner`
(R3, the default since Rubber Band 3.0): finer time/frequency
resolution preserves bow attacks and ornament transients down to 25 %
playback rate. R2 (`OptionEngineFaster`) is lighter on CPU but smears
transients at extreme stretches — the regime fiddlers actually use.
`OptionPitchHighConsistency` keeps pitch rock-stable across ratio
changes. Formant preservation is off; it's a vocal feature and on
fiddle it can subtly tint timbre.

The `audio::Stretcher` class wraps `RubberBandStretcher` behind pImpl
(Rule 6) so Rubber Band headers don't leak into Player or the UI.

### Waveform view: `QWidget` + `QPainter`

`audio::WaveformOverview` is the data model — N peak buckets per
channel keyed on source-time milliseconds — and `ui::WaveformWidget`
is a plain `QWidget` that paints peaks plus a playhead cursor and
emits `seekRequested(ms)` on left-click. Source-time is the universal
coordinate; every overlay added later (staff barlines in step 5,
note ranges and selections in step 6+) hangs off the same axis.

Architectural commitments made up front, against the temptation to
defer all of them (Rule 8 still applies — these are tiny *interface*
choices, not features):

- The widget consumes a `std::shared_ptr<const WaveformOverview>` and
  emits a signal; it knows nothing about `Player`. Parent wires the
  loop. Future views (mini-strip, second pane, spectrogram) drop in
  against the same data without touching `Player`.
- Public coordinate transforms `xToMs(int)` / `msToX(int64_t)` so step
  5's overlay code maps pixels ↔ source time without reimplementing.
- Overview construction is two-stage: `WaveformOverview::fromSamples`
  is pure (testable end-to-end without FFmpeg); `buildOverview(Decoder&)`
  is the production glue that decodes a file and forwards.

Considered alternatives:

- `QGraphicsView` was attractive for the eventual draggable overlay
  handles in step 6+, but `QWidget` + `QPainter` is enough for peaks
  + cursor today. Migrating later is localised inside the widget.
- An overlay-layer / region API was deliberately *not* built up front;
  step 5 will introduce the first concrete overlay (barlines), and
  guessing the shape now would produce the wrong abstraction.
- An on-disk thumbnail cache was deferred; re-decoding on file open
  takes well under a second for typical Irish-trad recordings.

### Score model: barlines + time signature

`score::BarlineModel` is the shared data layer for everything the
user annotates on top of the audio. For step 5 it holds two pieces:

- a list of `int64_t` source-time anchors (barline positions), kept
  sorted ascending and paired with an LIFO add-history so a single
  `Ctrl+Z` peels the most recent placement;
- a `TimeSignature` value (numerator, denominator, plus a `tuneType`
  string label like `"Reel"` / `"Jig"`).

Two design commitments worth recording, because both look unremarkable
in passing but are load-bearing:

- **`TimeSignature` is descriptive, not prescriptive.** It labels what
  the user thinks the tune is in; it never generates or constrains
  barline positions. Each bar's length is just
  `barlines[i+1] - barlines[i]` — whatever the recording happens to
  give you. That's what lets the model handle non-uniform recording
  speed (rubato) and crooked tunes (irregular bar lengths or counts
  in a section) honestly. A future auto-suggestion advisor will
  *propose* positions for the user to confirm; it must never write to
  the model directly.
- **Barline placement is keyboard-tap-driven, not click-driven.** The
  primary gesture is `B` to place a bar at `Player::position()` —
  the workflow that matches how an ear-oriented player actually
  marks bars (slow the tempo, tap on each downbeat). Click on the
  waveform / staff is reserved for *seek*. Del removes the
  currently-selected bar (newly-placed bars are auto-selected, so
  "tap B, press Del" works without an intermediate click);
  `Ctrl+Z` peels the most-recent placement.

The model is a `QObject` so widgets can observe `changed()` and
repaint, but it owns no widget references and pulls no UI types
beyond `QString` for the tune-type label.

### Score model: markers (named cue points)

`score::MarkerModel` is the second project-artifact model. Distinct
from `BarlineModel`:

- **Sparse, not dense.** A user typically has a handful of markers
  per tune ("intro", "hard bit", "modulation"), versus dozens of
  barlines.
- **Named, not anonymous.** Each marker carries a `QString name`
  that auto-defaults to "Mark N" via a monotonic counter (so a
  rename + add cycle never produces duplicate auto-names) and the
  user renames freely via the project viewer dock.
- **Repositionable.** `MarkerModel::setPosition(id, ms)` moves a
  marker to a new source-time and re-sorts; the marker's index
  changes, but it carries a stable `int64_t id` that survives the
  move. Selection in the widgets and the dock is therefore by ID,
  not by index. `BarlineModel` doesn't need this because barlines
  aren't repositioned — they're placed and removed, never moved.
- **Duplicates allowed at the same ms.** Two markers can sit at
  the same instant ("Section A start" and "Theme entry"); sort
  ties are broken by ID so paint order stays deterministic.

A small `undoLastAdd` LIFO mirrors `BarlineModel`'s, but the
authoritative undo across both models lives at MainWindow level
(see "Combined `Ctrl+Z`" below). The model's own LIFO becomes
incidental in MainWindow context — kept around because the model
is unit-tested standalone.

### Score model: practice loops

`score::LoopModel` is the third project-artifact model. Distinct
from both barlines and markers because a loop is *paired* — a
named region rather than a single anchor.

- **Stable IDs.** Same rationale as `MarkerModel`: `setRange()`
  re-sorts on startMs change, so widgets and the dock track the
  selected / armed loop by stable `int64_t` ID, not by index.
- **Strict invariant `endMs > startMs`.** `add()` returns 0 and
  `setRange()` returns `false` on degenerate or inverted ranges.
  The model never silently fixes user input; the UI is responsible
  for keeping the property-page spinboxes valid (the End spinbox's
  lower bound is dynamically clamped to Start + 1 and vice versa).
- **Per-loop pause-between-repeats.** Each loop carries its own
  `pauseMs`, defaulting to **500 ms** — a short breath the user
  asked for so the ear can reset before the next pass without
  dragging the practice tempo. Negative values clamp to 0.
- **No `nearest()` helper.** Loops are selected from the dock or
  from the band overlay — never by tick hit-test — so the lookup
  pattern that `BarlineModel` and `MarkerModel` need doesn't apply.

The model owns the same kind of `undoLastAdd` LIFO as the others;
combined `Ctrl+Z` at MainWindow level dispatches across all three
kinds (see below).

### Loop creation gesture: two anchors + `L`

A loop is created from any two anchors, of any kind. The
gesture has three parts:

1. **Primary anchor.** Single-click any barline or marker on the
   waveform, the staff, or the dock's marker list. Standard
   selection — solid yellow tick (or cyan, for markers).
2. **Secondary anchor.** Ctrl+click any second artifact, again on
   any of the three surfaces (waveform / staff / dock). The prior
   primary's ms is captured into a `secondaryAnchorMs_` slot on
   the score widgets; visual feedback is a dashed yellow / cyan
   tick at that ms, mirrored to whichever view the user isn't
   currently looking at.
3. **`L`.** Window-scope shortcut. MainWindow reads
   `WaveformWidget::primaryAnchorMs()` and `secondaryAnchorMs()`,
   takes `min`/`max` to derive `startMs`/`endMs`, and passes the
   pair to `LoopModel::add`. The secondary slot is consumed
   (cleared) on success — a fresh `L` press needs a fresh
   Ctrl+click. Refuses degenerate ranges (`primary == secondary`)
   and missing-anchor cases (silent no-op + log line).

Two design commitments worth recording:

- **Secondary stores a raw ms, not an artifact ID.** The only
  consumer is the L shortcut, which only needs two ms values to
  hand to `LoopModel::add`. Storing an ID would create a
  lifetime headache (the originating artifact may be deleted
  between the Ctrl+click and the L press); a raw ms doesn't care.
- **Secondary mirrors waveform ↔ staff but is not in the dock.**
  The dock is a gesture detector for the dock-Ctrl+click path — it
  emits `loopAnchorAddRequested` / `loopAnchorClearRequested` and
  MainWindow translates those into setSecondaryAnchorMs calls on
  the score widgets. The dock itself never holds secondary state.

**Painted-as-dashed-on-overlap.** A naive secondary indicator
(separate dashed tick painted over the artifact's solid tick)
loses its dashing — the underlying solid line fills the dash
gaps. Two cooperating fixes ensure the secondary is always
visibly distinct:
1. When an artifact's ms equals the secondary anchor's ms, the
   artifact itself is painted dashed (replacing its normal solid
   rendering).
2. The dashed indicator paints **after the playhead cursor** so
   that, when the cursor and the secondary share an x (the common
   case right after tap-place because tap-place seeks the cursor
   to the new artifact), the dashes pierce through the cursor's
   red. Gap pixels reveal the cursor; dash pixels show yellow /
   cyan. The user sees both clearly.

### Loop activation: arm + wrap-around

A loop can be **armed** — selected for transport. Two arming
gestures, with deliberately different intents:

- **Double-click on the loop's row in the dock** — "jump and
  play". Arms the loop, seeks to `startMs`, starts playback.
  Mirrors `markerActivated`'s aggressive idiom; this is what the
  user reaches for when they explicitly want to drill the loop
  *now*.
- **Arm checkbox on the loop property page** — "armed in place".
  Arms without seeking and without auto-playing. The user might
  be mid-listen and just want wrap-around to engage at `endMs`;
  a seek would be jarring.

Pressing the **Play button while a loop is armed** seeks to
`startMs` first if the cursor is outside `[startMs, endMs)` —
otherwise plays from current (mid-listen continuity). Three
branches:
- pos < startMs → seek (the "I armed and pressed Play to drill
  the loop" scenario; without the seek the user hears the whole
  tune up to endMs once before wrap engages).
- pos in loop → no seek (mid-listen continuity).
- pos >= endMs → seek (without it the user is stuck past endMs;
  the GUI wrap path would only fire once and from a stale frame).

**Wrap-around** lives in `MainWindow::updatePosition` — the
existing 50 ms GUI-poll timer. When `pos >= loop.endMs`:
- `pauseMs == 0` → tight wrap with `seek(startMs)` while playing.
- `pauseMs > 0` → pause, seek to `startMs` immediately (so the
  visible position slides back right away — silence reads as
  "loop break", not "dead air at the end of the tune"), then
  `QTimer::singleShot(pauseMs, ...)` to resume play.

GUI-driven (rather than audio-callback-driven) is the deliberate
Rule-8 choice: ~50 ms wrap jitter is inaudible during practice,
and it keeps the loop region out of the Player + Rubber Band
internals. A `wrapPending_` flag blocks re-entry while the pause
timer is in flight; the timer's lambda re-checks `armedLoopId_`
so disarm-during-pause is a quiet no-op.

**Stop disarms.** Per the design discussion, Stop is the user's
"exit loop mode" gesture. The next Play press resumes normal
(non-wrapping) playback. Removing the armed loop (Del or
Ctrl+Z) also disarms via a `LoopModel::changed` listener.
Loading a new file disarms.

### Project viewer dock

`ui::ProjectViewerDock` is the right-side `QDockWidget` that hosts
the project's "artifacts" — markers and loops today, notes /
sections later. Layout is two-pane vertical:

```
┌─────────────────────────────────┐
│ Project                         │ ← QDockWidget title
├─────────────────────────────────┤
│ ▾ Markers                       │
│   Mark 1     (1000 ms)          │ ← QTreeWidget; one row per artifact
│   Mark 2     (1500 ms)          │
│ ▾ Loops                         │
│   ▶ Loop 1   (1000–3000 ms)     │ ← ▶ glyph marks the armed loop
│   Loop 2     (5000–7000 ms)     │
├─────────────────────────────────┤
│ Properties                      │
│   Name:     [Mark 1____]        │ ← QStackedWidget; per-type page
│   Position: [1000   ↕]          │   (here, the Marker page)
└─────────────────────────────────┘
```

**`Qt::RightDockWidgetArea`** by convention — DaVinci Resolve,
Final Cut, Adobe Audition, GarageBand all put their inspector /
project panel on the right. Browsers (track lists, file libraries)
go on the left in DAWs; this dock is more inspector than browser,
so right is the right call.

**Selection mirrored three ways.** Clicking a marker row in the
dock emits `markerSelectionChanged(id)`; MainWindow forwards to
both score widgets so all three views show the same highlight.
Symmetric in the other directions — clicking a marker on the
waveform or staff also navigates the tree. The
`mirroringSelection_` flag on MainWindow stops the bounce.

**Property-page editing.**
- Name → `editingFinished` → `MarkerModel::rename`.
- Position (ms) → `editingFinished` → `MarkerModel::setPosition`.
  We bind to `editingFinished` (Enter / Tab / focus-loss), *not*
  `valueChanged` — `valueChanged` would round-trip through the
  model on every keystroke and re-format the spinbox text, which
  strips leading zeros. The user editing "30004" by deleting the
  '3' expects "0004" to remain in the field; with editingFinished
  the in-progress text survives until the user signals "done".

**Double-click on a marker row** emits `markerActivated(id)`
(distinct from the passive `markerSelectionChanged`) — MainWindow
seeks the player to the marker's source-time and starts playback.
"Jump and play" is the standard DAW idiom for activate-on-
double-click in a marker / clip / region list.

**Loop property page** has Name + Start + End + Pause +
Armed-checkbox fields. The range spinboxes maintain the
`endMs > startMs` invariant by dynamically clamping each box's
opposite bound: editing Start moves End's lower bound to
`Start+1`, and vice versa. The Arm checkbox emits
`loopArmToggleRequested(id, armed)`; MainWindow holds the
canonical armed state and pushes it back via `setArmedLoopId(id)`,
which the dock uses to keep the checkbox + tree-row glyph in sync
without re-emitting (suppressed by the `updatingPropertyPage_`
guard).

**Adding a new artifact category** is one new top-level item
under the tree's invisible root, plus one new page in the
property `QStackedWidget`. No restructuring required.

**Reopening the dock after closing.** The dock's title-bar X
hides it; a `View → Project Viewer` menu entry (with **F4**
shortcut) brings it back. The action is `dock->toggleViewAction()`
— Qt wires its checkable state to the dock's visibility
automatically. Layout (including dock visibility, floating
state, position, and sizes) is persisted via QSettings: the
`closeEvent` saves geometry + state, and the constructor
restores them. The user's chosen layout therefore survives
across launches.

### Combined `Ctrl+Z` across barlines + markers + loops

The user's mental model is "Z = undo last placement", regardless
of whether the last thing placed was a barline, a marker, or a
loop. Each model has its own placement-history LIFO, but no model
alone knows the global order across kinds. MainWindow keeps a
small additional LIFO of `PlacementKind` enums:

```cpp
enum class PlacementKind { Barline, Marker, Loop };
std::vector<PlacementKind> placementHistory_;
```

`onTapBarline` / `onTapMarker` / `onCreateLoop` push the kind on
a successful add; `onUndoLastPlacement` pops the back and
dispatches to the matching model's `undoLastAdd`, walking past
stale entries (a manual Del between placements leaves the history
slightly out of sync, but the loop tolerates that). This is a
degenerate undo — placements only, no redo, no edit history —
which is sufficient for the tap-along workflow. A full
`QUndoStack` is deferred until something demands it (Rule 8).

### Time-signature picker: tradition-named, technical-form deferred

The combo box surfaces ten Irish-trad presets — Reel (4/4), Hornpipe
(4/4), Polka (2/4), March (2/4), Single Jig (6/8), Double Jig (6/8),
Slip Jig (9/8), Slide (12/8), Waltz (3/4), Mazurka (3/4) — picking
one pushes a complete `TimeSignature` (numerator + denominator +
label) into the model. The label rides through to step 7's MusicXML
/ ABC export so genre survives the round-trip.

Free-meter Airs and explicit numerator/denominator entry are
reachable in the model but don't yet have a UI; they're a follow-on
once we have a feel for how the preset path performs in real
transcription. See `memory/project_handbook_for_self_taught.md` for
why traditional names lead the picker rather than bare technical form.

### Score widgets: WaveformWidget overlay + StaffWidget

Both widgets observe the same `BarlineModel`, `MarkerModel`, and
`LoopModel` (each held as `shared_ptr<const ...>`) and the same
source-time axis, so barline ticks, marker flags, and loop bands
line up 1:1 horizontally. Each widget is small, self-contained,
and emits a narrow set of signals (`seekRequested`,
`barlineSelectionChanged`, `markerSelectionChanged`,
`loopSelectionChanged`, `secondaryAnchorChanged`,
`barlineDeleteRequested`, `markerDeleteRequested`) that MainWindow
turns into model mutations and seek calls.

**Selection is mutually exclusive across all three artifact
kinds.** A widget can have at most one of `selectedBarline_` /
`selectedMarkerId_` / `selectedLoopId_` populated; the click
handler and the dock both enforce this. The user wanted "the
selected artifact" to be a single concept (the property viewer
shows its properties), and this is the simplest way to deliver
that.

**Marker visual** is a vertical tick (cyan to distinguish from
the amber barline ticks) plus a small label flag at the top of
the widget bearing the marker's name. The flag is elided if the
name is too long; the rendering colour brightens for the
currently-selected marker.

**Loop visual** is a translucent sage-green band spanning
`[startMs, endMs)` across the full vertical extent of the
widget, with the loop name in a label strip at the bottom of
the band (so it doesn't clash with the marker flag row at the
top). Selected loops render at higher alpha; loops are
render-only on the score widgets — selection is dock-driven, so
clicking inside a band on the waveform doesn't change selection
(this would conflict with the seek-anywhere affordance, and the
explicit dock surface is the canonical way to inspect a loop).

**Secondary-anchor visual** is a dashed indicator at the captured
ms — yellow if the anchor lands on a barline (or stands alone),
cyan if it lands on a marker. See "Loop creation gesture" above
for the painted-as-dashed-on-overlap rules and the cursor-pierce
behaviour.

Things deliberately not built in this PR (deferred to follow-on
work):

- Tap-latency compensation (subtracting ~150 ms from
  `player.position()` at tap time to account for human reaction
  lag).
- Drag-to-nudge a placed barline / marker / loop endpoint.
- Auto-suggestion of bar positions.
- Free-meter / Air / "Other..." entries in the time-sig picker.
- A generic overlay-layer registration API. We have three
  concrete overlays now (barlines + markers + loops); the
  abstraction can wait until note ranges arrive in step 6+ and
  we have a fourth example to design from.
- Sample-accurate loop wrap. The current GUI-poll-driven wrap
  has up to ~50 ms jitter, which is inaudible during practice;
  if a future use case (looped playback into a tracking studio?)
  needs tighter wrap, the hook would move into the audio-callback
  path.

### Logging: spdlog (behind a facade)

The application code never includes spdlog headers directly. All log
calls go through `src/util/Log.h`'s `FLOG_*` macros. The facade exists
so we can swap loggers without touching call sites. spdlog was picked
over `std::format`-from-scratch (more code) and Qt's `QLoggingCategory`
(awkward in non-Qt audio code).

### Test framework: Catch2 v3 + Qt6::Test for GUI

Catch2 (Ubuntu's `catch2` package) drives the suite. Header-light,
low ceremony, expressive `REQUIRE`/`SECTION` syntax.

GUI tests use Qt's `QTest` namespace for input synthesis
(`QTest::mouseClick`, `QTest::qWaitFor`) and `QSignalSpy` to assert
emitted signals. Two pieces of plumbing are non-obvious and worth
recording:

- **Stack-allocated `QApplication` in a custom `main()`.** A
  function-local-static `QApplication` segfaults at process exit on
  Ubuntu 24.04 / Qt 6.4 because Qt's own globals destruct in a
  different order than the static. We link `Catch2::Catch2` (not
  `Catch2WithMain`) and provide our own `main()` in
  `tests/qt_test_app.cpp` that constructs `QApplication` on the stack,
  then calls `Catch::Session().run(argc, argv)`.
- **`QT_QPA_PLATFORM=offscreen`** is set both in that `main()` (for
  direct invocation of the test exe) and via `catch_discover_tests
  PROPERTIES ENVIRONMENT` (so ctest invocations see it before the
  child process starts). Qt resolves the variable during
  `QApplication`'s constructor; an in-process `qputenv` would be too
  late on hosts where `DISPLAY` is set.

The integration test for `MainWindow` writes a small in-memory PCM
WAV (440 Hz sine, 2 s, stereo, 44.1 kHz) into the system temp dir on
first use, so the suite is self-contained and doesn't depend on the
`.gitignore`d corpus directory being populated.

### Score format (later, steps 6–7)

Save as **MusicXML** for portability with MuseScore, Finale, Sibelius.
Also export **ABC notation** — it's the lingua franca of the Irish trad
community (thesession.org), and many users will paste tunes back and
forth as ABC.

## Engineering rules (non-negotiable)

These are project-specific lessons learned the hard way. Each one cost
us debugging time. Internalize them before writing new code.

### Rule 1: forward-declare C library types at *global* scope

When you forward-declare a C struct inside a C++ namespace:

```cpp
namespace fiddler::audio {
    struct AVFormatContext* ctx;  // WRONG
}
```

C++ treats this as declaring a brand-new struct
`fiddler::audio::AVFormatContext`, *not* a reference to the global
`::AVFormatContext` defined by libavformat. The compiler error you'll
see is "cannot convert `fiddler::audio::AVFormatContext*` to
`AVFormatContext*`" — the symptom, not the cause.

Always forward-declare C library types at global scope, before the
namespace opens:

```cpp
// At the top of Decoder.h, before any `namespace`:
struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

namespace fiddler::audio {
    AVFormatContext* ctx;  // refers to ::AVFormatContext, correct.
}
```

We hit this with FFmpeg first, then with PortAudio's
`PaStreamCallbackTimeInfo`. Both `Decoder.h` and `Player.h` now have an
explanatory comment block at the top documenting the rule. Any new
header that wraps a C library should follow the same pattern.

### Rule 2: nothing on the realtime audio callback

`Player::paCallback` runs on PortAudio's realtime thread. Inside it, you
must not:

- Allocate memory (`new`, `malloc`, `std::vector::push_back`, etc.)
- Take any lock (`std::mutex`, even briefly)
- Log (spdlog takes a mutex — explicitly forbidden by the logger docs)
- Call any I/O function

The contract is enforced by convention, not by the type system. The
callback only:

- Reads from the lock-free SPSC `RingBuffer` (`ring_->read(...)`)
- Increments `framesPlayed_` (an `std::atomic`)
- Pads with zeros on underrun

If you need diagnostics from the callback's neighbourhood, expose
counters as `std::atomic` and let the GUI timer or the decoder thread
log them periodically. There is a comment block at the top of
`Player::paCallback` reminding future contributors of this rule.

### Rule 3: prefer pkg-config over `find_package` on Ubuntu

Debian/Ubuntu maintainers strip CMake config files from `-dev` packages
when they conflict with distribution policy. The pkg-config file is the
canonical "Debian-supported" integration point.

Concrete example: `libspdlog-dev` ships `spdlog.pc` but no
`spdlogConfig.cmake`. So:

```cmake
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
target_link_libraries(target PRIVATE PkgConfig::SPDLOG)
```

works, while:

```cmake
find_package(spdlog REQUIRED)  # FAILS on Ubuntu
target_link_libraries(target PRIVATE spdlog::spdlog)
```

does not. We use pkg-config for FFmpeg, PortAudio, and spdlog. Reserve
`find_package` for libraries that genuinely don't ship a `.pc` file
(Catch2 and Qt6 are the current exceptions).

### Rule 4: every commit on `main` builds and tests pass

Bisect-friendly history matters more than fine-grained commits. If a
change spans multiple files where a partial application would break the
build, those files belong in **one** commit even if it's larger than
typical.

For solo work this is enforced by discipline. The mental model: imagine
`git bisect` later picking any commit at random; would it build? If no,
combine commits.

For experiments and WIP, use a feature branch and `git rebase -i` to
clean up before merging to `main`.

### Rule 5: include what you use, don't lean on transitive includes

`std::ofstream` needs `<fstream>` even if `<chrono>` happens to drag in
`<iosfwd>` and you can write `std::ofstream` as a name. The compiler
will accept the name (forward-declared) but reject the use (incomplete
type). IDEs that pre-index TUs hide this; a fresh build doesn't.

When you create an object of a standard-library type, include the
specific header that defines it. We've been bitten by this once already
(`<fstream>` in a test).

### Rule 6: hide library types behind facades

Two examples already in the codebase:

- `Decoder` exposes `AudioFormat`, `read()`, `seek()`. FFmpeg types
  appear nowhere in `Decoder.h`'s public surface.
- `util/Log.h` exposes `FLOG_*` macros and a `setCapture()` hook.
  spdlog types appear nowhere in `Log.h`.

When tests need to verify behaviour of a wrapped library, they go
through the facade's hooks (e.g. `log::setCapture()`), not around it.
A test that imports the wrapped library directly is a sign the facade
is missing an affordance — add it to the facade rather than reaching
past.

### Rule 7: log calls use fmt-style format strings, not printf

```cpp
FLOG_INFO("decoder", "opened {}, {} ms", path, duration.count());  // good
FLOG_INFO("decoder", "opened %s, %d ms", path, duration.count());  // wrong
```

The macros forward to `fmt::format`. printf-style `%s`/`%d` will not
compile.

### Rule 8: prefer the simple solution; elaborate only when forced to

Premature optimization is evil. When two designs both meet the current
requirements, take the smaller one. Reach for the more elaborate option
only when the simple version has demonstrably failed against a concrete
need — not in anticipation of a hypothetical future requirement.

If you're tempted to add machinery "in case we want it later", don't.
Code that isn't needed now has no test, no use site to validate the API,
and a guaranteed maintenance bill. Keep the door open by isolating the
simple solution behind a small interface, so swapping it later is a
localised change, not a rewrite.

We took this path on step 3's position tracking. The simple option —
anchor source position on each tempo change and seek; tolerate ≤2 s of
bounded drift while the ring buffer drains at the new ratio — lives
inside `Player::position()` and three atomics. If step 5's staff-cursor
sync proves the drift unacceptable, exact per-chunk-ratio bookkeeping
can replace just those atomics without touching the rest of the
pipeline.

## Threading model

```
+----------+    PCM    +-----------+   stretched   +-------------+
| Decoder  | --------> | Stretcher | ------------> | RingBuffer  |
| (worker) |           | (worker)  |               | (lock-free  |
+----------+           +-----------+               |  SPSC)      |
                                                   +------+------+
                                                          |
                                                          v
                                                 +-----------------+
                                                 | PortAudio       |
                                                 | callback (RT)   |
                                                 +-----------------+
```

Three threads of interest:

- **GUI thread** (Qt's main thread). Drives the `Player` API, polls
  `Player::position()` every 50 ms via `QTimer` to update the slider,
  and writes `targetTempoRatio_` when the tempo slider moves. Never
  touches FFmpeg or PortAudio directly — always goes through `Player`.
- **Decoder thread**, owned by `Player`. Sleeps in a 5 ms loop. Each
  iteration: (1) if the GUI moved the tempo slider, advance the
  position anchor under the *old* ratio and apply the new ratio to the
  stretcher; (2) drain the stretcher into the ring; (3) feed one chunk
  of decoded input to the stretcher. Holds `Player::mutex_` while
  touching `decoder_` and `stretcher_` so `seek()` can safely reset
  state.
- **PortAudio realtime callback thread**. Drains `ring_` only. Subject
  to Rule 2 (nothing else allowed).
- **Overview-builder workers** (one per file open, detached). Decode
  the entire file into a `WaveformOverview` and post the result back
  to the GUI thread via `QMetaObject::invokeMethod` with
  `Qt::QueuedConnection`. We *detach* (rather than store a joinable
  handle) so a slow build never freezes the GUI when the user opens
  the next file; an atomic generation counter on `MainWindow`,
  bumped per `loadFile()`, ensures only the most recent build's
  result is installed (a stale post from a still-running build for
  file A is dropped when its generation no longer matches). The
  post-back captures the receiver via `QPointer<MainWindow>` so the
  lambda no-ops if the window is destroyed before the queued event
  runs.

Synchronization primitives:

- `RingBuffer` — lock-free SPSC, `std::atomic` head/tail. Producer is
  the decoder thread, consumer is the audio callback.
- `Player::mutex_` — protects `decoder_`, `stretcher_`, and ring
  writes against concurrent seek from the GUI thread. The audio
  callback NEVER takes this mutex.
- `Player::framesPlayed_` — `std::atomic<int64_t>`, incremented by the
  callback, read by the GUI for `position()`.
- `Player::state_` — `std::atomic<TransportState>`, written by GUI
  thread, read by the GUI for play/pause UI state.
- `Player::decoderRunning_` — `std::atomic<bool>` for shutdown signal.
- `Player::targetTempoRatio_` — `std::atomic<double>`, written by the
  GUI when the slider moves, observed by the decoder thread.
- `Player::currentTempoRatio_`, `Player::anchorSourceMs_`,
  `Player::anchorOutFrames_` — atomic anchor for `position()` under
  changing tempo. Updated together by the decoder thread on tempo
  change and seek; `position()` reads them lock-free. The cost is
  bounded source-time drift (≤ 2 s, the ring's worth) while the ring
  drains output produced under an older ratio. See Rule 8 for the
  simple-first reasoning behind this choice over per-chunk-ratio
  bookkeeping.
- `MainWindow::overviewGeneration_` — `std::atomic<uint64_t>`,
  incremented on every `loadFile()`. Detached overview workers carry
  the generation that was active when they were spawned and drop
  their result if it no longer matches when the queued post-back
  reaches the GUI thread.
- `score::BarlineModel`, `score::MarkerModel`, `score::LoopModel`
  — all three owned by MainWindow, observed by the score widgets
  and the project viewer dock. All mutations happen on the GUI
  thread; no cross-thread access. Each model's `changed()` Qt
  signal is the one synchronisation point — observers connect
  and call `update()`, which hops naturally onto the GUI
  thread's paint queue.
- `MainWindow::placementHistory_` — `std::vector<PlacementKind>`,
  GUI-thread-only. Combined `Ctrl+Z` LIFO across barlines +
  markers + loops; pop dispatches to the matching model's
  `undoLastAdd`.
- `MainWindow::armedLoopId_` — `std::optional<int64_t>`,
  GUI-thread-only. Canonical "this loop is wrapping the
  transport" state; pushed to the dock via `setArmedLoopId` so
  the ▶ glyph + Arm checkbox stay in sync. Reset on Stop, file
  load, or the armed loop being removed from the model.
- `MainWindow::wrapPending_` — bool, GUI-thread-only. Suppresses
  re-entry into the wrap path while a pause-between-repeats
  `QTimer::singleShot` is in flight.

## Logging

The logging facade lives in `src/util/Log.{h,cpp}` and wraps spdlog so
the rest of the code never includes spdlog headers directly. All log
calls go through five macros — `FLOG_TRACE`, `FLOG_DEBUG`, `FLOG_INFO`,
`FLOG_WARN`, `FLOG_ERROR` — that take a category string and an
fmt-style format:

```cpp
FLOG_INFO("decoder", "opened {}, {} ms, {} Hz",
          path, duration.count(), sampleRate);
```

### Levels

| Level | Use for |
| --- | --- |
| TRACE | Per-iteration firehose (decoder thread refills, ring stats). Off by default. |
| DEBUG | Algorithmic milestones useful during development (seek targets, codec params, every gestural UI action). |
| INFO  | Lifecycle events (file loaded, app starting/stopping). |
| WARN  | Recoverable problems (missing Xing header, fallback paths). |
| ERROR | Operations that failed. |

Default threshold is **INFO** — quiet by default, but the startup
banner and lifecycle events are visible without any flags. Override
at runtime with `--log-level=debug` for verbose output, or set
`FIDDLER_LOG_LEVEL=trace` in the environment.

**Load-bearing UX rule:** if `--log-filter` is set explicitly and
no level is otherwise specified, the level is auto-promoted to
**Debug**. The user's intent in passing a filter is "I want to see
X"; if we left the level at Info they'd get only INFO+ lines and
miss every Debug call (which is where almost all gestural logging
lives), then quietly conclude the filter is broken. Two debugging
sessions burned exactly that hour before the auto-promote landed.

The effective config is printed to stderr unconditionally on
startup:
```
fiddler 0.1.0 | log level=debug filter='ui.*,score'
```
This is a plain `std::cerr` print rather than a log line, because
a narrow filter like `ui.*,score` would silently exclude an
"app"-tagged FLOG_INFO — and the banner would then be invisible
exactly when it's most useful.

### Categories

Hierarchical with `.` separators: `decoder`, `player`, `player.thread`,
`player.callback`, `ui`, `app`. Filter at runtime:

```bash
fiddler --log-level=trace --log-filter='player.*'
```

The filter glob currently supports a bare `*` (everything) and a
trailing `.*` (subtree match). Intentionally simpler than a regex.

### Sinks

Always: stderr, with colour. Optional: a rotating file at the path
given by `--log-file` (5 MiB per file, 3 backups kept).

### Capture hook (for tests)

`log::setCapture(fn)` installs a callback that receives every emitted
log line in addition to the regular sinks. Tests use this to assert
against emitted messages without including spdlog headers. Production
code never touches it. The hook is also potentially useful for plumbing
log output into a future "log viewer" widget.

### CLI summary

```
--log-level=LEVEL    trace|debug|info|warn|error|off
--log-filter=GLOB    e.g. 'player.*' or 'decoder' (default '*')
--log-file=PATH      also write to PATH, rotated
FIDDLER_LOG_LEVEL    env var, overridden by --log-level
```

## Code layout

```
fiddler/
├── CMakeLists.txt                  # top-level
├── docs/                           # this file, ADRs, design notes
├── scripts/                        # install-deps.sh, bootstrap.sh
├── src/
│   ├── main.cpp                    # CLI parsing + log init + Qt app
│   ├── ui/
│   │   ├── MainWindow.{h,cpp}        # transport + tempo + tune-type picker + dock
│   │   ├── WaveformWidget.{h,cpp}    # peaks + cursor + barline + marker overlays
│   │   ├── StaffWidget.{h,cpp}       # 5-line staff + time sig + barlines + markers
│   │   └── ProjectViewerDock.{h,cpp} # right-side artifact viewer / inspector
│   ├── audio/
│   │   ├── Decoder.{h,cpp}           # FFmpeg wrapper, opaque interface
│   │   ├── Stretcher.{h,cpp}         # Rubber Band wrapper (pImpl)
│   │   ├── WaveformOverview.{h,cpp}  # downsampled peak summary
│   │   ├── Player.{h,cpp}            # PortAudio + decoder thread
│   │   └── RingBuffer.h              # lock-free SPSC, header-only
│   ├── score/
│   │   ├── BarlineModel.{h,cpp}      # barlines + time signature
│   │   ├── LoopModel.{h,cpp}         # practice loops (paired ms ranges)
│   │   └── MarkerModel.{h,cpp}       # named cue points (stable IDs)
│   └── util/Log.{h,cpp}              # logging facade over spdlog
└── tests/
    ├── CMakeLists.txt              # Catch2 v3 + Qt6::Test
    ├── data/audio/                 # corpus dir, scanned at test time
    ├── qt_test_app.{h,cpp}         # QApplication + offscreen plugin
    └── test_*.cpp                  # one file per concern
```

`src/audio/` is plain C++, no Qt. `src/ui/` is Qt-specific. `src/util/`
is shared infrastructure. This separation is deliberate — the audio
engine could be reused in a CLI or a different GUI without touching it.

## Build & test

```bash
./scripts/install-deps.sh              # one-time apt install
./scripts/bootstrap.sh                 # cmake + ninja build
./build/src/fiddler                    # run
./build/src/fiddler --log-level=debug  # with logging
ctest --test-dir build --output-on-failure
```

Adding a new dependency requires three steps in lockstep:

1. Update `scripts/install-deps.sh`.
2. Update `.github/workflows/ci.yml`.
3. Re-run `./scripts/install-deps.sh` locally before the next build.

Skipping step 3 leads to confusing CMake "package not found" errors that
look like the build is broken when really apt just hasn't fetched the
new dep yet.

## Roadmap

Steps 0–2 are complete. Steps 3–7 are planned but not implemented.

- ✅ **Step 0** — Repository skeleton, build pipeline proven on the target stack.
- ✅ **Step 1** — Open and play any common audio format at normal speed (Qt UI, FFmpeg decode, PortAudio output, synchronised slider).
- ✅ **Step 2** — Structured logging via spdlog facade with CLI flags.
- ✅ **Step 3** — Real-time time-stretching via Rubber Band; tempo slider 25–100 % with no pitch change. `Stretcher` (R3 engine) sits between the decoder and the ring buffer on the decoder thread; `Player::setTempoRatio()` is the GUI-visible knob. Position tracking uses the simple anchor model (Rule 8).
- ✅ **Step 4** — Waveform view synchronised with playback position. `audio::WaveformOverview` (peak buckets keyed on source-time ms) is built on a detached worker thread on file open; `ui::WaveformWidget` paints peaks plus a cursor and emits `seekRequested(ms)` on click. Decoupled from `Player`; public `xToMs`/`msToX` so step 5's overlays plug in without restructuring.
- ✅ **Step 5** — Empty staff widget; user-placed barlines mapped to audio timestamps. `score::BarlineModel` is the shared data layer (descriptive `TimeSignature`, no uniformity enforcement — supports crooked tunes); `ui::StaffWidget` and `ui::WaveformWidget` both observe it. Tap-to-place is the primary gesture (`B`); `Ctrl+Z` undoes the last placement; `Del` removes the selected (auto-selected on tap) bar. Tradition-named time-signature picker; `QPainter` rather than `QGraphicsView` — see "Score widgets" above for the deferred features.
- ✅ **Step 5.5 (part A)** — Markers + project viewer dock. `score::MarkerModel` is the second project-artifact model (stable IDs, auto-named, repositionable, free-text rename). `ui::ProjectViewerDock` is the right-side `QDockWidget` that hosts the marker list and a property editor; double-click on a marker row "jump and plays". Tap-to-place is `M`; `Ctrl+Z` is now combined across barlines + markers; `Del` removes whichever is selected (mutually-exclusive selection).
- ✅ **Step 5.5 (part B)** — Practice loops. `score::LoopModel` is the third project-artifact model (paired `start_ms` + `end_ms`, stable IDs, per-loop pause-between-repeats default 500 ms). Two-anchor creation gesture: click first anchor, Ctrl+click second (any combination of barlines + markers, on any of the three surfaces — waveform, staff, dock), press `L` to create. Loops appear under a "Loops" category in the dock with a property page; double-click arms + jumps + plays, the Arm checkbox arms in place. Pressing Play with an armed loop seeks to startMs if the cursor is outside the loop. Wrap-around lives in the 50 ms GUI poll (jitter inaudible at practice tempos); Stop disarms.
- 🔜 **Step 6** — Bidirectional cursor between audio and staff; reference-tone synthesiser (sine/triangle) triggered by placing a note.
- 🔜 **Step 7** — MusicXML and ABC notation export.

## How we work

- **Small, motivated commits.** Each commit's message says *why*, not
  just *what*. The diff shows the *what*.
- **Test before commit.** Even if it's a one-line change, run `ctest`.
- **Acceptance criteria up front.** Before starting a step, write down
  what "done" looks like. The roadmap above includes acceptance hints
  for the upcoming steps.
- **Document decisions, not just code.** When a tradeoff is made between
  alternatives, record both choices and the reason. This file is the
  primary place for that.
