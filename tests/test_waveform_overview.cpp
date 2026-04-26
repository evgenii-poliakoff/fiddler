// Tests for WaveformOverview. The pure data path (fromSamples) is
// exercised in isolation with synthetic float buffers; the Decoder
// integration is exercised against the existing audio corpus when
// present.

#include "audio/Decoder.h"
#include "audio/WaveformOverview.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <numbers>
#include <vector>

using fiddler::audio::buildOverview;
using fiddler::audio::PeakBucket;
using fiddler::audio::WaveformOverview;

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels   = 2;

std::vector<float> makeSine(double freqHz,
                            std::size_t frames,
                            double amplitude = 1.0,
                            int channels = kChannels,
                            int sampleRate = kSampleRate) {
    std::vector<float> buf(frames * channels);
    for (std::size_t f = 0; f < frames; ++f) {
        const float s = static_cast<float>(amplitude *
            std::sin(2.0 * std::numbers::pi * freqHz
                     * static_cast<double>(f) / sampleRate));
        for (int c = 0; c < channels; ++c) {
            buf[f * channels + c] = s;
        }
    }
    return buf;
}

double maxAbs(std::span<const PeakBucket> peaks) {
    double m = 0.0;
    for (const auto& p : peaks) {
        m = std::max({m, std::abs(static_cast<double>(p.min)),
                         std::abs(static_cast<double>(p.max))});
    }
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// fromSamples — degenerate / boundary inputs
// ---------------------------------------------------------------------------

TEST_CASE("WaveformOverview: empty input yields zero buckets",
          "[waveform-overview]") {
    auto ov = WaveformOverview::fromSamples({}, kSampleRate, kChannels, 1024);
    REQUIRE(ov.bucketCount() == 0);
    REQUIRE(ov.totalFrames() == 0);
    REQUIRE(ov.duration().count() == 0);
    REQUIRE(ov.peaks(0).empty());
}

TEST_CASE("WaveformOverview: zero buckets requested yields empty",
          "[waveform-overview]") {
    const auto in = makeSine(440.0, 1000);
    auto ov = WaveformOverview::fromSamples(in, kSampleRate, kChannels, 0);
    REQUIRE(ov.bucketCount() == 0);
}

TEST_CASE("WaveformOverview: invalid sample rate / channels rejected",
          "[waveform-overview]") {
    const auto in = makeSine(440.0, 1000);
    REQUIRE(WaveformOverview::fromSamples(in, 0, kChannels, 64)
                .bucketCount() == 0);
    REQUIRE(WaveformOverview::fromSamples(in, kSampleRate, 0, 64)
                .bucketCount() == 0);
}

TEST_CASE("WaveformOverview: bucketCount clamped when frames < buckets",
          "[waveform-overview]") {
    // Only 10 frames of stereo audio, but ask for 1000 buckets.
    std::vector<float> in(10 * kChannels, 0.5f);
    auto ov = WaveformOverview::fromSamples(in, kSampleRate, kChannels, 1000);
    REQUIRE(ov.bucketCount() == 10);
    REQUIRE(ov.totalFrames() == 10);
    // Every bucket holds the same constant.
    for (const auto& p : ov.peaks(0)) {
        REQUIRE(p.min == 0.5f);
        REQUIRE(p.max == 0.5f);
    }
}

// ---------------------------------------------------------------------------
// fromSamples — content checks
// ---------------------------------------------------------------------------

TEST_CASE("WaveformOverview: silence yields all-zero peaks",
          "[waveform-overview]") {
    std::vector<float> silence(kSampleRate * kChannels, 0.0f); // 1 s
    auto ov = WaveformOverview::fromSamples(
        silence, kSampleRate, kChannels, 256);
    REQUIRE(ov.bucketCount() == 256);
    for (int c = 0; c < kChannels; ++c) {
        for (const auto& p : ov.peaks(c)) {
            REQUIRE(p.min == 0.0f);
            REQUIRE(p.max == 0.0f);
        }
    }
}

TEST_CASE("WaveformOverview: DC produces flat min == max == DC",
          "[waveform-overview]") {
    constexpr float dc = 0.42f;
    std::vector<float> in(kSampleRate * kChannels, dc); // 1 s
    auto ov = WaveformOverview::fromSamples(in, kSampleRate, kChannels, 128);
    for (int c = 0; c < kChannels; ++c) {
        for (const auto& p : ov.peaks(c)) {
            REQUIRE(p.min == dc);
            REQUIRE(p.max == dc);
        }
    }
}

TEST_CASE("WaveformOverview: full-amplitude sine reaches ±1 in most buckets",
          "[waveform-overview]") {
    // 1 s of 440 Hz sine = 440 full cycles. With 256 buckets that's
    // ~1.7 cycles per bucket, more than enough for both crests to be
    // present in every bucket.
    auto in = makeSine(440.0, kSampleRate);
    auto ov = WaveformOverview::fromSamples(
        in, kSampleRate, kChannels, 256);

    REQUIRE(ov.bucketCount() == 256);
    REQUIRE(maxAbs(ov.peaks(0)) > 0.99);

    // Every bucket sees both halves of the cycle.
    for (const auto& p : ov.peaks(0)) {
        REQUIRE(p.min < -0.95f);
        REQUIRE(p.max >  0.95f);
    }
}

TEST_CASE("WaveformOverview: stereo asymmetry preserved per channel",
          "[waveform-overview]") {
    // Channel 0 = sine, channel 1 = silence.
    constexpr std::size_t frames = kSampleRate;
    std::vector<float> in(frames * kChannels, 0.0f);
    for (std::size_t f = 0; f < frames; ++f) {
        in[f * kChannels + 0] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 440.0 * f / kSampleRate));
        // channel 1 stays zero
    }
    auto ov = WaveformOverview::fromSamples(in, kSampleRate, kChannels, 128);

    REQUIRE(maxAbs(ov.peaks(0)) > 0.95);
    REQUIRE(maxAbs(ov.peaks(1)) == 0.0);
}

