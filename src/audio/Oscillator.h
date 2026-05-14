// Oscillator — procedural waveform generator (sine / triangle).
//
// One of the two concrete SoundSource implementations (the other,
// future, is `Sampler` for real fiddle samples). A naive
// phase-accumulator with the waveform shape chosen at construction
// time. Allocation-free; safe to call from the audio callback.
//
// MEMO[#step6.3]: Sine + Triangle is the minimum useful set for
// reference-tone gestures. Sawtooth / Square / Pulse would slot in
// as additional `Waveform` enum values without changing the API.
// When the Sampler lands (real fiddle), it sits beside Oscillator
// behind the same SoundSource interface — Voice swaps the source
// pointer; everything above stays put.

#pragma once

#include "audio/SoundSource.h"

namespace fiddler::audio {

enum class Waveform {
    Sine,
    Triangle,
};

class Oscillator final : public SoundSource {
public:
    explicit Oscillator(Waveform waveform = Waveform::Triangle) noexcept;

    void setWaveform(Waveform waveform) noexcept;
    [[nodiscard]] Waveform waveform() const noexcept { return waveform_; }

    void setFrequency(double freqHz) noexcept override;

    void render(float* output,
                int    frameCount,
                double sampleRate) noexcept override;

private:
    Waveform waveform_;
    double   freqHz_  = 440.0;
    double   phase_   = 0.0;        // 0 .. 1
};

} // namespace fiddler::audio
