// Integration tests for MainWindow. Drive a real file load → wait
// for the async overview build to complete → simulate a click on the
// waveform → verify the player has moved to the clicked position.
//
// Self-contained: writes a tiny PCM-WAV fixture (16-bit, 44.1 kHz
// stereo, 440 Hz sine, ~2 s) into the build's binary dir at startup,
// so the suite doesn't depend on a populated tests/data/audio/.

#include "audio/Player.h"
#include "qt_test_app.h"
#include "ui/MainWindow.h"
#include "ui/WaveformWidget.h"

#include <QSignalSpy>
#include <QString>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>

using fiddler::test::qtApp;
using fiddler::ui::MainWindow;
using fiddler::ui::WaveformWidget;

namespace {

namespace fs = std::filesystem;

// Write a minimal RIFF/WAVE file (16-bit PCM, little-endian) at
// `path`. Content is a 440 Hz sine wave on every channel.
void writeSineWav(const fs::path& path,
                  double durationSec,
                  int    sampleRate,
                  int    channels) {
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
        const auto s = static_cast<std::int16_t>(
            std::sin(2.0 * std::numbers::pi * 440.0 * t) * 16384);
        for (int c = 0; c < channels; ++c) put16(static_cast<std::uint16_t>(s));
    }
}

// Lazy-built fixture path so a single sine WAV is shared across the
// integration tests in this TU.
const fs::path& fixtureWav() {
    static const fs::path p = [] {
        const auto dir = fs::temp_directory_path() / "fiddler-tests";
        fs::create_directories(dir);
        const auto path = dir / "fixture-sine-2s.wav";
        if (!fs::exists(path)) {
            writeSineWav(path, /*durationSec=*/2.0,
                         /*sampleRate=*/44100, /*channels=*/2);
        }
        return path;
    }();
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Failure mode
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: load fails gracefully on a non-existent file",
          "[main-window][gui][integration]") {
    qtApp();
    MainWindow w;
    REQUIRE_FALSE(w.loadFile("/no/such/file.wav"));
}

// ---------------------------------------------------------------------------
// Full pipeline: open → overview ready → click → player seeks
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: loadFile drives the full pipeline and "
          "click-on-waveform seeks the player",
          "[main-window][gui][integration]") {
    qtApp();
    MainWindow w;
    w.show();
    REQUIRE(w.loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = w.findChild<WaveformWidget*>();
    REQUIRE(waveform != nullptr);

    // The build is async — no overview yet at this point.
    REQUIRE(waveform->overview() == nullptr);

    // Spin the GUI event loop until the queued setOverview lands.
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    const auto& ov = *waveform->overview();
    REQUIRE(ov.bucketCount() > 0);
    REQUIRE(ov.duration().count() > 0);

    // Player and overview agree on the file's duration.
    const auto durationMs = w.player().duration().count();
    REQUIRE(durationMs > 0);
    REQUIRE(durationMs == ov.duration().count());

    // Click ~25% in (x=250 of width=1000). Verify the click both fires
    // seekRequested with the right ms AND moves the player to it.
    waveform->resize(1000, 100);
    QSignalSpy spy(waveform, &WaveformWidget::seekRequested);
    QTest::mouseClick(waveform, Qt::LeftButton, Qt::NoModifier,
                      QPoint(250, 50));

    REQUIRE(spy.count() == 1);
    const auto seekMs     = spy.takeFirst().at(0).toLongLong();
    const auto expectedMs = durationMs / 4;
    REQUIRE(seekMs >= expectedMs - durationMs / 50);
    REQUIRE(seekMs <= expectedMs + durationMs / 50);

    // Player::seek is synchronous and updates the position anchor in
    // place, so position() returns the seek target immediately without
    // needing to spin the event loop.
    const auto playerPos = w.player().position().count();
    REQUIRE(playerPos >= seekMs - 10);
    REQUIRE(playerPos <= seekMs + 10);
}

// ---------------------------------------------------------------------------
// Re-loading clears + replaces the overview
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: opening a second file replaces the first overview",
          "[main-window][gui][integration]") {
    qtApp();
    MainWindow w;
    REQUIRE(w.loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = w.findChild<WaveformWidget*>();
    REQUIRE(waveform != nullptr);
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));
    const auto firstOverview = waveform->overview();

    // Reload. The old overview is cleared immediately; the new one
    // (a different shared_ptr) arrives once the worker finishes.
    REQUIRE(w.loadFile(QString::fromStdString(fixtureWav().string())));
    REQUIRE(waveform->overview() == nullptr);

    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));
    REQUIRE(waveform->overview() != firstOverview);
}
