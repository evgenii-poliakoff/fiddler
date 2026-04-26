# Fiddler

A desktop transcription aid for self-taught Irish fiddle players.

Open an audio recording, slow it down without changing pitch, and transcribe
the tune onto a synchronised musical staff — clicking a bar in the staff
seeks the audio, scrubbing the audio moves the staff cursor. Place a note
on the staff to hear a reference tone for pitch-matching.

**Status:** step 0 — repository skeleton. Nothing functional yet.

## Roadmap

1. **Step 1** — Open and play any common audio format at normal speed (Qt UI, FFmpeg decode, PortAudio output).
2. **Step 2** — Real-time time-stretching via Rubber Band; tempo slider 50–100 % with no pitch change.
3. **Step 3** — Waveform view synchronised with playback position.
4. **Step 4** — Empty staff widget; user sets time signature and clicks to place barlines mapped to audio timestamps.
5. **Step 5** — Bidirectional cursor between audio and staff.
6. **Step 6** — Reference-tone synthesiser (sine/triangle) triggered by placing a note.
7. **Step 7** — MusicXML and ABC notation export.

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
├── cmake/                      # FindXxx.cmake helpers
├── docs/                       # architecture notes, ADRs
├── scripts/                    # dev helper scripts
├── src/
│   ├── main.cpp
│   ├── ui/                     # Qt widgets
│   └── audio/                  # decoder, ring buffer, player
└── tests/                      # Catch2 unit tests
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

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
