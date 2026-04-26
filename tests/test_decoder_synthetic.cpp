// Synthetic roundtrip: write a 1 s 440 Hz sine WAV, decode it back through
// Decoder, verify the sample count and that 440 Hz dominates the spectrum.
//
// This test is self-contained — it doesn't need anything on disk — so it
// runs in CI even when the corpus directory is empty.

#include "audio/Decoder.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Minimal little-endian RIFF / WAVE PCM-16 mono writer.
void writeWav(const fs::path& path, int sampleRate, int channels,
              const std::vector<std::int16_t>& samples) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());

    auto w32 = [&](std::uint32_t v) {
        char b[4] = { char(v), char(v >> 8), char(v >> 16), char(v >> 24) };
        out.write(b, 4);
    };
    auto w16 = [&](std::uint16_t v) {
        char b[2] = { char(v), char(v >> 8) };
        out.write(b, 2);
    };

    const std::uint32_t dataSize =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));

    out.write("RIFF", 4);
    w32(36 + dataSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    w32(16);                                       // PCM fmt chunk size
    w16(1);                                        // PCM
    w16(static_cast<std::uint16_t>(channels));
    w32(static_cast<std::uint32_t>(sampleRate));
    w32(static_cast<std::uint32_t>(sampleRate * channels * sizeof(std::int16_t)));
    w16(static_cast<std::uint16_t>(channels * sizeof(std::int16_t)));
    w16(16);                                       // bits per sample
    out.write("data", 4);
    w32(dataSize);
    out.write(reinterpret_cast<const char*>(samples.data()), dataSize);
}

// Goertzel single-bin DFT magnitude squared. We don't care about absolute
// magnitude here — only the relative energy at the target frequency.
double goertzelEnergy(const std::vector<float>& samples,
                      int sampleRate, double frequency) {
    const int N = static_cast<int>(samples.size());
    const int k = static_cast<int>(0.5 + (N * frequency / sampleRate));
    const double omega = (2.0 * std::numbers::pi / N) * k;
    const double cosine = std::cos(omega);
    const double coeff = 2.0 * cosine;
    double q1 = 0.0, q2 = 0.0;
    for (float s : samples) {
        const double q0 = coeff * q1 - q2 + s;
        q2 = q1;
        q1 = q0;
    }
    return q1 * q1 + q2 * q2 - q1 * q2 * coeff;
}

} // namespace

TEST_CASE("Decoder roundtrip on a synthetic 440 Hz sine WAV", "[decoder][synthetic]") {
    constexpr int srcRate          = 44100;
    constexpr double frequency     = 440.0;
    constexpr int durationSeconds  = 1;
    const int totalSamples         = srcRate * durationSeconds;

    std::vector<std::int16_t> mono(totalSamples);
    for (int i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / srcRate;
        const double v = 0.5 * std::sin(2.0 * std::numbers::pi * frequency * t);
        mono[i] = static_cast<std::int16_t>(v * 32767.0);
    }

    const auto wavPath = fs::temp_directory_path() / "fiddler_test_sine.wav";
    writeWav(wavPath, srcRate, 1, mono);

    fiddler::audio::Decoder decoder;
    REQUIRE(decoder.open(wavPath));

    const auto fmt = decoder.outputFormat();
    REQUIRE(fmt.channels   == 2);
    REQUIRE(fmt.sampleRate == 48000);

    std::vector<float> all;
    std::vector<float> chunk(4096);
    std::ptrdiff_t got = 0;
    int safetyMaxReads = 200000;
    while ((got = decoder.read({chunk.data(), chunk.size()})) > 0) {
        all.insert(all.end(), chunk.begin(), chunk.begin() + got);
        REQUIRE(--safetyMaxReads > 0);
    }
    REQUIRE(got == 0); // clean EOF, not error

    // 1 s @ 48 kHz stereo == 96 000 floats; resampler may shave a few off
    // for filter delay. Allow ~1 % slack on both sides.
    REQUIRE(all.size() >= 95'000);
    REQUIRE(all.size() <= 97'000);

    // De-interleave the left channel.
    std::vector<float> left;
    left.reserve(all.size() / 2);
    for (std::size_t i = 0; i + 1 < all.size(); i += 2) {
        left.push_back(all[i]);
    }

    // 440 Hz energy must dominate adjacent bins by a wide margin.
    const double e440 = goertzelEnergy(left, fmt.sampleRate, 440.0);
    const double e880 = goertzelEnergy(left, fmt.sampleRate, 880.0);
    const double e220 = goertzelEnergy(left, fmt.sampleRate, 220.0);

    INFO("e440=" << e440 << " e880=" << e880 << " e220=" << e220);
    REQUIRE(e440 > e880 * 100.0);
    REQUIRE(e440 > e220 * 100.0);

    std::error_code ec;
    fs::remove(wavPath, ec); // best-effort cleanup
}

TEST_CASE("Decoder seek lands at the requested position", "[decoder][synthetic][seek]") {
    constexpr int srcRate = 44100;
    constexpr int seconds = 3;
    const int total = srcRate * seconds;

    // A simple ramp so we can tell roughly where in the file we are.
    std::vector<std::int16_t> mono(total);
    for (int i = 0; i < total; ++i) {
        mono[i] = static_cast<std::int16_t>((i % 32768) - 16384);
    }

    const auto wavPath = fs::temp_directory_path() / "fiddler_test_ramp.wav";
    writeWav(wavPath, srcRate, 1, mono);

    fiddler::audio::Decoder decoder;
    REQUIRE(decoder.open(wavPath));

    REQUIRE(decoder.duration() >= std::chrono::milliseconds{2900});
    REQUIRE(decoder.duration() <= std::chrono::milliseconds{3100});

    REQUIRE(decoder.seek(std::chrono::milliseconds{1500}));

    // After seek we should still be able to pull samples without error.
    std::vector<float> chunk(4096);
    const auto got = decoder.read({chunk.data(), chunk.size()});
    REQUIRE(got > 0);

    std::error_code ec;
    fs::remove(wavPath, ec);
}
