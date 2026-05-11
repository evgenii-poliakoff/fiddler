# Fiddler User Manual

## 1. Welcome

*Fiddler* is a transcription aid for self-taught fiddle players, designed for learning tunes by ear from recordings. Open an audio file, slow it down without changing pitch, navigate by clicking the waveform, and build practice loops with a pre-roll countdown that gives you time to switch from the mouse to the instrument before each take.

This manual covers the version of Fiddler in your hands. The chapters that follow walk through opening audio, controlling playback, marking phrases, building loops, and reporting bugs.

![The Fiddler main window — a recording open with the project viewer dock on the right.](img/manual-overview.png)

## 2. First five minutes

This chapter walks through opening a recording, slowing it down, navigating the waveform, and placing a marker so you can return to a phrase. The whole sequence takes about five minutes from a cold start.

1. **Open an audio file.** Choose **File ▸ Open** and pick any recording. Fiddler accepts MP3, FLAC, WAV, OGG, M4A, AAC, Opus, and AIFF.

2. **Drop the tempo to 70 %.** Drag the **Tempo** slider at the bottom of the window until the label reads *Tempo: 70 %*. Pitch is preserved; only the playback rate changes.

3. **Press Play.** Audio begins from the start of the recording. The vertical red cursor on the waveform tracks the current playback position.

4. **Click anywhere on the waveform to seek.** Playback jumps to where you clicked.

5. **Press M to drop a marker.** A labelled flag appears above the waveform at the playback position. The marker is named automatically and shows up in the project viewer dock on the right.

6. **Double-click the marker's row in the dock.** Playback jumps back to the marker, ready for another listen.

7. **Press Ctrl+S to save.** A dialog asks where to save the *Fiddler project* file — a `.fdlp` sidecar alongside the audio. Future opens of that `.fdlp` restore the session, marker and all.

You now have the core navigation and bookmarking gestures. The chapters that follow add tempo control, barline tapping, practice loops, and the pre-roll countdown.

## 3. The main window

The Fiddler window is divided into four working areas, stacked vertically with a side panel on the right.

![The main window with each region labelled.](img/manual-overview.png)

- *Transport row.* Play and Stop buttons, the *Tune type* picker, and the *Pre-roll* checkbox and spinbox. The transport row stays at the top so the controls are always reachable.
- *Waveform.* A summary view of the loaded recording. Click anywhere along the waveform to seek; the red cursor tracks the current playback position.
- *Staff.* A musical staff with the time signature on the left. Barlines you place with **B** appear here, lined up with the same source-time as the waveform above.
- *Position and tempo sliders.* The position slider mirrors the playback cursor and accepts dragging for precise seeks. The tempo slider sets playback speed from 25 % to 100 %.
- *Project viewer dock.* The right-hand panel lists every marker and loop in the recording, with a property page below for renaming and re-positioning. Press **F4** to hide or show the dock.

## 4. Playing back audio

Fiddler reads any common audio format and plays it back at a tempo of your choosing without changing the pitch. The transport row across the top owns playback control.

![The transport row.](img/manual-transport-row.png)

- *Play / Stop.* Press the **Play** button or the spacebar to start playback; press it again to pause. **Stop** rewinds to the beginning of the recording.
- *Tune type.* A drop-down with tradition-named time signatures (Reel, Jig, Hornpipe, and so on). Picking one updates the staff's time signature on the left of the music staff.
- *Pre-roll.* A checkbox and a duration spinbox. The pre-roll plays a short silence before each take so you have time to ready the bow. Chapter 7 covers it in detail.

### Tempo

Drag the **Tempo** slider to play back at 25 % to 100 % of the original speed. Pitch is preserved at every tempo. The slider lives at the bottom of the window so it stays close to the position slider you'll use alongside it.

![The tempo slider at 70 %.](img/manual-tempo-slider.png)

Note that the tempo change is real-time: there is no rebuild of the recording when you move the slider.

### Seeking

