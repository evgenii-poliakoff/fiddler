// Corpus test — exercises the Decoder against any audio files dropped into
// tests/data/audio/. Each file gets its own DYNAMIC_SECTION, so one bad
// file doesn't poison the rest of the run.
//
// If the directory is missing or empty we emit a Catch2 WARN and pass —
// the test is opportunistic, not mandatory. Drop your fiddle recordings
// (any of .wav/.flac/.mp3/.ogg/.m4a/.aac/.opus) in there to exercise it.

#include "audio/Decoder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#ifndef FIDDLER_TEST_DATA_DIR
#  define FIDDLER_TEST_DATA_DIR "tests/data/audio"
#endif

namespace fs = std::filesystem;

namespace {

const std::set<std::string>& audioExtensions() {
    static const std::set<std::string> exts = {
        ".wav", ".flac", ".mp3", ".ogg", ".m4a",
        ".aac", ".opus", ".wma", ".aiff", ".aif",
    };
    return exts;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<fs::path> collectAudioFiles(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (audioExtensions().count(toLower(entry.path().extension().string()))) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("Decoder corpus: open and decode every file in tests/data/audio/",
          "[decoder][corpus]") {
    const fs::path corpusDir{FIDDLER_TEST_DATA_DIR};
    const auto files = collectAudioFiles(corpusDir);

    if (files.empty()) {
        WARN("No audio files found in " << corpusDir
             << ". Drop .wav / .flac / .mp3 / .ogg / .m4a / .aac / .opus files "
                "in that directory to exercise the decoder against real input. "
                "Skipping corpus test.");
        SUCCEED("Corpus directory empty — skipped.");
        return;
    }

    INFO("Corpus directory: " << corpusDir);
    INFO("Files found:      " << files.size());

    for (const auto& file : files) {
        DYNAMIC_SECTION("Decode " << file.filename().string()) {
            fiddler::audio::Decoder decoder;
            const bool opened = decoder.open(file);
            INFO("File:        " << file);
            INFO("Last error:  " << decoder.lastError());
            REQUIRE(opened);

            const auto fmt = decoder.outputFormat();
            REQUIRE(fmt.sampleRate > 0);
            REQUIRE(fmt.channels   > 0);

            std::vector<float> chunk(4096);
            std::size_t totalSamples = 0;
            std::ptrdiff_t got = 0;

            // Cap reads so a pathological input can't hang the suite.
            // 4096 samples / iteration · 2 000 000 == ~85 000 s of audio,
            // far past anything we'd realistically transcribe.
            int safetyReads = 2'000'000;

            while ((got = decoder.read({chunk.data(), chunk.size()})) > 0) {
                totalSamples += static_cast<std::size_t>(got);
                if (--safetyReads <= 0) {
                    FAIL("Read loop exceeded safety cap — possible infinite loop");
                }
            }
            REQUIRE(got == 0); // clean EOF, not error

            const double seconds = static_cast<double>(totalSamples)
                                 / fmt.channels / fmt.sampleRate;
            INFO("Decoded "      << totalSamples << " samples ("
                 << seconds      << " s)");
            REQUIRE(totalSamples > 0);

            // Cross-check decoded length against container-reported duration
            // when the container provides one. Allow 5 % slop — VBR formats
            // and trailing silence trimming legitimately disagree at the edges.
            const auto reported = decoder.duration();
            if (reported.count() > 0) {
                const double reportedSec = reported.count() / 1000.0;
                INFO("Reported duration: " << reportedSec << " s");
                REQUIRE(seconds >= reportedSec * 0.95);
                REQUIRE(seconds <= reportedSec * 1.05);
            }
        }
    }
}
