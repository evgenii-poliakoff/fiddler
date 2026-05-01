# Fiddler

A desktop transcription aid for self-taught Irish fiddle players.

Open an audio recording, slow it down without changing pitch, and transcribe
the tune onto a synchronised musical staff — clicking a bar in the staff
seeks the audio, scrubbing the audio moves the staff cursor. Place a note
on the staff to hear a reference tone for pitch-matching.

**Status:** step 5 done — open audio, play with pitch-preserving tempo
control 25–100 %, see and seek through a synchronised waveform, place
barlines on a staff by tapping along to the recording.

## Roadmap

- ✅ **Step 0** — Repository skeleton.
- ✅ **Step 1** — Open and play any common audio format at normal speed (Qt UI, FFmpeg decode, PortAudio output).
- ✅ **Step 2** — Structured logging via spdlog facade with CLI flags.
- ✅ **Step 3** — Real-time time-stretching via Rubber Band; tempo slider 25–100 % with no pitch change.
- ✅ **Step 4** — Waveform view synchronised with playback position; click to seek.
- ✅ **Step 5** — Empty staff widget + user-placed barlines + tradition-named time-signature picker. Tap `B` to place at the playback position; `Ctrl+Z` undoes; `Del` removes.
- 🔜 **Step 6** — Bidirectional cursor between audio and staff; reference-tone synthesiser (sine/triangle) triggered by placing a note.
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

```bash
./scripts/bootstrap.sh           # configure + build in ./build
./build/src/fiddler              # run
ctest --test-dir build --output-on-failure   # tests
```

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
├── docs/                       # architecture notes, ADRs
├── scripts/                    # dev helper scripts
├── src/
│   ├── main.cpp
│   ├── ui/                     # Qt widgets (main window, waveform, staff)
│   ├── audio/                  # decoder, stretcher, overview, ring buffer, player
│   └── score/                  # barlines + time signature
└── tests/                      # Catch2 + Qt6::Test
```

## Logging

Default threshold is **WARN** (silent unless something's wrong). For
debugging during development:

```bash
./build/src/fiddler --log-level=debug
./build/src/fiddler --log-level=trace --log-filter='player.*'
./build/src/fiddler --log-level=info --log-file=/tmp/fiddler.log
FIDDLER_LOG_LEVEL=trace ./build/src/fiddler
```

See [`docs/architecture.md`](docs/architecture.md#logging) for levels,
categories, and the realtime-callback rule.

## Reporting a UI bug

If something goes wrong in the UI, please re-run with the structured
event log enabled and attach the resulting file:

```bash
./build/src/fiddler \
    --log-level=debug \
    --log-filter='ui.*,player,waveform,score' \
    --log-file=/tmp/fiddler.log
```

Every user-driven action (file open, play, pause, seek, tempo change,
tap-to-place, undo, select, delete, time-signature pick, close) emits
one line capturing the verb, the parameters, and the resulting model
state. The full workflow — including how each fix lands as a
regression test that replays the same log sequence — is in
[`docs/debugging.md`](docs/debugging.md).

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
