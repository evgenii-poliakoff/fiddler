#include "score/Pitch.h"

#include <QChar>
#include <QRegularExpression>

#include <cmath>

namespace fiddler::score {

namespace {

// Letter offsets within an octave for the seven naturals. -1 for
// chromatic positions that are not on a natural — diatonicStep()
// returns -1 in that case so the caller can detect "this is an
// accidental, route through the spelling layer."
constexpr int kNaturalOffset[12] = {
    0,   // C
    -1,  // C#
    1,   // D
    -1,  // D#
    2,   // E
    3,   // F
    -1,  // F#
    4,   // G
    -1,  // G#
    5,   // A
    -1,  // A#
    6    // B
};

constexpr const char* kSharpSpelling[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Letter A..G → its chroma offset within an octave (A = 9, B = 11,
// C = 0, ...). Used by spnToMidi.
int letterChroma(QChar letter) {
    switch (letter.toUpper().unicode()) {
        case 'C': return 0;
        case 'D': return 2;
        case 'E': return 4;
        case 'F': return 5;
        case 'G': return 7;
        case 'A': return 9;
        case 'B': return 11;
    }
    return -1;
}

} // namespace

QString midiToSpn(int midi) {
    if (midi < 0 || midi > 127) return {};
    const int octave   = midi / 12 - 1;
    const int chroma   = midi % 12;
    return QString::fromLatin1(kSharpSpelling[chroma]) + QString::number(octave);
}

int spnToMidi(const QString& spn) {
    if (spn.isEmpty()) return -1;

    // Letter
    const int chromaBase = letterChroma(spn.at(0));
    if (chromaBase < 0) return -1;

    // Walk accidentals: # / ♯ raise, b / B-after-letter / ♭ lower.
    // After the leading letter we read all accidental characters
    // before the octave digits start.
    int alter = 0;
    int i = 1;
    while (i < spn.size()) {
        const QChar c = spn.at(i);
        if (c == '#' || c == QChar(0x266F)) { ++alter; ++i; continue; }
        if (c == 'b' || c == QChar(0x266D)) { --alter; ++i; continue; }
        break;
    }

    // Octave: optional sign + digits
    if (i >= spn.size()) return -1;
    bool negative = false;
    if (spn.at(i) == '-') { negative = true; ++i; }
    if (i >= spn.size() || !spn.at(i).isDigit()) return -1;
    int octave = 0;
    while (i < spn.size() && spn.at(i).isDigit()) {
        octave = octave * 10 + spn.at(i).digitValue();
        ++i;
    }
    if (i != spn.size()) return -1;     // trailing junk
    if (negative) octave = -octave;

    const int midi = (octave + 1) * 12 + chromaBase + alter;
    if (midi < 0 || midi > 127) return -1;
    return midi;
}

double midiToFrequencyHz(int midi) {
    if (midi < 0) return 0.0;
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

int diatonicStep(int midi) {
    if (midi < 0) return -1;
    const int octave = midi / 12 - 1;
    const int chroma = midi % 12;
    const int off    = kNaturalOffset[chroma];
    if (off < 0) return -1;
    // Step is counted from C0 (octave 0, offset 0). C-1 would be -7;
    // we accept it for completeness, but in practice 6.1 inputs are
    // far above that boundary.
    return (octave + 1) * 7 + off - 7;
}

} // namespace fiddler::score
