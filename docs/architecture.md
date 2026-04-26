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

### Time stretching: Rubber Band Library (arrives in step 3)

State-of-the-art open option. Phase-vocoder engine with formant
preservation; default in Ardour and Mixxx. Real-time mode
(`OptionProcessRealTime`) is suited to interactive tempo changes during
playback. License: GPL or commercial. We adopt GPL-3.0 in step 0;
switch to a commercial Rubber Band licence if a closed-source build is
ever needed. SoundTouch (WSOLA, LGPL) is the lighter fallback, but
quality drops below ~75 % tempo, which is exactly where fiddlers want
to listen.

### Logging: spdlog (behind a facade)

The application code never includes spdlog headers directly. All log
calls go through `src/util/Log.h`'s `FLOG_*` macros. The facade exists
so we can swap loggers without touching call sites. spdlog was picked
over `std::format`-from-scratch (more code) and Qt's `QLoggingCategory`
(awkward in non-Qt audio code).

### Test framework: Catch2 v3

Available in 24.04 as `catch2`. Header-light, low ceremony, expressive
`REQUIRE`/`SECTION` syntax.

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

## Threading model

```
+----------+    decoded float32     +-------------+
| Decoder  | ---------------------> | RingBuffer  |
| (worker) |     (later via         | (lock-free  |
+----------+      RubberBand)       |  SPSC)      |
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
  `Player::position()` every 50 ms via `QTimer` to update the slider.
  Never touches FFmpeg or PortAudio directly — always goes through
  `Player`.
- **Decoder thread**, owned by `Player`. Sleeps in a 5 ms loop watching
  `ring_->writeAvailable()`; tops up the ring when it has space. Holds
  `Player::mutex_` while reading from the decoder so seek() can safely
  reset state.
- **PortAudio realtime callback thread**. Drains `ring_` only. Subject
  to Rule 2 (nothing else allowed).

Synchronization primitives:

- `RingBuffer` — lock-free SPSC, `std::atomic` head/tail. Producer is
  the decoder thread, consumer is the audio callback.
- `Player::mutex_` — protects `decoder_` and ring writes against
  concurrent seek from the GUI thread. The audio callback NEVER takes
  this mutex.
- `Player::framesPlayed_` — `std::atomic<int64_t>`, incremented by the
  callback, read by the GUI for `position()`.
- `Player::state_` — `std::atomic<TransportState>`, written by GUI
  thread, read by the GUI for play/pause UI state.
- `Player::decoderRunning_` — `std::atomic<bool>` for shutdown signal.

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
| DEBUG | Algorithmic milestones useful during development (seek targets, codec params). |
| INFO  | Lifecycle events (file loaded, app starting/stopping). |
| WARN  | Recoverable problems (missing Xing header, fallback paths). |
| ERROR | Operations that failed. |

Default threshold is **WARN** — silent unless something's wrong. Override
at runtime with `--log-level=debug` or `FIDDLER_LOG_LEVEL=trace`.

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
├── CMakeLists.txt              # top-level
├── cmake/                      # FindXxx.cmake helpers
├── docs/                       # this file, ADRs, design notes
├── scripts/                    # install-deps.sh, bootstrap.sh
├── src/
│   ├── main.cpp                # CLI parsing + log init + Qt app
│   ├── ui/MainWindow.{h,cpp}   # Qt main window, transport controls
│   ├── audio/
│   │   ├── Decoder.{h,cpp}     # FFmpeg wrapper, opaque interface
│   │   ├── Player.{h,cpp}      # PortAudio + decoder thread
│   │   └── RingBuffer.h        # lock-free SPSC, header-only
│   └── util/Log.{h,cpp}        # logging facade over spdlog
└── tests/
    ├── CMakeLists.txt          # Catch2 v3
    ├── data/audio/             # corpus dir, scanned at test time
    └── test_*.cpp              # one file per concern
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
- 🔜 **Step 3** — Real-time time-stretching via Rubber Band; tempo slider 50–100 % with no pitch change. Insert `RubberBandStretcher` between the decoder and the ring buffer; expose a tempo-ratio property on `Player`. Acceptance: dragging the slider changes playback speed audibly while pitch stays constant; no underruns at 50 %.
- 🔜 **Step 4** — Waveform view synchronised with playback position. Decode-on-load into a downsampled overview buffer; render with `QPainter`.
- 🔜 **Step 5** — Empty staff widget; user sets time signature and clicks to place barlines mapped to audio timestamps. `QGraphicsView`-based.
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