Click anywhere on the waveform to jump playback to that point. The position slider beneath the waveform is the same control surface in linear form: drag the handle for fine adjustments where the waveform is dense.

![The position slider mid-track.](img/manual-position-slider.png)

## 5. Barlines

A *barline* is a vertical line on the music staff that divides the recording into bars. You place barlines as you listen, by tapping a key in time with the music. Each barline is anchored to a source-time position; the staff renders them in order.

### Tap to place

Press **B** during playback to drop a barline at the current playback position. The barline appears on the staff immediately, and the cursor keeps moving so you can tap the next one as the bar arrives.

![The staff with three tapped barlines.](img/manual-staff-barlines.png)

Tap-to-place is the primary gesture for adding barlines. Clicking on the waveform or the staff *seeks* — it does not place. Keep a hand on the keyboard while listening so you can tap **B** without breaking flow.

### Adjust a barline

Drag a placed barline to nudge its position. Press it on the waveform or the staff and drag horizontally; release to commit. The cursor follows the barline as you drag, so you hear where you're moving it to.

To delete a barline, click it once to select, then press **Delete**. To take back the most recent edit — a placement, a drag, a position change in the dock, a delete, or a rename — press **Ctrl+Z**.

### Tune type and time signature

The **Tune type** picker on the transport row sets the time signature shown on the staff. The presets are tradition-named — *Reel (4/4)*, *Jig (6/8)*, *Slip Jig (9/8)*, and so on — so you pick the dance form rather than the bare numerator and denominator.

![The Tune type picker open.](img/manual-tune-picker.png)

Note that Fiddler does not enforce a uniform bar length. Crooked tunes (with bars that vary in length) place freely; the time signature describes the prevailing shape rather than constraining each bar.

## 6. Markers

A *marker* is a labelled cue point in the recording. Use markers to bookmark a phrase, an ornament, or a hard passage you want to return to. Markers appear as flags above the waveform and as rows in the project viewer dock.

![A marker tick and its label flag.](img/manual-marker-flag.png)

### Place a marker

Press **M** during playback to drop a marker at the current playback position. The marker is auto-named *Mark 1*, *Mark 2*, and so on, and shows up in the *Markers* category of the dock immediately.

![The Markers category of the dock with three markers placed; the second is selected and its property page is open below.](img/manual-dock-markers.png)

### Rename and re-position

Click a marker — on the waveform, on the staff, or in the dock — to select it. The dock's property page below the tree shows the marker's name and its position in milliseconds.

- *Name.* Edit the field and press Enter. The new name shows in the dock and on the marker's flag.
- *Position.* Edit the millisecond value and press Enter, or drag the marker tick on the waveform or the staff for finer control while listening.

### Jump and play

Double-click a marker's row in the dock to jump playback to that marker and start playing. If the pre-roll is on, you'll hear the countdown before audio resumes.

### Markers vs barlines

Markers and barlines look similar in the data model — each is anchored to a source-time position — but they serve different purposes. Use *barlines* to mark the regular pulse of the music; use *markers* to label specific moments you want to revisit. A barline says "this is where bar 3 starts"; a marker says "this is the awkward triplet."

## 7. Practice loops

A *loop* is a region of the recording that plays back repeatedly. Use loops to drill a phrase: arm the loop, press Play, and the recording wraps from the end back to the start of the loop until you stop. Combine the loop with the pre-roll and Fiddler gives you ready-set-go time before each take.

![A loop band on the waveform.](img/manual-loop-band.png)

### Build a loop

A loop is built from two anchors — a *primary* anchor and a *secondary* anchor. Each anchor can be any artifact already in the recording: a barline, a marker, or another loop's edge.

1. Click an artifact to make it the primary anchor.
2. Hold **Ctrl** and click another artifact to attach it as the secondary anchor.
3. Press **L**.

A new loop spans the source-time range from the smaller of the two anchor positions to the larger. The loop appears as a translucent band across the waveform and the staff, and as a row in the *Loops* category of the dock.

