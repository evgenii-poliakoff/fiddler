// SoundSource — abstract base for things that generate audio samples
// at a target frequency. The polymorphism point that lets the synth
// architecture grow:
//
//   • Oscillator (Step 6.3) — procedural sine / triangle / etc.
//   • Sampler    (future)   — plays back a recorded fiddle sample
//                             library, pitch-shifted to the target.
//
// A SoundSource owns its phase state. `setFrequency` retunes without
// a discontinuity; `render` writes `frameCount` MONO samples into
// `output` (the Voice layer above is responsible for stereo
// duplication and gain). All methods may be called from the audio
// callback thread — implementations must be allocation-free.
//
// MEMO[#step6.3]: kept deliberately minimal. Future fiddle synthesis
// (vibrato / portamento / bow strokes / ornaments) lives in higher
// layers (Voice's Modulator / Articulator) and shapes the SoundSource
// from the outside via setFrequency / setAmplitude (not added yet).
// Sampler-based playback drops in without touching Voice.

#pragma once

namespace fiddler::audio {

class SoundSource {
public:
    SoundSource() = default;
    virtual ~SoundSource() = default;

    SoundSource(const SoundSource&)            = delete;
    SoundSource& operator=(const SoundSource&) = delete;

    // Target frequency in Hz. Implementations adjust their internal
    // oscillator phase increment / sample-rate ratio on the next
    // render call.
    virtual void setFrequency(double freqHz) noexcept = 0;

    // Render `frameCount` MONO samples into `output` (additive write
    // is the caller's choice — for the simple single-voice case the
    // caller passes a zero-cleared buffer). Each call must advance
    // exactly `frameCount` frames of phase regardless of envelope /
    // amplitude — those are applied by the Voice layer above so the
    // SoundSource stays a pure tonal generator.
    virtual void render(float* output,
                        int    frameCount,
                        double sampleRate) noexcept = 0;
};

} // namespace fiddler::audio
