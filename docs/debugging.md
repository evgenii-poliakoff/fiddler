# Debugging UI bugs from logs

This is the working agreement for how UI bugs get reported, fixed,
and turned into regression tests. The short version:

> 1. Run fiddler with debug logging enabled, reproduce the bug,
>    attach the log to the bug report.
> 2. The log records every user action — the verb, the parameters,
>    and the resulting model state. We diagnose from the log alone.
> 3. Every fix lands with a regression test that replays the same
>    sequence the log captured.

The full rationale lives in `memory/feedback_logs_drive_tests.md`.

## Reproducing with logging

Use the CLI flags to widen the log threshold and tag the output with
a path:

```bash
./build/src/fiddler \
    --log-filter='ui.*,player,waveform,score' \
    --log-file=/tmp/fiddler.log
```

Note: passing `--log-filter` without `--log-level` auto-promotes
the level to `debug`. The user's intent in passing a filter is
"I want to see X"; the auto-promote prevents the silent-output
trap where the filter matches but every line is below the
threshold. Pass an explicit `--log-level=warn` (or whatever) if
you want a quieter view.

The startup banner is printed to stderr unconditionally:
```
fiddler 0.1.0 | log level=debug filter='ui.*,player,waveform,score'
```
That's the answer to "why am I not seeing the logs I expect" —
check the level and the filter as the runtime resolved them.

Then:

1. Reproduce the bug.
2. Close the window cleanly (so the `[ui.file] close` boundary is
   recorded — useful for log-replay).
3. Attach `/tmp/fiddler.log` to the bug report.

If the log is large, narrow `--log-filter` to the category you suspect
(`ui.score`, `ui.transport`, `ui.tempo`, `ui.file`) and re-run.
Filtering happens at emission time so this is cheap.

## What the log contains

Every user-driven action emits a single debug-level line in the
format `<verb> <params...> <state...>`. Categories:

| Category       | Action                                                  |
|----------------|---------------------------------------------------------|
| `ui.file`      | open success / failure / cancelled, close               |
| `ui.transport` | play, pause, stop, auto-pause, seek via position-slider, play-armed-seek, loop-wrap |
| `ui.tempo`     | tempo change (every applied step)                       |
| `ui.score`     | tap-place (barline), tap-marker, create-loop, undo-last (kind=barline\|marker\|loop), time-sig pick, seek via waveform / staff click, select / select-marker / select-loop / clear, secondary-anchor (set / cleared, via=waveform\|staff\|dock-ctrl-click\|dock-click), delete (widget key vs. window shortcut), marker-activated, loop-activated, loop-armed / loop-disarmed |

The `--log-filter` flag accepts a single glob or a comma-separated
list. Each entry is either a bare `*`, a trailing `.*` subtree
(`ui.*` matches `ui.file`, `ui.score`, …), or a literal exact
category name. Whitespace around commas is trimmed.

Below the UI layer, `[player]`, `[waveform]`, and `[score]` log lifecycle and
resource events that are useful when a UI symptom traces back through
the audio pipeline.

What is **not** in the log:

- Per-paint events.
- Per-mouse-move (we do log slider drags via `sliderMoved`, but only
  for the position slider where the drag *is* the user action).
- Per-tick position polling (those are at TRACE level, off by
  default).

## Example: a real session as logged

```
14:23:01 debug [ui.file]      open: path=/home/.../foo.mp3 duration=107702 ms audio=true
14:23:04 debug [ui.transport] play from=0 ms audio=true
14:23:08 debug [ui.tempo]     tempo=50% ratio=0.5000
14:23:12 debug [ui.transport] seek ms=12345 via=position-slider
14:23:13 debug [ui.score]     tap-place ms=12345 index=0 size=1 sel=0
14:23:14 debug [ui.score]     tap-place ms=14820 index=1 size=2 sel=1
14:23:15 debug [ui.score]     undo-last kind=barline bar-size=1 marker-size=0
14:23:16 debug [ui.score]     time-sig label=Single Jig numerator=6 denominator=8
14:23:18 debug [ui.score]     tap-marker ms=20100 id=1 size=1
14:23:20 debug [ui.score]     select-marker id=1 via=dock size=1
14:23:21 debug [ui.score]     marker-activated id=1 ms=20100 via=dock
14:23:22 debug [ui.transport] play from=20100 ms audio=true
14:23:30 debug [ui.score]     delete-marker id=1 via=window-shortcut size=0
14:23:32 debug [ui.transport] stop rewind=0
14:23:33 debug [ui.file]      close
```

