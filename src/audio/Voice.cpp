#include "audio/Voice.h"

#include <algorithm>

namespace fiddler::audio {

Voice::Voice()
    : source_(std::make_unique<Oscillator>(Waveform::Triangle))
{
}

void Voice::setSampleRate(double sampleRate) noexcept {
    if (sampleRate > 0.0) sampleRate_ = sampleRate;
    envelope_.setSampleRate(sampleRate);
}

void Voice::setSource(std::unique_ptr<SoundSource> source) noexcept {
    if (source) source_ = std::move(source);
}

void Voice::noteOnContinuous(double freqHz, Waveform waveform) noexcept {
    // The current Voice uses an Oscillator source; we set its
    // waveform inline. Sampler integration (future) will ignore
    // the Waveform parameter and pick a sample bank instead.
    if (auto* osc = dynamic_cast<Oscillator*>(source_.get())) {
        osc->setWaveform(waveform);
    }
    source_->setFrequency(freqHz);
    // Only re-trigger the envelope if we were idle / releasing.
    // Already-sustaining → retune-only (silent pivot, no click).
    if (!envelope_.isActive()) {
        envelope_.noteOnContinuous();
    }
}

void Voice::noteOnPulse(double freqHz,
                        int    durationMs,
                        Waveform waveform) noexcept {
    if (auto* osc = dynamic_cast<Oscillator*>(source_.get())) {
        osc->setWaveform(waveform);
    }
    source_->setFrequency(freqHz);
    // Sustain budget = durationMs minus attack+release so the total
    // tone length matches what the user asked for. Envelope's
    // setSampleRate has been called by the synth, so the millisecond
    // math is consistent.
    // Convert the user-facing ms duration to a frame budget using
    // the Voice's cached sample rate. Envelope's attack + release
    // (~40 ms) run in addition to this sustain, so the user
    // perceives a slightly longer tail — below "where did the
    // pulse go" perception for ear-match playback.
    const int sustainFrames = static_cast<int>(
        static_cast<double>(durationMs) * sampleRate_ / 1000.0);
    envelope_.noteOnPulse(sustainFrames);
}

void Voice::noteOff() noexcept {
    envelope_.noteOff();
}

void Voice::setFrequency(double freqHz) noexcept {
    source_->setFrequency(freqHz);
}

void Voice::setVolume(float volume) noexcept {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    volume_ = volume;
}

void Voice::render(float* output,
                   int    frameCount,
                   double sampleRate) noexcept {
    // Inactive voices are a fast no-op (no source render, no
    // envelope churn). Leaves `output` unchanged so the caller can
    // additively mix multiple voices into the same buffer.
    if (!envelope_.isActive()) return;

    int remaining = frameCount;
    int outPos    = 0;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kScratchFrames);
        // Source fills scratch_ with raw waveform.
        source_->render(scratch_, chunk, sampleRate);
        // Apply envelope + volume; additive write into output.
        for (int i = 0; i < chunk; ++i) {
            const float env = envelope_.nextSample();
            output[outPos + i] += scratch_[i] * env * volume_;
        }
        outPos    += chunk;
        remaining -= chunk;
    }
}

} // namespace fiddler::audio
