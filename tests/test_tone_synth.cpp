// Audio-layer unit tests for the synth stack:
//
//   Oscillator  — frequency accuracy via zero-crossing estimate
//   Envelope    — state machine, attack/sustain/release shape
//   Voice       — combines Oscillator + Envelope; isActive lifecycle
//   ToneSynth   — public API (renderForTest bypasses PortAudio so
//                 CI hosts without an audio device still cover the
//                 frequency / envelope contract)
//
// Frequency tests use the same zero-crossing method
// `test_stretcher.cpp` uses — cheap and good to a few percent for a
// clean periodic signal. No FFT dependency.

#include "audio/Envelope.h"
#include "audio/Oscillator.h"
#include "audio/ToneSynth.h"
#include "audio/Voice.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <vector>

using fiddler::audio::Envelope;
using fiddler::audio::Oscillator;
using fiddler::audio::ToneSynth;
using fiddler::audio::Voice;
using fiddler::audio::Waveform;

namespace {

// Count zero-crossings (sign changes) in the middle half of the
// buffer to estimate frequency without endpoint artifacts.
double estimateFrequency(const std::vector<float>& buffer,
                        double sampleRate) {
    const std::size_t n = buffer.size();
    const std::size_t startFrame = n / 4;
    const std::size_t endFrame   = n * 3 / 4;
    int zeroCrossings = 0;
    for (std::size_t f = startFrame + 1; f < endFrame; ++f) {
        const float prev = buffer[f - 1];
        const float cur  = buffer[f];
        if ((prev <= 0.0f) != (cur <= 0.0f)) ++zeroCrossings;
    }
    const double durationSec =
        static_cast<double>(endFrame - startFrame) / sampleRate;
    return zeroCrossings / (2.0 * durationSec);
}

constexpr double kRate = 44100.0;

} // namespace

// ---------------------------------------------------------------------------
// Oscillator
// ---------------------------------------------------------------------------

TEST_CASE("Oscillator: sine at 440 Hz produces the expected frequency",
          "[audio][oscillator]") {
    Oscillator osc(Waveform::Sine);
    osc.setFrequency(440.0);

    std::vector<float> buffer(static_cast<size_t>(kRate));  // 1 s
    osc.render(buffer.data(), static_cast<int>(buffer.size()), kRate);

    const auto freq = estimateFrequency(buffer, kRate);
    REQUIRE(freq > 440.0 * 0.98);
    REQUIRE(freq < 440.0 * 1.02);
}

TEST_CASE("Oscillator: triangle at 200 Hz produces the expected frequency",
          "[audio][oscillator]") {
    Oscillator osc(Waveform::Triangle);
    osc.setFrequency(200.0);

    std::vector<float> buffer(static_cast<size_t>(kRate));
    osc.render(buffer.data(), static_cast<int>(buffer.size()), kRate);

    const auto freq = estimateFrequency(buffer, kRate);
    REQUIRE(freq > 200.0 * 0.98);
    REQUIRE(freq < 200.0 * 1.02);
}

TEST_CASE("Oscillator: triangle peaks at ±1 amplitude",
          "[audio][oscillator]") {
    Oscillator osc(Waveform::Triangle);
    osc.setFrequency(100.0);
    std::vector<float> buffer(static_cast<size_t>(kRate));
    osc.render(buffer.data(), static_cast<int>(buffer.size()), kRate);

    float maxAbs = 0.0f;
    for (float s : buffer) {
        const float a = std::fabs(s);
        if (a > maxAbs) maxAbs = a;
    }
    // Triangle reaches ±1 at the peaks; should be very close.
    REQUIRE(maxAbs > 0.95f);
    REQUIRE(maxAbs <= 1.0f);
}

TEST_CASE("Oscillator: out-of-range frequency clamps without locking phase",
          "[audio][oscillator]") {
    // setFrequency(0) or negative should clamp to 1 Hz, not freeze
    // the phase at a constant value.
    Oscillator osc(Waveform::Sine);
    osc.setFrequency(-100.0);

    std::vector<float> buffer(static_cast<size_t>(kRate));
    osc.render(buffer.data(), static_cast<int>(buffer.size()), kRate);

    // At 1 Hz over 1 s we expect 2 zero crossings (one cycle).
    const auto freq = estimateFrequency(buffer, kRate);
    REQUIRE(freq >= 0.5);
    REQUIRE(freq <= 2.0);
}

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