TEST_CASE("WaveformOverview: peaks() returns empty span for invalid channel",
          "[waveform-overview]") {
    auto in = makeSine(440.0, kSampleRate);
    auto ov = WaveformOverview::fromSamples(in, kSampleRate, kChannels, 64);
    REQUIRE(ov.peaks(-1).empty());
    REQUIRE(ov.peaks(kChannels).empty());     // out of range
    REQUIRE(ov.peaks(kChannels + 5).empty()); // way out of range
    REQUIRE(!ov.peaks(0).empty());
    REQUIRE(!ov.peaks(kChannels - 1).empty());
}

// ---------------------------------------------------------------------------
// Source-time mapping
// ---------------------------------------------------------------------------

TEST_CASE("WaveformOverview: bucketStartMs spans cover the file evenly",
          "[waveform-overview][time-mapping]") {
    // 10 s file → first bucket starts at 0, last bucket starts near
    // (N-1)/N * 10000 ms. With 1000 buckets each spans ~10 ms.
    constexpr std::size_t durationSec = 10;
    std::vector<float> in(kSampleRate * durationSec * kChannels, 0.25f);
    auto ov = WaveformOverview::fromSamples(
        in, kSampleRate, kChannels, 1000);

    REQUIRE(ov.bucketCount() == 1000);
    REQUIRE(ov.bucketStartMs(0).count() == 0);
    REQUIRE(ov.bucketStartMs(500).count() >= 4990);
    REQUIRE(ov.bucketStartMs(500).count() <= 5010);
    REQUIRE(ov.bucketStartMs(1000).count() == 10000); // == bucketEndMs(999)
    REQUIRE(ov.bucketEndMs(999).count() == 10000);

    REQUIRE(ov.duration().count() == 10000);
}

TEST_CASE("WaveformOverview: bucketAtMs is inverse of bucketStartMs",
          "[waveform-overview][time-mapping]") {
    constexpr std::size_t durationSec = 5;
    std::vector<float> in(kSampleRate * durationSec * kChannels, 0.0f);
    auto ov = WaveformOverview::fromSamples(in, kSampleRate, kChannels, 500);

    for (std::size_t i = 0; i < ov.bucketCount(); i += 47) {
        const auto ms = ov.bucketStartMs(i);
        // Round-trip through bucketAtMs should return the same bucket
        // (or one off due to integer boundary conditions, hence ≤ 1).
        const auto roundTrip = ov.bucketAtMs(ms);
        REQUIRE(roundTrip <= i + 1);
        REQUIRE(roundTrip + 1 >= i);
    }

    // Out-of-range times clamp to valid buckets.
    REQUIRE(ov.bucketAtMs(std::chrono::milliseconds{-1000}) == 0);
    REQUIRE(ov.bucketAtMs(std::chrono::milliseconds{999999})
                == ov.bucketCount() - 1);
}

// ---------------------------------------------------------------------------
// Decoder integration
// ---------------------------------------------------------------------------

TEST_CASE("buildOverview: returns nullopt for an unopened decoder",
          "[waveform-overview][decoder]") {
    fiddler::audio::Decoder dec; // not opened
    REQUIRE_FALSE(buildOverview(dec, 1024).has_value());
}

TEST_CASE("buildOverview: zero buckets requested yields nullopt",
          "[waveform-overview][decoder]") {
    fiddler::audio::Decoder dec;
    // We don't even need an open decoder — the function should
    // short-circuit on buckets == 0.
    REQUIRE_FALSE(buildOverview(dec, 0).has_value());
}

TEST_CASE("buildOverview: produces a sensible overview from corpus files",
          "[waveform-overview][decoder][corpus]") {
    namespace fs = std::filesystem;
    const fs::path corpus{ FIDDLER_TEST_DATA_DIR };
    if (!fs::exists(corpus) || fs::is_empty(corpus)) {
        WARN("corpus dir " << corpus << " is empty; skipping");
        return;
    }

    std::size_t checked = 0;
    for (const auto& entry : fs::directory_iterator(corpus)) {
        if (!entry.is_regular_file()) continue;
        fiddler::audio::Decoder dec;
        if (!dec.open(entry.path())) continue;

        auto ov = buildOverview(dec, 2048);
        REQUIRE(ov.has_value());
        REQUIRE(ov->bucketCount() > 0);
        REQUIRE(ov->bucketCount() <= 2048);
        REQUIRE(ov->channels() == 2);  // Decoder always outputs stereo
        REQUIRE(ov->sampleRate() == 48000);
        REQUIRE(ov->duration().count() > 0);
        // Peaks should sit inside the float range; nothing pathological.
        for (int c = 0; c < ov->channels(); ++c) {
            for (const auto& p : ov->peaks(c)) {
                REQUIRE(p.min >= -2.0f);
                REQUIRE(p.max <=  2.0f);
                REQUIRE(p.min <= p.max);
            }
        }
        ++checked;
    }
    if (checked == 0) {
        WARN("no decodable files found in corpus");
    }
}
