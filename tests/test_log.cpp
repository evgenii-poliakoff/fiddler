// Verifies the logging facade: levels, category filtering, parseLevel,
// and that a real log call from Decoder::open is captured.
//
// These tests rely on log::setCapture() — a test-only hook that delivers
// every emitted log line to a callback. We never include spdlog headers
// here; the facade is the only seam between application code and
// whichever logging library backs it.

#include "audio/Decoder.h"
#include "util/Log.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Captured {
    fiddler::log::Level level;
    std::string         category;
    std::string         message;
};

// Sets the facade up with the given config and installs a capture hook
// that pushes every emitted line into `out`. Returns a guard that
// detaches the hook on destruction so tests don't leak hooks across
// each other.
struct CaptureGuard {
    std::vector<Captured>& out;
    explicit CaptureGuard(std::vector<Captured>& sink) : out(sink) {
        fiddler::log::setCapture(
            [&sink](fiddler::log::Level lvl,
                    std::string_view cat,
                    std::string_view msg) {
                sink.push_back({lvl, std::string{cat}, std::string{msg}});
            });
    }
    ~CaptureGuard() { fiddler::log::setCapture(nullptr); }
    CaptureGuard(const CaptureGuard&)            = delete;
    CaptureGuard& operator=(const CaptureGuard&) = delete;
};

void initFacade(fiddler::log::Level level, std::string filter = "*") {
    fiddler::log::Config cfg;
    cfg.level  = level;
    cfg.filter = std::move(filter);
    fiddler::log::init(cfg);
}

} // namespace

TEST_CASE("parseLevel handles the documented spellings", "[log]") {
    using fiddler::log::Level;
    using fiddler::log::parseLevel;
    REQUIRE(parseLevel("trace")   == Level::Trace);
    REQUIRE(parseLevel("DEBUG")   == Level::Debug);
    REQUIRE(parseLevel("Info")    == Level::Info);
    REQUIRE(parseLevel("warn")    == Level::Warn);
    REQUIRE(parseLevel("warning") == Level::Warn);
    REQUIRE(parseLevel("error")   == Level::Error);
    REQUIRE(parseLevel("off")     == Level::Off);
    REQUIRE_FALSE(parseLevel("verbose").has_value());
    REQUIRE_FALSE(parseLevel("").has_value());
}

TEST_CASE("threshold suppresses below-level messages", "[log]") {
    initFacade(fiddler::log::Level::Warn);
    std::vector<Captured> captured;
    CaptureGuard guard(captured);

    FLOG_TRACE("x", "trace line");
    FLOG_DEBUG("x", "debug line");
    FLOG_INFO ("x", "info line");
    FLOG_WARN ("x", "warn line");
    FLOG_ERROR("x", "error line");

    REQUIRE(captured.size() == 2);
    REQUIRE(captured[0].level   == fiddler::log::Level::Warn);
    REQUIRE(captured[0].message == "warn line");
    REQUIRE(captured[1].level   == fiddler::log::Level::Error);
    REQUIRE(captured[1].message == "error line");
}

TEST_CASE("category filter matches prefixes via .* glob", "[log]") {
    initFacade(fiddler::log::Level::Trace, "player.*");
    std::vector<Captured> captured;
    CaptureGuard guard(captured);

    FLOG_INFO("decoder",       "should be filtered out");
    FLOG_INFO("player",        "matches player exactly");
    FLOG_INFO("player.thread", "matches player.thread");
    FLOG_INFO("ui",            "should also be filtered");

    REQUIRE(captured.size() == 2);
    REQUIRE(captured[0].category == "player");
    REQUIRE(captured[1].category == "player.thread");
}

TEST_CASE("category filter accepts a comma-separated list of globs",
          "[log]") {
    // MEMO: regression for the bug surfaced during step-5.5 smoke
    // testing — the documented filter form ("ui.*,player,waveform")
    // had been silently treated as a single literal category name
    // and dropped every log line. The fix splits on commas and
    // any-matches; this test pins that behaviour.
    initFacade(fiddler::log::Level::Trace,
               "ui.*,player,waveform,score");
    std::vector<Captured> captured;
    CaptureGuard guard(captured);

    FLOG_INFO("decoder",       "filtered (not in any pattern)");
    FLOG_INFO("ui",            "matches ui.* prefix form");
    FLOG_INFO("ui.score",      "matches ui.* subtree");
    FLOG_INFO("player",        "matches literal 'player'");
    FLOG_INFO("player.thread", "filtered ('player' is exact, not a prefix)");
    FLOG_INFO("waveform",      "matches literal 'waveform'");
    FLOG_INFO("score",         "matches literal 'score'");

    REQUIRE(captured.size() == 5);
    REQUIRE(captured[0].category == "ui");
    REQUIRE(captured[1].category == "ui.score");
    REQUIRE(captured[2].category == "player");
    REQUIRE(captured[3].category == "waveform");
    REQUIRE(captured[4].category == "score");
}

TEST_CASE("category filter trims whitespace around comma entries", "[log]") {
    initFacade(fiddler::log::Level::Trace, "  ui.*  ,  player  ");
    std::vector<Captured> captured;
    CaptureGuard guard(captured);

    FLOG_INFO("ui.score",      "matches first entry, whitespace ignored");
    FLOG_INFO("player",        "matches second entry, whitespace ignored");
    FLOG_INFO("decoder",       "filtered");

    REQUIRE(captured.size() == 2);
}

TEST_CASE("Decoder::open emits an INFO line on success", "[log][decoder]") {
    namespace fs = std::filesystem;
    const auto wavPath = fs::temp_directory_path() / "fiddler_log_test.wav";

    std::vector<std::int16_t> samples(8000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>((i * 257) % 32767 - 16383);
    }

    auto write32 = [](std::ofstream& f, std::uint32_t v) {
        char b[4] = { char(v), char(v >> 8), char(v >> 16), char(v >> 24) };
        f.write(b, 4);
    };
    auto write16 = [](std::ofstream& f, std::uint16_t v) {
        char b[2] = { char(v), char(v >> 8) };
        f.write(b, 2);
    };

    std::ofstream out(wavPath, std::ios::binary);
    const std::uint32_t dataSize = samples.size() * sizeof(std::int16_t);
    out.write("RIFF", 4); write32(out, 36 + dataSize); out.write("WAVE", 4);
    out.write("fmt ", 4); write32(out, 16); write16(out, 1); write16(out, 1);
    write32(out, 8000); write32(out, 16000); write16(out, 2); write16(out, 16);
    out.write("data", 4); write32(out, dataSize);
    out.write(reinterpret_cast<const char*>(samples.data()), dataSize);
    out.close();

    initFacade(fiddler::log::Level::Info);
    std::vector<Captured> captured;
    CaptureGuard guard(captured);

    fiddler::audio::Decoder decoder;
    REQUIRE(decoder.open(wavPath));

    bool sawOpenedLine = false;
    for (const auto& c : captured) {
        if (c.category == "decoder"
            && c.level == fiddler::log::Level::Info
            && c.message.find("opened") != std::string::npos) {
            sawOpenedLine = true;
        }
    }
    REQUIRE(sawOpenedLine);

    std::error_code ec;
    fs::remove(wavPath, ec);
}
