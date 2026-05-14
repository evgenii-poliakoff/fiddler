// Voice — one sounding voice. Combines a SoundSource (today an
// Oscillator; future a Sampler) with an Envelope.
//
// Architecture (issue #step6.3 → future):
//
//   Voice
//   ├── SoundSource   — Oscillator (Step 6.3) or Sampler (future)
//   ├── Envelope      — attack/sustain/release shaping
//   └── (future inserts)
//       • Modulator   — vibrato LFO mixed into setFrequency
//       • Articulator — translates ornament/accent hints into the
//                        envelope shape + vibrato params
//
// Allocation-free; all methods are safe from the audio callback
// thread (and the GUI thread, which sets target params; the
// callback reads them additively).
//
// For Step 6.3 the synth holds a single Voice (`referenceVoice_`)
// shared by click-keyboard / hover-tone / pulse gestures. When
// `scheduleNote()` arrives for chord playback, a VoicePool will
// own N voices; Voice itself doesn't change.

#pragma once

#include "audio/Envelope.h"
#include "audio/Oscillator.h"

#include <memory>

namespace fiddler::audio {

class Voice {
public:
    Voice();

    void setSampleRate(double sampleRate) noexcept;

    // Swap the sound source (used by future Sampler integration).
    // Default ownership is Oscillator; callers replace it via this
    // setter. Source ownership transfers to Voice.
    void setSource(std::unique_ptr<SoundSource> source) noexcept;
    [[nodiscard]] SoundSource& source() noexcept { return *source_; }

    // ---- Note triggers ----

    // Start a steady note. Attack ramps up; sustain holds until
    // noteOff. Re-calling on an already-sustaining voice retunes
    // without click (envelope stays in Sustain).
    void noteOnContinuous(double freqHz, Waveform waveform) noexcept;

    // Start a one-shot note that auto-releases after `durationMs`.
    void noteOnPulse(double freqHz,
                     int    durationMs,
                     Waveform waveform) noexcept;

    // Release a steady note (10 ms+ fade). No-op if already idle.
    void noteOff() noexcept;

    // Retune mid-sustain — for hover-tone tracking the row under
    // the cursor, or for a vertical drag re-pitching a note. No
    // envelope retrigger, so the tone slides cleanly. The
    // SoundSource clamps to its audio range internally.
    void setFrequency(double freqHz) noexcept;

    void setVolume(float volume) noexcept;
    [[nodiscard]] float volume() const noexcept { return volume_; }

    [[nodiscard]] bool isActive() const noexcept { return envelope_.isActive(); }

    // Render `frameCount` MONO samples into `output`. The caller is
    // responsible for stereo duplication / mixing into the final
    // device buffer. Internally: source produces the raw waveform,
    // envelope * volume gives the per-sample gain.
    void render(float* output, int frameCount, double sampleRate) noexcept;

private:
    // Default source is an Oscillator; replaceable at runtime.
    std::unique_ptr<SoundSource> source_;
    Envelope envelope_;
    float    volume_ = 0.5f;
    // Cached so noteOnPulse can convert ms → frames consistently
    // with the envelope's setSampleRate. Set by setSampleRate.
    double   sampleRate_ = 44100.0;

    // Scratch buffer for source output — caller-supplied buffer
    // would do, but a Voice that one day adds modulators (FM) might
    // need its own working buffer. Kept small (256 frames) and
    // re-used to avoid allocations in the callback.
    static constexpr int kScratchFrames = 256;
    float scratch_[kScratchFrames]{};
};

} // namespace fiddler::audio
