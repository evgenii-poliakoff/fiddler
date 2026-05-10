#include "screenshot_fixture.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <numbers>

namespace fiddler::screenshots {

namespace {

void writeFixtureWav(const std::filesystem::path& path) {
    constexpr int    kSampleRate  = 44100;
    constexpr int    kChannels    = 2;
    constexpr double kDurationSec = 8.0;
    constexpr double kCarrierHz   = 220.0;       // low fiddle pitch

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

    const auto frames     = static_cast<std::uint32_t>(
        kDurationSec * kSampleRate);
    const auto byteRate   = static_cast<std::uint32_t>(
        kSampleRate * kChannels * 2);
    const auto blockAlign = static_cast<std::uint16_t>(kChannels * 2);
    const auto dataBytes  = frames * kChannels * 2;

    f.write("RIFF", 4);
    put32(36u + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(16u);
    put16(1);                 // PCM
    put16(static_cast<std::uint16_t>(kChannels));
    put32(static_cast<std::uint32_t>(kSampleRate));
    put32(byteRate);
    put16(blockAlign);
    put16(16);                // bits/sample
    f.write("data", 4);
    put32(dataBytes);

    // Envelope: four "phrases" with a soft attack-decay each, so the
    // waveform overview has visible amplitude variation across the
    // 8-second clip rather than a flat band. Tuned for visual
    // interest in screenshots, not for fidelity to any real tune.
    for (std::uint32_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kSampleRate;
        const double phrase = std::fmod(t, 2.0);
        const double envelope =
            std::pow(std::sin(std::numbers::pi * phrase / 2.0), 1.5);
        const double amp = envelope
            * std::sin(2.0 * std::numbers::pi * kCarrierHz * t);
        const auto sample = static_cast<std::int16_t>(amp * 16384);
        for (int c = 0; c < kChannels; ++c) {
            put16(static_cast<std::uint16_t>(sample));
        }
    }
}

} // namespace

const std::filesystem::path& generateFixtureWav() {
    namespace fs = std::filesystem;
    static const fs::path cached = [] {
        const auto dir = fs::temp_directory_path() / "fiddler-screenshots";
        fs::create_directories(dir);
        const auto path = dir / "fixture-8s.wav";
        if (!fs::exists(path)) {
            writeFixtureWav(path);
        }
        return path;
    }();
    return cached;
}

} // namespace fiddler::screenshots