A reader who has never met this user can reconstruct the session
from this fragment: which file, which tempo, where they tapped,
what they undid, which time signature they picked, etc.

## Translating a log into a regression test

When a fix lands, the test that locks it in should mirror the log
sequence one-for-one. Reading the log next to the test should look
like the same script twice.

Skeleton (Catch2 + Qt6::Test, against the in-memory WAV fixture):

```cpp
TEST_CASE("regression: <one-sentence summary of the bug>",
          "[main-window][gui][integration]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());

    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // ---- Replay the log here ----
    // Each line in the user's log fragment maps to one of:
    //
    //   [ui.transport] play from=…           → mouseClick(playButton)
    //   [ui.transport] pause at=…            → mouseClick(playButton)
    //   [ui.transport] stop rewind=0         → mouseClick(stopButton)
    //   [ui.transport] seek … via=position-slider
    //                                        → emit posSlider->sliderMoved(N)
    //   [ui.tempo] tempo=N%                  → tempoSlider->setValue(N)
    //   [ui.score] tap-place ms=…            → keyClick(window, Qt::Key_B)
    //                                          (after seeking to that ms)
    //   [ui.score] tap-marker ms=… id=…      → keyClick(window, Qt::Key_M)
    //                                          (after seeking to that ms)
    //   [ui.score] undo-last kind=…          → keyClick(window, Qt::Key_Z, Ctrl)
    //   [ui.score] time-sig label=… …        → tuneCombo->setCurrentIndex(idx);
    //                                          emit tuneCombo->activated(idx)
    //   [ui.score] seek … via=waveform-click → mouseClick(waveform, …)
    //   [ui.score] seek … via=staff-click    → mouseClick(staff, …)
    //   [ui.score] select-marker id=… via=dock
    //                                        → dock->setSelectedMarkerId(id)
    //                                          (or QTreeWidget::setCurrentItem
    //                                           on the matching row)
    //   [ui.score] marker-activated id=…     → emit dock->markerActivated(id)
    //                                          (simulates a double-click on
    //                                           the marker row)
    //   [ui.score] secondary-anchor ms=… via=waveform
    //                                        → mouseClick(waveform, …,
    //                                                     ControlModifier)
    //   [ui.score] secondary-anchor ms=… via=dock-ctrl-click
    //                                        → emit dock->loopAnchorAddRequested()
    //                                          (or QTest::mouseClick on the
    //                                           tree viewport with Ctrl)
    //   [ui.score] create-loop start=… end=… → setSecondaryAnchorMs(ms);
    //                                          keyClick(window, Qt::Key_L)
    //   [ui.score] loop-activated id=…       → emit dock->loopActivated(id)
    //   [ui.score] loop-armed id=… via=checkbox
    //                                        → emit dock->loopArmToggleRequested(id, true)
    //   [ui.transport] loop-wrap id=… …      → cursor crossed endMs while armed;
    //                                          drive via posSlider beyond endMs
    //                                          and let updatePosition fire
    //   [ui.transport] play-armed-seek id=… to=…
    //                                        → mouseClick(playButton) with
    //                                          armedLoopId set + cursor outside loop
    //   [ui.score] delete … via=window-shortcut
    //                                        → keyClick(window, Qt::Key_Delete)
    //   [ui.score] delete-marker … via=window-shortcut
    //                                        → keyClick(window, Qt::Key_Delete)
    //                                          (with a marker selected)
    //   [ui.score] delete-loop … via=…       → keyClick(window, Qt::Key_Delete)
    //                                          (with a loop selected)
    //   [ui.file] close                      → window->close()
    //
    // Then assert the bug-fix invariant.
}
```

The objectName tags on the testable widgets (`playButton`,
`stopButton`, `positionSlider`, `tempoSlider`, `waveformWidget`,
`staffWidget`, `tuneTypeCombo`) are stable so tests can find each
control without reaching into private state.

## Format-pinning tests

`tests/test_event_logging.cpp` pins the log-line format itself —
each verb has a test that fires the action and asserts the captured
log contains the load-bearing fields. If you change a log message's
wording, search that test file for the substring you're changing and
adjust the matching test in lockstep. The format is treated as a
public contract, not an implementation detail.

## Future work

A small "log replayer" test helper that ingests a log fragment and
emits the corresponding `QTest` events automatically. Until that
lands, translating log → test is a manual but mechanical job — the
table above is its informal grammar.