TEST_CASE("Envelope: continuous note attacks then sustains until noteOff",
          "[audio][envelope]") {
    Envelope env;
    env.setSampleRate(kRate);

    REQUIRE_FALSE(env.isActive());
    env.noteOnContinuous();
    REQUIRE(env.isActive());

    // Attack phase: gain starts near 0, ramps up.
    const float firstGain = env.nextSample();
    REQUIRE(firstGain < 0.1f);

    // After ~20 ms (well past the 10 ms attack) we should be in
    // sustain at gain ≈ 1.
    const int sustainProbeFrame = static_cast<int>(kRate * 20.0 / 1000.0);
    for (int i = 1; i < sustainProbeFrame; ++i) (void)env.nextSample();
    REQUIRE(env.nextSample() > 0.99f);

    // Sustain holds for a long time without auto-release.
    for (int i = 0; i < static_cast<int>(kRate); ++i) (void)env.nextSample();
    REQUIRE(env.isActive());
    REQUIRE(env.nextSample() > 0.99f);

    // noteOff begins the release.
    env.noteOff();
    REQUIRE(env.isActive());

    // Release ramps down over 30 ms. After 40 ms we should be idle.
    const int releaseProbeFrame = static_cast<int>(kRate * 40.0 / 1000.0);
    for (int i = 0; i < releaseProbeFrame; ++i) (void)env.nextSample();
    REQUIRE_FALSE(env.isActive());
    REQUIRE(env.nextSample() == 0.0f);
}

TEST_CASE("Envelope: pulse auto-releases after its sustain budget",
          "[audio][envelope]") {
    Envelope env;
    env.setSampleRate(kRate);

    // 100 ms sustain budget. Total audible ≈ attack (10) + sustain (100)
    // + release (30) ≈ 140 ms.
    const int sustainFrames = static_cast<int>(kRate * 100.0 / 1000.0);
    env.noteOnPulse(sustainFrames);
    REQUIRE(env.isActive());

    // Drain ~50 ms — still active (attack + part of sustain).
    const int probe1 = static_cast<int>(kRate * 50.0 / 1000.0);
    for (int i = 0; i < probe1; ++i) (void)env.nextSample();
    REQUIRE(env.isActive());

    // After 200 ms total we should be idle (sustain done + release fade
    // complete).
    const int probe2 = static_cast<int>(kRate * 150.0 / 1000.0);
    for (int i = 0; i < probe2; ++i) (void)env.nextSample();
    REQUIRE_FALSE(env.isActive());
}