### Arm and play

A loop is *armed* when its row in the dock has a play-arrow glyph and the **Armed** checkbox on the property page is checked. Only one loop is armed at a time.

- Double-click the loop's row in the dock to arm it, jump to its start, and play.
- Or click the loop's row to select it, then check **Armed** on the property page to arm it without seeking.

While a loop is armed, the recording wraps from the loop's end back to its start each time playback reaches the end. **Stop** disarms; pressing **Play** with no loop armed plays through normally.

### Adjust a loop's edges

Drag either edge of a loop band to fine-tune its boundaries. Loop edges *snap* to nearby barlines and markers when you drag within a few pixels of one, so you can pin an edge to a known anchor without typing a position.

![A loop edge snapping to a barline mid-drag.](img/manual-loop-edge-snap.png)

You can also edit the loop's start and end positions in the dock's property page, in milliseconds.

### Pre-roll

The *pre-roll* is a short silence inserted before audio resumes, so you have time to lift the bow and prepare. The transport row's **Pre-roll** checkbox switches it on; the spinbox sets the duration in milliseconds.

![The Pre-roll checkbox and spinbox.](img/manual-preroll-group.png)

When the pre-roll is on, a circular countdown widget appears at the bottom of the project viewer dock. Each tick around the ring represents a fraction of the pre-roll; the ring depletes during the silence and refills when audio starts.

![The pre-roll countdown ring mid-tick.](img/manual-preroll-countdown.png)

The pre-roll runs whenever playback resumes from Paused or Stopped — when you press Play, double-click a marker, or cancel and resume. Armed loops are the exception: they always play a pre-roll between repeats when pre-roll is enabled, even when playback is already running. This gives you the same ready-set-go cadence between every take.

## 8. The project viewer dock

The dock on the right of the window lists every marker and loop in the recording, with a property page below the tree for editing the selected artifact.

![The project viewer dock with markers and loops expanded.](img/manual-dock-detail.png)

- *Tree.* Markers and Loops as two collapsible categories. Click a row to select; double-click a row to jump and play.
- *Property page.* Shows the fields of the currently-selected artifact. Markers expose Name and Position; loops expose Name, Start, End, and the Armed checkbox.
- *Pre-roll countdown.* When the pre-roll is enabled, the countdown ring appears at the bottom of the dock and animates during each pre-roll silence.

Press **F4** to hide or show the dock. The dock's visibility persists across sessions, so a hidden dock stays hidden until you press **F4** again.

## 9. Saving, opening, and undo

### Save and reopen

A *Fiddler project* is the audio file path plus everything you've placed on top of it — barlines, markers, loops, the chosen tune type, and the pre-roll setting. Projects are saved as `.fdlp` files alongside the audio.

- **File ▸ Save** (**Ctrl+S**) — saves the project. If the session hasn't been saved before, this opens the Save As dialog.
- **File ▸ Save As…** (**Ctrl+Shift+S**) — picks a new path. Default suggestion is the audio file's name with a `.fdlp` extension.
- **File ▸ Open…** (**Ctrl+O**) — opens an audio file *or* a `.fdlp` project. The file dialog filter shows both kinds.

### Open Recent

**File ▸ Open Recent** lists the ten most-recently-opened files — both audio and `.fdlp` projects — in most-recent-first order. Picking an entry opens it the same way the file dialog would; the entry then jumps to the front of the list. If the file has moved or been deleted since you last opened it, you'll see a *File not found* dialog and the entry drops off the list automatically.

The submenu is dimmed when nothing has been opened yet. **Clear Recent Files** at the bottom empties the list.

When a project is loaded, the window title shows its filename: *Fiddler — tune.fdlp*. An asterisk in front of the title — *\* Fiddler — tune.fdlp* — means the session has unsaved changes. Save with **Ctrl+S** to clear the asterisk.

