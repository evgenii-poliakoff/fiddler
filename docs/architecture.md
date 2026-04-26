# Architecture & stack decisions

Short ADR-style notes on why each library was chosen. Update when revisiting.

## GUI: Qt 6 (Widgets)

- First-class on Ubuntu, ships in 24.04 as `qt6-base-dev`.
- `QPainter` and `QGraphicsView` are well-suited to the staff editor
  in steps 4–5.
- Considered: GTK 4 (less ergonomic from C++), Dear ImGui (great for
  prototyping, weaker for a polished score editor), Slint (immature
  for this use case).

## Audio decoding: FFmpeg (`libavformat` + `libavcodec`)

- One API covers WAV, FLAC, MP3, OGG/Vorbis, M4A/AAC, OPUS, and more.
- The alternative — libsndfile + a separate MP3 decoder (mpg123) —
  has simpler ergonomics but needs format-specific glue.
- We hide all FFmpeg types behind `Decoder` so the rest of the app is
  insulated from FFmpeg ABI churn.

## Audio playback: PortAudio

- Direct buffer-callback model is what we need for time-stretching:
  the callback pulls from a ring buffer fed by Rubber Band.
- Qt Multimedia abstracts too much for this and is harder to hook the
  stretcher into.
- RtAudio is a viable alternative with similar ergonomics.

## Time stretching: Rubber Band Library (step 2)

- State-of-the-art open option. Phase-vocoder engine with formant
  preservation; default in Ardour and Mixxx.
- Real-time mode (`OptionProcessRealTime`) is suited to interactive
  tempo changes during playback.
- License: GPL or commercial. We adopt GPL-3.0 in step 0; switch to a
  commercial Rubber Band licence if a closed-source build is ever
  needed.
- SoundTouch (WSOLA, LGPL) is the lighter fallback, but quality drops
  below ~75 % tempo, which is exactly where fiddlers want to listen.

## Test framework: Catch2 v3

- Available in 24.04 as `catch2`.
- Header-light, low ceremony, expressive `REQUIRE`/`SECTION` syntax.

## Score format (later)

- Save as **MusicXML** for portability with MuseScore, Finale, Sibelius.
- Also export **ABC notation** — it's the lingua franca of the Irish
  trad community (thesession.org), and many users will paste tunes
  back and forth as ABC.

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

Rules:

- The PortAudio callback thread allocates nothing, locks nothing, and
  only touches the ring buffer. This is a hard contract.
- The Qt GUI thread never touches FFmpeg or PortAudio directly — it
  drives the `Player` API, which marshals work onto the decoder
  thread.
- Transport state lives in `std::atomic` so the GUI can read it from
  a timer for the position slider without locking.

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
trailing `.*` (subtree match). It's intentionally simpler than a regex.

### Hard rule: no logging from the PortAudio callback

The callback runs on a realtime thread; spdlog takes a mutex. **Never**
add `FLOG_*` calls to `Player::paCallback`. If you need per-callback
diagnostics, stash counters in `std::atomic` and let the decoder thread
or the GUI timer log them periodically.

### Sinks

Always: stderr, with colour. Optional: a rotating file at the path
given by `--log-file` (5 MiB per file, 3 backups kept).

### CLI summary

```
--log-level=LEVEL    trace|debug|info|warn|error|off
--log-filter=GLOB    e.g. 'player.*' or 'decoder' (default '*')
--log-file=PATH      also write to PATH, rotated
FIDDLER_LOG_LEVEL    env var, overridden by --log-level
```