TEST_CASE("Envelope: noteOff while in attack still releases cleanly",
          "[audio][envelope]") {
    // Mid-attack interrupt: release should ramp from the current gain
    // (some value < 1) down to 0. No click.
    Envelope env;
    env.setSampleRate(kRate);
    env.noteOnContinuous();

    // Drain 5 ms of attack — gain partway to 1.
    const int probe = static_cast<int>(kRate * 5.0 / 1000.0);
    for (int i = 0; i < probe; ++i) (void)env.nextSample();
    env.noteOff();

    // 40 ms later we should be idle and at 0.
    const int afterRelease = static_cast<int>(kRate * 40.0 / 1000.0);
    for (int i = 0; i < afterRelease; ++i) (void)env.nextSample();
    REQUIRE_FALSE(env.isActive());
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

TEST_CASE("Voice: continuous note renders at the requested pitch",
          "[audio][voice]") {
    Voice voice;
    voice.setSampleRate(kRate);
    voice.setVolume(1.0f);
    voice.noteOnContinuous(440.0, Waveform::Sine);

    std::vector<float> buffer(static_cast<size_t>(kRate), 0.0f);
    voice.render(buffer.data(), static_cast<int>(buffer.size()), kRate);

    const auto freq = estimateFrequency(buffer, kRate);
    REQUIRE(freq > 440.0 * 0.95);
    REQUIRE(freq < 440.0 * 1.05);
}

TEST_CASE("Voice: pulse goes idle after the requested duration",
          "[audio][voice]") {
    Voice voice;
    voice.setSampleRate(kRate);
    voice.noteOnPulse(440.0, /*durationMs=*/50, Waveform::Triangle);
    REQUIRE(voice.isActive());

    // Render a buffer well past the pulse duration (200 ms).
    std::vector<float> buffer(static_cast<size_t>(kRate * 0.2), 0.0f);
    voice.render(buffer.data(), static_cast<int>(buffer.size()), kRate);

    REQUIRE_FALSE(voice.isActive());
}

TEST_CASE("Voice: setFrequency mid-sustain retunes without retriggering",
          "[audio][voice]") {
    // 200 Hz for the first half, retune to 400 Hz for the second.
    // The zero-crossing estimate should land near 300 Hz (the mean).
    Voice voice;
    voice.setSampleRate(kRate);
    voice.setVolume(1.0f);
    voice.noteOnContinuous(200.0, Waveform::Sine);

    const std::size_t halfFrames = static_cast<std::size_t>(kRate / 2);
    std::vector<float> firstHalf(halfFrames, 0.0f);
    voice.render(firstHalf.data(),
                 static_cast<int>(firstHalf.size()),
                 kRate);

    voice.setFrequency(400.0);
    std::vector<float> secondHalf(halfFrames, 0.0f);
    voice.render(secondHalf.data(),
                 static_cast<int>(secondHalf.size()),
                 kRate);

    // First half ≈ 200 Hz, second half ≈ 400 Hz. The envelope is in
    // sustain throughout so amplitude is steady — no click on
    // retune.
    REQUIRE(estimateFrequency(firstHalf, kRate)  > 195.0);
    REQUIRE(estimateFrequency(firstHalf, kRate)  < 205.0);
    REQUIRE(estimateFrequency(secondHalf, kRate) > 395.0);
    REQUIRE(estimateFrequency(secondHalf, kRate) < 405.0);
}

TEST_CASE("Voice: volume scales the output amplitude linearly",
          "[audio][voice]") {
    Voice voice;
    voice.setSampleRate(kRate);
    voice.setVolume(0.25f);
    voice.noteOnContinuous(440.0, Waveform::Triangle);

    // Render past the attack so amplitude is steady.
    std::vector<float> buffer(static_cast<size_t>(kRate), 0.0f);
    voice.render(buffer.data(), static_cast<int>(buffer.size()), kRate);

    // Peak should be ≈ 0.25 * 1.0 (triangle peak) = 0.25.
    float peak = 0.0f;
    // Probe the middle half to skip attack ramp.
    for (size_t i = buffer.size() / 4; i < buffer.size() * 3 / 4; ++i) {
        const float a = std::fabs(buffer[i]);
        if (a > peak) peak = a;
    }
    REQUIRE(peak > 0.20f);
    REQUIRE(peak < 0.30f);
}

// ---------------------------------------------------------------------------
// ToneSynth (public API)
// ---------------------------------------------------------------------------

TEST_CASE("ToneSynth: renderForTest produces audio at requested pitch",
          "[audio][tone-synth]") {
    // Pin the public API via the test seam — doesn't depend on a
    // working PortAudio device. Verifies that ToneSynth correctly
    // wires playContinuous → Voice → Oscillator.
    ToneSynth synth;
    synth.setVolume(1.0f);
    synth.playContinuous(330.0, Waveform::Sine);

    std::vector<float> buffer(static_cast<size_t>(kRate), 0.0f);
    synth.renderForTest(buffer.data(),
                        static_cast<int>(buffer.size()),
                        kRate);

    const auto freq = estimateFrequency(buffer, kRate);
    REQUIRE(freq > 330.0 * 0.95);
    REQUIRE(freq < 330.0 * 1.05);
}

TEST_CASE("ToneSynth: stop ends a continuous tone with a release fade",
          "[audio][tone-synth]") {
    ToneSynth synth;
    synth.setVolume(1.0f);
    synth.playContinuous(440.0, Waveform::Sine);

    // Drain 100 ms of sustain.
    std::vector<float> drainBuf(
        static_cast<size_t>(kRate * 0.1), 0.0f);
    synth.renderForTest(drainBuf.data(),
                        static_cast<int>(drainBuf.size()),
                        kRate);

    synth.stop();

    // Drain another 100 ms; should reach silence after the 30 ms
    // release.
    std::vector<float> afterStop(
        static_cast<size_t>(kRate * 0.1), 0.0f);
    synth.renderForTest(afterStop.data(),
                        static_cast<int>(afterStop.size()),
                        kRate);

    // Last 10 ms — final samples — should be ~0.
    float maxTail = 0.0f;
    const size_t tailStart = afterStop.size() - kRate / 100;
    for (size_t i = tailStart; i < afterStop.size(); ++i) {
        const float a = std::fabs(afterStop[i]);
        if (a > maxTail) maxTail = a;
    }
    REQUIRE(maxTail < 0.01f);
}

TEST_CASE("ToneSynth: playPulse renders briefly and returns to silence",
          "[audio][tone-synth]") {
    ToneSynth synth;
    synth.setVolume(1.0f);
    synth.playPulse(440.0, std::chrono::milliseconds(50),
                    Waveform::Triangle);

    // Render 300 ms — the pulse + release should be over well before.
    std::vector<float> buffer(static_cast<size_t>(kRate * 0.3), 0.0f);
    synth.renderForTest(buffer.data(),
                        static_cast<int>(buffer.size()),
                        kRate);

    // Last 50 ms should be silent.
    float maxTail = 0.0f;
    const size_t tailStart = buffer.size() - kRate / 20;
    for (size_t i = tailStart; i < buffer.size(); ++i) {
        const float a = std::fabs(buffer[i]);
        if (a > maxTail) maxTail = a;
    }
    REQUIRE(maxTail < 0.01f);

    // Middle of the pulse should have audible signal.
    const size_t midProbe = static_cast<size_t>(kRate * 0.04);
    REQUIRE(std::fabs(buffer[midProbe]) > 0.05f);
}

TEST_CASE("ToneSynth: setVolume scales subsequent output",
          "[audio][tone-synth]") {
    ToneSynth synth;
    synth.setVolume(0.1f);
    synth.playContinuous(440.0, Waveform::Triangle);

    std::vector<float> buffer(static_cast<size_t>(kRate), 0.0f);
    synth.renderForTest(buffer.data(),
                        static_cast<int>(buffer.size()),
                        kRate);

    float peak = 0.0f;
    for (size_t i = buffer.size() / 4; i < buffer.size() * 3 / 4; ++i) {
        const float a = std::fabs(buffer[i]);
        if (a > peak) peak = a;
    }
    // Triangle peak * 0.1 volume ≈ 0.1.
    REQUIRE(peak > 0.05f);
    REQUIRE(peak < 0.15f);
}
