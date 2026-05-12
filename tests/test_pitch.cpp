// Tests for score::Pitch — the small set of free-function helpers
// that turn a MIDI integer into a string / a frequency / a diatonic
// staff step. These are the seams the staff paint code, the dock
// property page, and the future tone synth all consume; pinning
// them with spot-checks here means a regression shows up at this
// layer, not in the GUI.

#include "score/Pitch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using fiddler::score::diatonicStep;
using fiddler::score::midiToFrequencyHz;
using fiddler::score::midiToSpn;
using fiddler::score::spnToMidi;

// ---------------------------------------------------------------------------
// midiToSpn — uppercase letter + sharp spelling + octave
// ---------------------------------------------------------------------------

TEST_CASE("Pitch: midiToSpn middle-C convention is C4 = 60",
          "[pitch]") {
    // MEMO: load-bearing — Logic / MuseScore "scientific" mode. We
    // deliberately do NOT use the older Ableton C3 = middle-C
    // convention. If a user-facing toggle is ever wanted, expose it
    // as a setting; never silently change the default.
    REQUIRE(midiToSpn(60) == "C4");
}

TEST_CASE("Pitch: midiToSpn naturals across an octave",
          "[pitch]") {
    REQUIRE(midiToSpn(64) == "E4");
    REQUIRE(midiToSpn(65) == "F4");
    REQUIRE(midiToSpn(67) == "G4");
    REQUIRE(midiToSpn(69) == "A4");
    REQUIRE(midiToSpn(71) == "B4");
    REQUIRE(midiToSpn(72) == "C5");
    REQUIRE(midiToSpn(77) == "F5");
}

TEST_CASE("Pitch: midiToSpn accidentals use sharp spelling",
          "[pitch]") {
    REQUIRE(midiToSpn(61) == "C#4");
    REQUIRE(midiToSpn(66) == "F#4");
    REQUIRE(midiToSpn(70) == "A#4");
}

TEST_CASE("Pitch: midiToSpn out of range returns empty string",
          "[pitch]") {
    REQUIRE(midiToSpn(-1).isEmpty());
    REQUIRE(midiToSpn(128).isEmpty());
}

// ---------------------------------------------------------------------------
// spnToMidi — inverse parse; accepts ascii and unicode accidentals
// ---------------------------------------------------------------------------

TEST_CASE("Pitch: spnToMidi inverts midiToSpn for naturals",
          "[pitch]") {
    for (int midi = 60; midi <= 84; ++midi) {
        REQUIRE(spnToMidi(midiToSpn(midi)) == midi);
    }
}

TEST_CASE("Pitch: spnToMidi accepts lowercase letters",
          "[pitch]") {
    REQUIRE(spnToMidi("a4")  == 69);
    REQUIRE(spnToMidi("f#5") == 78);
}

TEST_CASE("Pitch: spnToMidi accepts both ascii and unicode accidentals",
          "[pitch]") {
    REQUIRE(spnToMidi("F#5") == 78);
    REQUIRE(spnToMidi(QString::fromUtf8("F\xE2\x99\xAF" "5")) == 78);  // F♯5
    REQUIRE(spnToMidi("Bb3") == 58);
    REQUIRE(spnToMidi(QString::fromUtf8("B\xE2\x99\xAD" "3")) == 58);  // B♭3
}

TEST_CASE("Pitch: spnToMidi handles double accidentals + negative octave",
          "[pitch]") {
    // F##5 = G5 (79); Bbb3 = A3 (57)
    REQUIRE(spnToMidi("F##5") == 79);
    REQUIRE(spnToMidi("Bbb3") == 57);

    // C-1 = midi 0 (lowest MIDI note)
    REQUIRE(spnToMidi("C-1") == 0);
}

TEST_CASE("Pitch: spnToMidi rejects malformed input",
          "[pitch]") {
    REQUIRE(spnToMidi("")      == -1);
    REQUIRE(spnToMidi("H4")    == -1);    // not a letter A-G
    REQUIRE(spnToMidi("C")     == -1);    // missing octave
    REQUIRE(spnToMidi("C4x")   == -1);    // trailing junk
    REQUIRE(spnToMidi("C99")   == -1);    // out of MIDI range
}

// ---------------------------------------------------------------------------
// midiToFrequencyHz — equal temperament, A4 = 440
// ---------------------------------------------------------------------------

TEST_CASE("Pitch: midiToFrequencyHz anchors at A4 = 440",
          "[pitch]") {
    using Catch::Matchers::WithinAbs;
    REQUIRE_THAT(midiToFrequencyHz(69), WithinAbs(440.0, 1e-9));
    // Octave below = 220, octave above = 880
    REQUIRE_THAT(midiToFrequencyHz(57), WithinAbs(220.0, 1e-9));
    REQUIRE_THAT(midiToFrequencyHz(81), WithinAbs(880.0, 1e-9));
}

TEST_CASE("Pitch: midiToFrequencyHz semitone ratio is the 12th root of 2",
          "[pitch]") {
    using Catch::Matchers::WithinAbs;
    const double a4 = midiToFrequencyHz(69);
    const double a4s = midiToFrequencyHz(70);
    const double ratio = a4s / a4;
    REQUIRE_THAT(ratio, WithinAbs(std::pow(2.0, 1.0 / 12.0), 1e-12));
}

TEST_CASE("Pitch: midiToFrequencyHz negative midi returns 0.0",
          "[pitch]") {
    REQUIRE(midiToFrequencyHz(-1) == 0.0);
}

// ---------------------------------------------------------------------------
// diatonicStep — staff-Y axis math
// ---------------------------------------------------------------------------

TEST_CASE("Pitch: diatonicStep treble-clef anchors",
          "[pitch]") {
    // MEMO: these five values pin treble-clef geometry. StaffWidget
    // computes staff-Y from diatonicStep(midi); if any of these
    // shift, every painted note moves on screen.
    REQUIRE(diatonicStep(64) == 30);   // E4, bottom line
    REQUIRE(diatonicStep(67) == 32);   // G4, 2nd line
    REQUIRE(diatonicStep(71) == 34);   // B4, middle line
    REQUIRE(diatonicStep(74) == 36);   // D5, 4th line
    REQUIRE(diatonicStep(77) == 38);   // F5, top line
}

TEST_CASE("Pitch: diatonicStep middle-C and adjacent naturals",
          "[pitch]") {
    REQUIRE(diatonicStep(60) == 28);   // C4 = middle C (one ledger line below)
    REQUIRE(diatonicStep(62) == 29);   // D4 (just below bottom line)
    REQUIRE(diatonicStep(81) == 40);   // A5 (one ledger above top line)
}

TEST_CASE("Pitch: diatonicStep returns -1 for accidentals",
          "[pitch]") {
    REQUIRE(diatonicStep(61) == -1);   // C#4
    REQUIRE(diatonicStep(63) == -1);   // D#4
    REQUIRE(diatonicStep(66) == -1);   // F#4
    REQUIRE(diatonicStep(68) == -1);   // G#4
    REQUIRE(diatonicStep(70) == -1);   // A#4
}

TEST_CASE("Pitch: diatonicStep is monotonic across naturals",
          "[pitch]") {
    int prev = -2;
    for (int midi = 60; midi <= 84; ++midi) {
        const int s = diatonicStep(midi);
        if (s < 0) continue;
        REQUIRE(s > prev);
        prev = s;
    }
}
