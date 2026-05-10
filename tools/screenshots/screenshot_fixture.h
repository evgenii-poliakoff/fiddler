// Generates a small WAV file on disk for the screenshot tool to
// load via MainWindow::loadFile. Mirrors tests/wav_fixture.cpp's
// shape but tuned for visual interest in the waveform overview —
// an envelope-modulated sine produces visible amplitude variation
// across the time axis, which makes screenshots more illustrative
// than a flat sine band.
//
// MEMO: deliberately duplicated from tests/wav_fixture.cpp rather
// than promoting it to a shared utility. The tool is the second
// consumer; if a third wants the same fixture, that's the moment
// to extract.

#pragma once

#include <filesystem>

namespace fiddler::screenshots {

// First call generates the WAV into a stable temp path; subsequent
// calls return the same path. Thread-safe via C++'s magic statics.
const std::filesystem::path& generateFixtureWav();

} // namespace fiddler::screenshots
