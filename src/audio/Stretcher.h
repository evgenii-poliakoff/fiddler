// Stretcher — pitch-preserving time-stretch wrapper around Rubber Band.
//
// The decoder thread feeds interleaved float samples in via process(),
// then drains stretched output via retrieve(). All Rubber Band types
// are confined to Stretcher.cpp through pImpl, per Rule 6.
//
// Threading: not thread-safe by itself. Owned and called from a single
// thread (the decoder thread inside Player). The caller serialises
// setTimeRatio() against process()/retrieve() — typically by sharing
// Player::mutex_.
//
// Realtime configuration: built with OptionProcessRealTime so that
// process() and retrieve() do not allocate after construction. The
// engine is OptionEngineFiner (R3) — phase-vocoder with finer
// time/frequency resolution; preserves bow attacks and ornament
// transients down to 25% playback rate.

#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace fiddler::audio {

class Stretcher {
public:
    // sampleRate / channels match the decoder's output format.
    Stretcher(int sampleRate, int channels);
    ~Stretcher();

    Stretcher(const Stretcher&)            = delete;
    Stretcher& operator=(const Stretcher&) = delete;

    // Output duration / input duration. 1.0 = passthrough rate;
    // 2.0 = output is twice as long (half-tempo playback at the
    // original pitch).
    void setTimeRatio(double ratio);

    // Drop all buffered state; safe after a seek. Retains the current
    // time ratio.
    void reset();

    // Push interleaved float samples in. `final = true` flushes the
    // tail at end-of-stream so the last input is fully processed.
    void process(std::span<const float> interleaved, bool final = false);

    // Number of output frames currently retrievable.
    [[nodiscard]] std::size_t available() const noexcept;

    // Pull up to interleaved.size()/channels frames out. Returns the
    // number of float samples actually written.
    std::size_t retrieve(std::span<float> interleaved);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fiddler::audio
