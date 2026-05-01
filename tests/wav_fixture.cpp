#include "wav_fixture.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>

namespace fiddler::test {

namespace {

// MEMO: 16-bit PCM, little-endian. The minimal RIFF/WAVE writer below
// hard-codes the format; if a future fixture needs a different bit
// depth or sample format, factor a real WAV writer instead of
// generalising in place.
void writeSineWav(const std::filesystem::path& path,
                  double durationSec,
                  int    sampleRate,
                  int    channels)
{
    std::ofstream f(path, std::ios::binary);

    const auto put32 = [&](std::uint32_t v) {
        unsigned char b[4]{
            static_cast<unsigned char>(v),
            static_cast<unsigned char>(v >> 8),
            static_cast<unsigned char>(v >> 16),
            static_cast<unsigned char>(v >> 24)};
        f.write(reinterpret_cast<const char*>(b), 4);
    };
    const auto put16 = [&](std::uint16_t v) {
        unsigned char b[2]{
            static_cast<unsigned char>(v),
            static_cast<unsigned char>(v >> 8)};
        f.write(reinterpret_cast<const char*>(b), 2);
    };

    const auto frames     = static_cast<std::uint32_t>(durationSec * sampleRate);
    const auto byteRate   = static_cast<std::uint32_t>(sampleRate * channels * 2);
    const auto blockAlign = static_cast<std::uint16_t>(channels * 2);
    const auto dataBytes  = frames * channels * 2;

    f.write("RIFF", 4);
    put32(36u + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(16u);
    put16(1);                 // PCM
    put16(static_cast<std::uint16_t>(channels));
    put32(static_cast<std::uint32_t>(sampleRate));
    put32(byteRate);
    put16(blockAlign);
    put16(16);                // bits/sample
    f.write("data", 4);
    put32(dataBytes);

    for (std::uint32_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / sampleRate;
        const auto sample = static_cast<std::int16_t>(
            std::sin(2.0 * std::numbers::pi * 440.0 * t) * 16384);
        for (int c = 0; c < channels; ++c) {
            put16(static_cast<std::uint16_t>(sample));
        }
    }
}

} // namespace

const std::filesystem::path& fixtureWav() {
    namespace fs = std::filesystem;
    // MEMO: function-local static — first call generates the WAV
    // into a stable temp path; subsequent calls return the same
    // path. Concurrent first-calls from different tests are
    // serialised by C++'s magic-statics rule, so no extra mutex.
    static const fs::path cached = [] {
        const auto dir = fs::temp_directory_path() / "fiddler-tests";
        fs::create_directories(dir);
        const auto path = dir / "fixture-sine-2s.wav";
        if (!fs::exists(path)) {
            writeSineWav(path, /*durationSec=*/2.0,
                         /*sampleRate=*/44100, /*channels=*/2);
        }
        return path;
    }();
    return cached;
}

} // namespace fiddler::test
