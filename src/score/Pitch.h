// score::Pitch — small free-function helpers for converting between
// representations of a single note's pitch.
//
// MEMO: deliberately a free-function header, not a class. None of
// these helpers need state, and keeping them as `inline`-able
// free functions lets staff paint code, the dock property page,
// the future tone-synth, and the future MusicXML exporter all
// share one canonical conversion without dragging in a wrapper.
//
// Coordinate conventions:
//
//   * MIDI integer: standard MIDI note number. 60 = C4 (middle C),
//     69 = A4 = 440 Hz, 21 = A0, 108 = C8. Same as Logic / MuseScore
//     "scientific" mode (note middle-C convention) — Ableton / older
//     DAWs that call middle-C "C3" are not what we use.
//
//   * SPN (Scientific Pitch Notation): "<letter><optional accidental><octave>",
//     e.g. "A4", "C#5", "F#5". Sharp spelling is the v1 default;
//     when accidentals + key signatures land, this helper grows a
//     mode argument (sharp / flat / preferred-by-key). Lower-case
//     letters are accepted on parse; we always emit uppercase.
//
//   * Staff diatonic step: a 0-indexed counter of naturals starting
//     from C0. C0 → 0, D0 → 1, … B0 → 6, C1 → 7, … The treble-clef
//     bottom line E4 lands at step 30; top line F5 at step 38. This
//     is the metric the staff Y-coordinate is calculated in.

#pragma once

#include <QString>

namespace fiddler::score {

// MIDI integer → Scientific Pitch Notation. Always uppercase letter
// + sharp spelling for accidentals (e.g. "F#5", never "Gb5"). Out of
// range inputs (< 0 or > 127) return an empty string — caller
// chose to keep the conversion total without throwing.
[[nodiscard]] QString midiToSpn(int midi);

// SPN → MIDI integer. Accepts forms: "A4", "a4", "F#5", "F♯5",
// "Bb3", "B♭3", "F##5" (double sharp), "Bbb3" (double flat). Returns
// -1 on parse failure or if the result would fall outside [0, 127].
[[nodiscard]] int spnToMidi(const QString& spn);

// MIDI integer → frequency in Hz, using equal temperament with
// A4 = 440 Hz. Returns 0.0 for negative midi (out-of-range guard).
// midi need NOT be a natural — sharps/flats work too; the future
// tone-synth uses this as soon as the model accepts accidentals.
[[nodiscard]] double midiToFrequencyHz(int midi);

// MIDI integer → 0-indexed diatonic staff step (counting only
// naturals). Returns -1 if midi is an accidental (sharp/flat) — the
// caller is expected to restrict input to naturals in 6.1; later
// step-and-alter spelling will route accidentals to the correct
// step at render time.
//
// Reference values (treble clef geometry):
//   midi 12 (C0) → 0
//   midi 60 (C4) → 28
//   midi 64 (E4) → 30   (bottom staff line)
//   midi 71 (B4) → 34   (middle staff line)
//   midi 77 (F5) → 38   (top staff line)
//   midi 108 (C8) → 56
[[nodiscard]] int diatonicStep(int midi);

} // namespace fiddler::score