Note that closing the window with unsaved changes prompts you to save first; pick *Discard* to lose the changes, *Cancel* to keep the window open.

### Undo

Press **Ctrl+Z** to reverse the most recent edit. The undo history covers every kind of action across barlines, markers, and loops, in the order you took them: placements, drags, dock spinbox edits, renames, and deletes. Pre-roll changes — both the duration spinbox and the enable checkbox — are covered too. A deleted artifact returns with the same name and the same position it had before.

Repeated **Ctrl+Z** presses walk back through the history one step at a time. When the history drains, further presses are quiet no-ops. Redo (**Ctrl+Shift+Z**) is on the roadmap.

Note that the undo history resets when you load a project: **Ctrl+Z** does not reach across a Open call.

## 10. Keyboard shortcuts

| Key | Action | Where |
|---|---|---|
| **Space** | Play / Pause | anywhere |
| **B** | Place barline at playback position | anywhere |
| **M** | Place marker at playback position | anywhere |
| **L** | Create a loop from the primary and secondary anchors | anywhere |
| **Ctrl+click** | Attach a secondary anchor for loop creation | waveform, staff, dock |
| **Click** | Select an artifact, or seek if no artifact is hit | waveform, staff |
| **Drag** | Move a marker tick or a loop edge | waveform, staff |
| **Double-click** | Jump and play a marker; jump, arm, and play a loop | dock |
| **Delete** | Remove the selected artifact | anywhere |
| **Ctrl+O** | Open an audio file or a Fiddler project | anywhere |
| **Ctrl+S** | Save the current project | anywhere |
| **Ctrl+Shift+S** | Save the current project to a new path | anywhere |
| **Ctrl+Z** | Undo the most recent edit (placement, drag, dock edit, rename, or delete) | anywhere |
| **F4** | Show / hide the project viewer dock | anywhere |
| **Esc** | Clear the current selection | waveform, staff |
| **← / →** | Step through artifacts of the same kind | waveform, staff |

## 11. Reporting bugs

If something goes wrong, re-run Fiddler with the structured event log enabled and attach the resulting file to your bug report.

```bash
./build/src/fiddler \
    --log-filter='ui.*,player,waveform,score' \
    --log-file=/tmp/fiddler.log
```

Every user-driven action — file open, play, pause, seek, tempo change, tap-to-place, secondary-anchor, create-loop, loop-armed, loop-wrap, drag commit, undo, select, delete, time-signature pick, close — emits a single line capturing the verb, its parameters, and the resulting model state. The log lets the maintainers replay your session and reproduce the bug deterministically.

Note that the log captures gestures and state, not audio content. The recording you had open is not embedded in the log.

## 12. Glossary

*Anchor*. A source-time position that serves as one endpoint of a loop. The primary anchor is the currently-selected artifact; the secondary anchor is set by Ctrl+clicking another artifact. Pressing **L** creates a loop spanning both.

*Barline*. A vertical line on the staff that marks the start of a bar. Tap **B** during playback to place one at the playback position.

*Crooked tune*. A tune whose bars vary in length. Fiddler permits crooked-tune transcription by anchoring barlines to source-time rather than to a uniform bar grid.

*Loop*. A region of the recording, bounded by two source-time positions, that plays back repeatedly while armed.

*Marker*. A labelled cue point in the recording. Tap **M** during playback to drop one at the playback position.

*Pre-roll*. A short silence inserted before audio resumes, giving you time to ready the bow. Set the duration in the transport row's Pre-roll spinbox.

*Project*. The audio file plus everything you've placed on top of it — barlines, markers, loops, tune type, pre-roll setting. Saved as a `.fdlp` sidecar next to the audio with **Ctrl+S**.

*Tap to place*. Pressing **B** or **M** during playback to add a barline or marker at the current playback position. The primary placement gesture; clicks on the waveform or staff seek rather than place.

*Tune type*. A tradition-named preset that picks a time signature for the staff (Reel = 4/4, Jig = 6/8, and so on).
