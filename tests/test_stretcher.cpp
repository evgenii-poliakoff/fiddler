// Tests for the Stretcher facade. Exercise the wrapper directly without
// pulling in Player or PortAudio — same approach as test_decoder.cpp.

#include "audio/Stretcher.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

using fiddler::audio::Stretcher;

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels   = 2;

std::vector<float> makeSine(double freqHz,
                            std::size_t frames,
                            double amplitude = 0.25) {
    std::vector<float> buf(frames * kChannels);
    for (std::size_t f = 0; f < frames; ++f) {
        const float s = static_cast<float>(amplitude *
            std::sin(2.0 * std::numbers::pi * freqHz
                     * static_cast<double>(f) / kSampleRate));
        for (int c = 0; c < kChannels; ++c) {
            buf[f * kChannels + c] = s;
        }
    }
    return buf;
}

// Drain everything currently retrievable into a fresh buffer.
std::vector<float> drainAll(Stretcher& s, std::size_t maxFrames) {
    std::vector<float> out(maxFrames * kChannels);
    std::size_t writtenFrames = 0;
    while (writtenFrames < maxFrames) {
        const std::size_t before = writtenFrames * kChannels;
        const std::size_t got = s.retrieve(
            std::span<float>(out).subspan(before,
                                          (maxFrames - writtenFrames) * kChannels));
        if (got == 0) break;
        writtenFrames += got / kChannels;
    }
    out.resize(writtenFrames * kChannels);
    return out;
}

double peak(std::span<const float> samples) {
    double m = 0.0;
    for (float v : samples) m = std::max(m, static_cast<double>(std::abs(v)));
    return m;
}

} // namespace

TEST_CASE("Stretcher passes audio through at ratio 1.0", "[stretcher]") {
    Stretcher s(kSampleRate, kChannels);
    s.setTimeRatio(1.0);

    constexpr std::size_t inputFrames = kSampleRate; // 1 s
    const auto input = makeSine(440.0, inputFrames);
    s.process(input, /*final=*/true);

    const auto output = drainAll(s, inputFrames * 2);
    const std::size_t outFrames = output.size() / kChannels;

    // ±15% slack: at ratio 1.0 the engine still emits a few thousand
    // trailing frames on final=true (the lookahead window) on top of
    // the real audio. The lower bound mostly catches a stalled engine.
    // (Pre-#40 this was ±10%; post-#40 the start-delay is consumed at
    // construction via primeRealtime(), which shifts where the slack
    // lands — the tail stays, the preroll silence is gone.)
    REQUIRE(outFrames >= inputFrames * 85 / 100);
    REQUIRE(outFrames <= inputFrames * 115 / 100);

    // Peak amplitude survives the round-trip within ~3 dB.
    const double pIn  = peak(input);
    const double pOut = peak(output);
    REQUIRE(pOut > pIn * 0.7);
    REQUIRE(pOut < pIn * 1.3);
}

TEST_CASE("Stretcher doubles duration at ratio 2.0", "[stretcher]") {
    Stretcher s(kSampleRate, kChannels);
    s.setTimeRatio(2.0);

    constexpr std::size_t inputFrames = kSampleRate; // 1 s
    const auto input = makeSine(440.0, inputFrames);
    s.process(input, /*final=*/true);

    const auto output = drainAll(s, inputFrames * 4);
    const std::size_t outFrames = output.size() / kChannels;

    REQUIRE(outFrames >= inputFrames * 18 / 10); // ≥ 1.8 s
    REQUIRE(outFrames <= inputFrames * 22 / 10); // ≤ 2.2 s
}

TEST_CASE("Stretcher quarters tempo at ratio 4.0 (extreme stretch)", "[stretcher]") {
    Stretcher s(kSampleRate, kChannels);
    s.setTimeRatio(4.0);

    constexpr std::size_t inputFrames = kSampleRate; // 1 s
    const auto input = makeSine(440.0, inputFrames);
    s.process(input, /*final=*/true);

    const auto output = drainAll(s, inputFrames * 6);
    const std::size_t outFrames = output.size() / kChannels;

    REQUIRE(outFrames >= inputFrames * 35 / 10); // ≥ 3.5 s
    REQUIRE(outFrames <= inputFrames * 45 / 10); // ≤ 4.5 s
}

TEST_CASE("Stretcher preserves pitch at ratio 2.0", "[stretcher]") {
    Stretcher s(kSampleRate, kChannels);
    s.setTimeRatio(2.0);

    // Two seconds of input gives plenty of stretched output (~4 s)
    // so the middle 50 % is well clear of preroll and tail.
    constexpr double freq = 440.0;
    constexpr std::size_t inputFrames = kSampleRate * 2;
    const auto input = makeSine(freq, inputFrames);
    s.process(input, /*final=*/true);

    const auto output = drainAll(s, inputFrames * 4);
    const std::size_t outFrames = output.size() / kChannels;
    REQUIRE(outFrames > kSampleRate); // sanity: more than 1 s out

    // Estimate pitch by counting zero crossings on channel 0 in the
    // middle half — cheap and good to a few percent for a clean sine.
    const std::size_t startFrame = outFrames / 4;
    const std::size_t endFrame   = outFrames * 3 / 4;
    int zeroCrossings = 0;
    for (std::size_t f = startFrame + 1; f < endFrame; ++f) {
        const float prev = output[(f - 1) * kChannels];
        const float cur  = output[f * kChannels];
        if ((prev <= 0.0f) != (cur <= 0.0f)) ++zeroCrossings;
    }
    const double durationSec =
        static_cast<double>(endFrame - startFrame) / kSampleRate;
    const double estimatedFreq = zeroCrossings / (2.0 * durationSec);

    // 5 % tolerance — proves we time-stretched, not resampled.
    REQUIRE(estimatedFreq > freq * 0.95);
    REQUIRE(estimatedFreq < freq * 1.05);
}

TEST_CASE("Stretcher reset clears buffered output", "[stretcher]") {
    Stretcher s(kSampleRate, kChannels);
    s.setTimeRatio(2.0);

    const auto input = makeSine(440.0, kSampleRate / 4); // 0.25 s
    s.process(input);
    REQUIRE(s.available() > 0);

    s.reset();
    REQUIRE(s.available() == 0);

    // After reset the stretcher is usable again at the same ratio.
    s.process(input, /*final=*/true);
    const auto output = drainAll(s, kSampleRate);
    REQUIRE(!output.empty());
}
