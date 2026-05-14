# Fiddler

A desktop transcription aid for self-taught Irish fiddle players.

Open an audio recording, slow it down without changing pitch, and transcribe
the tune onto a synchronised musical staff — clicking a bar in the staff
seeks the audio, scrubbing the audio moves the staff cursor. Place a note
on the staff to hear a reference tone for pitch-matching.

![The Fiddler main window — a recording open with markers, barlines, an armed practice loop, and the project viewer dock on the right.](docs/img/manual-overview.png)

## User manual

The [**user manual**](docs/user-manual.md) walks through opening audio,
slowing it down, marking phrases, building practice loops, and using
the pre-roll countdown. Start there if you're new to Fiddler.

**Status:** step 6.3 done — open audio, play with pitch-preserving
tempo control 25–100 %, see and seek through a synchronised
waveform, place barlines and named markers on a staff by tapping
along to the recording, build practice loops from a drag on the
waveform (or from any two anchors), drag-move / resize / drag-create
loops and notes on the chromatic piano roll, snap edges to nearby
barlines / markers / loop edges, edit any artifact from the
right-side project viewer dock, and audition pitches with a
built-in reference-tone synth (click a piano key, or hover-tone in
Continuous / On-tap mode) for ear-matching against the recording.
Toggle the **Pre-roll** checkbox in the transport row to switch
between passive listening (no silence) and practice mode, where
every Play press inserts a ready-set-go countdown of the chosen
ms before audio starts.

## Roadmap

- ✅ **Step 0** — Repository skeleton.
- ✅ **Step 1** — Open and play any common audio format at normal speed (Qt UI, FFmpeg decode, PortAudio output).
- ✅ **Step 2** — Structured logging via spdlog facade with CLI flags.
- ✅ **Step 3** — Real-time time-stretching via Rubber Band; tempo slider 25–100 % with no pitch change.
- ✅ **Step 4** — Waveform view synchronised with playback position; click to seek.
- ✅ **Step 5** — Empty staff widget + user-placed barlines + tradition-named time-signature picker. Tap `B` to place at the playback position; `Ctrl+Z` undoes; `Del` removes.
- ✅ **Step 5.5 (part A)** — Markers + project viewer dock. Tap `M` to drop a named cue point at the playback position; the dock on the right lists all markers, lets you rename / re-position them, and double-clicking a row jumps the player there and starts playback.
- ✅ **Step 5.5 (part B)** — Practice loops. Click any artifact to mark a primary anchor, Ctrl+click any second artifact to add a secondary, press `L` to create a loop spanning them. Loops appear under a "Loops" category in the dock with a property page (Name / Start / End / Pause / Armed). Double-clicking a loop arms + jumps + plays; the Arm checkbox arms in place; pressing the main Play button while a loop is armed seeks to the loop's start if you're outside it.
- ✅ **Step 6.1** — `NoteModel` + chromatic piano-roll rendering on the staff. Notes are stored as (start_ms, end_ms, midi) intervals; the staff renders horizontal bars on per-semitone rows over the violin range (G3 – E7).
- ✅ **Step 6.2** — Click-to-place + drag gestures for notes. Click a row to place a note starting at the click (default 400 ms); drag the body to move; drag an edge to resize; drag on empty grid to draw with explicit length. Same-row overlap guard prevents accidental duplicates.
- ✅ **Step 6.2.2** — Loop gestures on the waveform. Drag the body of a loop band to translate; drag on empty waveform to draw a new loop. Snap-to-anchor on barlines / markers / other loop edges.
- ✅ **Step 6.3** — Reference-tone synthesizer. Click a piano key on the staff to fire a pulse; hover-tone in Continuous (tone follows mouse) or On-tap (T fires a pulse) mode. Sine + Triangle waveforms; runs as its own audio stream alongside the recording. The layered architecture (`Voice` → `SoundSource` → `Oscillator`) is ready for expressive playback expansion: ornaments, vibrato, real fiddle samples.
- 🔜 **Step 6.4** — Bidirectional playback cursor. Current note highlights as playback crosses it; optional onset-tone pulse mixes the transcription against the audio.
- 🔜 **Step 7** — MusicXML and ABC notation export.

See [`docs/architecture.md`](docs/architecture.md) for stack-choice rationale.

## Prerequisites (Ubuntu 24.04)

```bash
./scripts/install-deps.sh
```

Or, equivalently:

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake ninja-build pkg-config git \
    qt6-base-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
    portaudio19-dev \
    librubberband-dev \
    catch2
```

## Build

All routine operations are wrapped in `./scripts/*.sh`. Humans, CI, and
Claude/agents use the same scripts so invocations don't drift across
contexts.

```bash
./scripts/install-deps.sh        # apt deps (Ubuntu 24.04)
./scripts/bootstrap.sh           # configure + build in ./build
./build/src/fiddler              # run the app
./scripts/test.sh                # run ctest
./scripts/screenshots.sh         # regenerate docs/img/*.png from the running app
```

Each script accepts `BUILD_DIR=<dir>` / `BUILD_TYPE=<Debug|Release>` env
overrides. `test.sh` and `screenshots.sh` forward extra args through
(`./scripts/test.sh -R 'WaveformWidget'`).

Or manually:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Test corpus

`tests/data/audio/` is scanned at test time. Drop any number of files
(`.wav`, `.flac`, `.mp3`, `.ogg`, `.m4a`, `.aac`, `.opus`, `.aiff`)
into it and the corpus test will open and fully decode each one.
The directory is `.gitignore`d for audio extensions so files stay
local; use `git add -f` if you ever want to commit a small fixture.
If the directory is empty, the corpus test prints a warning and passes
(a synthetic sine-wave test still exercises the decoder unconditionally).

## Layout

```
fiddler/
├── CMakeLists.txt              # top-level
├── docs/                       # architecture notes, debugging guide, ADRs
├── scripts/                    # dev helper scripts
├── src/
│   ├── main.cpp
│   ├── ui/                     # Qt widgets (main window, waveform, staff, project dock)
│   ├── audio/                  # decoder, stretcher, overview, ring buffer, player
│   └── score/                  # barlines + time signature, markers
└── tests/                      # Catch2 + Qt6::Test
```

## Logging

Default threshold is **INFO** — quiet by default, but the startup
banner and lifecycle events are visible without any flags. For
debugging during development:

```bash
./build/src/fiddler --log-filter='ui.*,score'    # auto-promotes level to debug
./build/src/fiddler --log-level=debug
./build/src/fiddler --log-level=trace --log-filter='player.*'
./build/src/fiddler --log-file=/tmp/fiddler.log
FIDDLER_LOG_LEVEL=trace ./build/src/fiddler
```

The effective config is printed to stderr unconditionally on
startup so you always know what's loaded:
```
fiddler 0.1.0 | log level=debug filter='ui.*,score'
```

See [`docs/architecture.md`](docs/architecture.md#logging) for levels,
categories, and the realtime-callback rule.

## Reporting a UI bug

If something goes wrong in the UI, please re-run with the structured
event log enabled and attach the resulting file:

```bash
./build/src/fiddler \
    --log-filter='ui.*,player,waveform,score' \
    --log-file=/tmp/fiddler.log
```

Every user-driven action (file open, play, pause, seek, tempo change,
tap-to-place, secondary-anchor, create-loop, loop-armed,
loop-wrap, undo, select, delete, time-signature pick, close) emits
one line capturing the verb, the parameters, and the resulting
model state. The full workflow — including how each fix lands as a
regression test that replays the same log sequence — is in
[`docs/debugging.md`](docs/debugging.md).

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
