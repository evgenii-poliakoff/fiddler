#include "audio/Oscillator.h"

#include <cmath>
#include <numbers>

namespace fiddler::audio {

Oscillator::Oscillator(Waveform waveform) noexcept
    : waveform_(waveform) {}

void Oscillator::setWaveform(Waveform waveform) noexcept {
    waveform_ = waveform;
}

void Oscillator::setFrequency(double freqHz) noexcept {
    // Clamp to a reasonable range. Negative or near-zero would
    // freeze the phase; very high (above Nyquist) would alias.
    // Audio range cap at 20 kHz; lower cap at 1 Hz so a misfired
    // setFrequency(0) doesn't lock the oscillator.
    if (freqHz < 1.0)      freqHz = 1.0;
    if (freqHz > 20000.0)  freqHz = 20000.0;
    freqHz_ = freqHz;
}

void Oscillator::render(float* output,
                        int    frameCount,
                        double sampleRate) noexcept {
    if (sampleRate <= 0.0) return;
    const double phaseInc = freqHz_ / sampleRate;
    for (int i = 0; i < frameCount; ++i) {
        float sample = 0.0f;
        switch (waveform_) {
        case Waveform::Sine:
            // sin(2π * phase). std::sin is fine for a single voice
            // in a non-realtime-critical path; if the synth ever
            // needs polyphony into the dozens we'd swap for a
            // wavetable lookup. Today it's a single ear-match tone.
            sample = static_cast<float>(
                std::sin(2.0 * std::numbers::pi * phase_));
            break;
        case Waveform::Triangle:
            // 4|x - 0.5| - 1 on x ∈ [0, 1] gives a triangle from
            // -1 to +1 with a peak at phase=0.5. Cheap, no calls.
            sample = static_cast<float>(
                4.0 * std::fabs(phase_ - 0.5) - 1.0);
            break;
        }
        output[i] = sample;
        phase_ += phaseInc;
        if (phase_ >= 1.0) phase_ -= 1.0;
    }
}

} // namespace fiddler::audio
