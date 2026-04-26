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
