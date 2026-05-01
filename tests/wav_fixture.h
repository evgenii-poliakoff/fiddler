// Tiny shared helper that hands tests a path to a self-contained
// PCM-WAV fixture (16-bit, 44.1 kHz stereo, 440 Hz sine, ~2 s).
//
// MEMO: keeps the test suite independent of the .gitignored
// tests/data/audio/ corpus. The fixture is generated on first call
// into the system temp directory and reused; subsequent calls return
// the same path. Used by test_main_window.cpp and test_event_logging.cpp.

#pragma once

#include <filesystem>

namespace fiddler::test {

// Returns the path of the cached fixture WAV, generating it on first
// call. Safe to call from multiple tests in the same process.
const std::filesystem::path& fixtureWav();

} // namespace fiddler::test
