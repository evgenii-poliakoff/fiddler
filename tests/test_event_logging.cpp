// Tests for the UI event-logging contract.
//
// MEMO: see memory/feedback_logs_drive_tests.md for the working
// agreement these tests enforce — every user-driven UI action emits
// one debug-level log line that includes the action verb, the
// parameters, and the resulting model state. The tests below pin
// each line so a refactor that changes the format (or drops a line)
// surfaces immediately.
//
// Each TEST_CASE follows the same structure:
//   1. Install a capture hook with `CapturedLogs` (RAII).
//   2. Set up a MainWindow, drive a single user action through Qt
//      event flow.
//   3. Assert that the captured log contains the expected line(s).
//
// MEMO[refactor]: when changing a log message's wording, search
// `tests/test_event_logging.cpp` for the substring you're changing
// and adjust the matching test in lockstep. The logging tests are
// here precisely so the format is treated as a public contract,
// not an implementation detail.

#include "qt_test_app.h"
#include "score/BarlineModel.h"
#include "ui/MainWindow.h"
#include "ui/StaffWidget.h"
#include "ui/WaveformWidget.h"
#include "util/Log.h"
#include "wav_fixture.h"

#include <QComboBox>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using fiddler::test::fixtureWav;
using fiddler::test::qtApp;
using fiddler::ui::MainWindow;
using fiddler::ui::StaffWidget;
using fiddler::ui::WaveformWidget;

namespace {

// MEMO: minimal RAII capture of log emissions during a test. Installs
// the capture hook on construction, removes it on destruction. The
// log subsystem is initialised at Debug level so our `FLOG_DEBUG`
// lines actually fire — by default the threshold is Warn, which
// would silently drop everything we want to assert on.
class CapturedLogs {
public:
    struct Entry {
        std::string category;
        std::string message;
    };

    CapturedLogs() {
        // MEMO: filter `*` so we capture every category, then we
        // post-filter in the assertion helpers. Filtering here on
        // ui.* would only be a perf optimisation; the snapshot is
        // small and tests run fast either way.
        fiddler::log::init({fiddler::log::Level::Debug, "*", std::nullopt});
        fiddler::log::setCapture(
            [this](fiddler::log::Level /*lvl*/,
                   std::string_view category,
                   std::string_view message) {
                std::lock_guard<std::mutex> lock(mu_);
                entries_.push_back({std::string(category),
                                    std::string(message)});
            });
    }

    ~CapturedLogs() {
        fiddler::log::setCapture(nullptr);
    }

    CapturedLogs(const CapturedLogs&)            = delete;
    CapturedLogs& operator=(const CapturedLogs&) = delete;

    [[nodiscard]] std::vector<Entry> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.clear();
    }

private:
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
};

// Returns true if any captured entry has the given category and a
// message containing the given substring. The substring match keeps
// these tests resilient to small wording changes (e.g. extra spaces,
// reordered fields) while still pinning the load-bearing bits.
bool containsLog(const std::vector<CapturedLogs::Entry>& entries,
                 std::string_view category,
                 std::string_view substring)
{
    return std::any_of(entries.begin(), entries.end(),
        [&](const CapturedLogs::Entry& e) {
            return e.category == category
                && e.message.find(substring) != std::string::npos;
        });
}

// Number of captured entries matching (category, substring).
std::size_t countLog(const std::vector<CapturedLogs::Entry>& entries,
                     std::string_view category,
                     std::string_view substring)
{
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(),
            [&](const CapturedLogs::Entry& e) {
                return e.category == category
                    && e.message.find(substring) != std::string::npos;
            }));
}

// Build a MainWindow with the fixture loaded and the overview built.
// Returns the window plus pointers to the widgets the tests need.
struct LoadedWindow {
    std::unique_ptr<MainWindow> window;
    WaveformWidget*             waveform   = nullptr;
    StaffWidget*                staff      = nullptr;
    QPushButton*                playButton = nullptr;
    QPushButton*                stopButton = nullptr;
    QSlider*                    posSlider  = nullptr;
    QComboBox*                  tuneCombo  = nullptr;
};

LoadedWindow makeLoadedWindow() {
    LoadedWindow lw;
    lw.window = std::make_unique<MainWindow>();
    lw.window->show();
    (void)QTest::qWaitForWindowExposed(lw.window.get());

    const QString fixturePath =
        QString::fromStdString(fixtureWav().string());
    REQUIRE(lw.window->loadFile(fixturePath));

    lw.waveform   = lw.window->findChild<WaveformWidget*>("waveformWidget");
    lw.staff      = lw.window->findChild<StaffWidget*>("staffWidget");
    lw.playButton = lw.window->findChild<QPushButton*>("playButton");
    lw.stopButton = lw.window->findChild<QPushButton*>("stopButton");
    lw.posSlider  = lw.window->findChild<QSlider*>("positionSlider");
    lw.tuneCombo  = lw.window->findChild<QComboBox*>("tuneTypeCombo");

    REQUIRE(lw.waveform   != nullptr);
    REQUIRE(lw.staff      != nullptr);
    REQUIRE(lw.playButton != nullptr);
    REQUIRE(lw.stopButton != nullptr);
    REQUIRE(lw.posSlider  != nullptr);
    REQUIRE(lw.tuneCombo  != nullptr);

    REQUIRE(QTest::qWaitFor(
        [&]() { return lw.waveform->overview() != nullptr; }, 5000));

    return lw;
}

} // namespace

// ---------------------------------------------------------------------------
// File category — open success / failure / cancel / close
// ---------------------------------------------------------------------------

TEST_CASE("ui.file: successful loadFile logs path + duration + audio flag",
          "[event-logging][gui][ui.file]") {
    qtApp();
    CapturedLogs logs;

    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    // MEMO: the substrings pinned here are the load-bearing fields —
    // path (so the log can identify which file), duration (so a
    // log-replay test knows the file's length), and the audio flag
    // (so we know whether playback was usable).
    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.file", "open:"));
    REQUIRE(containsLog(entries, "ui.file", "duration="));
    REQUIRE(containsLog(entries, "ui.file", "audio="));
}

TEST_CASE("ui.file: failed loadFile logs path",
          "[event-logging][gui][ui.file]") {
    qtApp();
    CapturedLogs logs;

    MainWindow w;
    REQUIRE_FALSE(w.loadFile("/no/such/file.wav"));

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.file", "failed"));
    REQUIRE(containsLog(entries, "ui.file", "/no/such/file.wav"));
}

TEST_CASE("ui.file: closeEvent logs 'close'",
          "[event-logging][gui][ui.file]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());

    CapturedLogs logs;
    window->close();
    // MEMO: close() is synchronous (calls closeEvent inline), so by
    // the time it returns the log line is already captured. No
    // event-loop spin needed.

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.file", "close"));
}

// ---------------------------------------------------------------------------
// Transport category — play / pause / stop / auto-pause / position-slider
// ---------------------------------------------------------------------------

TEST_CASE("ui.transport: clicking Play logs the source position",
          "[event-logging][gui][ui.transport]") {
    qtApp();
    auto lw = makeLoadedWindow();
    CapturedLogs logs;

    QTest::mouseClick(lw.playButton, Qt::LeftButton);

    const auto entries = logs.snapshot();
    // MEMO: we don't pin the position value (it's 0 here, but on a
    // host with audio it'd drift); the verb + "from=" suffix is the
    // contract.
    REQUIRE(containsLog(entries, "ui.transport", "play from="));
}

TEST_CASE("ui.transport: clicking Pause logs the position at pause",
          "[event-logging][gui][ui.transport]") {
    qtApp();
    auto lw = makeLoadedWindow();

    // Reach the Playing state first (Play toggles between Play and
    // Pause). On a no-audio host the Player won't actually play, but
    // the UI flow toggles regardless — see the no-audio path in
    // Player::load.
    QTest::mouseClick(lw.playButton, Qt::LeftButton);

    CapturedLogs logs;
    QTest::mouseClick(lw.playButton, Qt::LeftButton);  // now pause

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.transport", "pause at="));
}

TEST_CASE("ui.transport: clicking Stop logs 'stop rewind=0'",
          "[event-logging][gui][ui.transport]") {
    qtApp();
    auto lw = makeLoadedWindow();
    CapturedLogs logs;

    QTest::mouseClick(lw.stopButton, Qt::LeftButton);

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.transport", "stop rewind=0"));
}

TEST_CASE("ui.transport: dragging the position slider logs 'seek via=position-slider'",
          "[event-logging][gui][ui.transport]") {
    qtApp();
    auto lw = makeLoadedWindow();

    CapturedLogs logs;
    // sliderMoved is the user-driven signal (programmatic setValue
    // fires valueChanged but not sliderMoved). Emitting it directly
    // is the cleanest way to simulate the slider drag from a test.
    emit lw.posSlider->sliderMoved(1000);

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.transport", "via=position-slider"));
    REQUIRE(containsLog(entries, "ui.transport", "ms=1000"));
}

// ---------------------------------------------------------------------------
// Tempo category — every value step is logged
// ---------------------------------------------------------------------------

TEST_CASE("ui.tempo: each tempo change logs percent + ratio",
          "[event-logging][gui][ui.tempo]") {
    qtApp();
    auto lw = makeLoadedWindow();
    auto* tempoSlider =
        lw.window->findChild<QSlider*>("tempoSlider");
    REQUIRE(tempoSlider != nullptr);

    CapturedLogs logs;
    // Setting the value triggers valueChanged → onTempoChanged →
    // FLOG_DEBUG. We deliberately don't filter by isSliderDown here —
    // the contract is "every applied tempo change leaves a log line".
    tempoSlider->setValue(50);

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.tempo", "tempo=50%"));
    REQUIRE(containsLog(entries, "ui.tempo", "ratio="));
}

// ---------------------------------------------------------------------------
// Score category — tap-place, undo, time-sig, select, delete, seek-by-click
// ---------------------------------------------------------------------------

TEST_CASE("ui.score: tap-place logs ms + index + size",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();

    // Land on a deterministic position-zero state regardless of host
    // audio (Player::stop rewinds even without an audio device).
    QTest::mouseClick(lw.playButton, Qt::LeftButton);
    QTest::mouseClick(lw.stopButton, Qt::LeftButton);

    CapturedLogs logs;
    QTest::keyClick(lw.window.get(), Qt::Key_B);

    const auto entries = logs.snapshot();
    // MEMO: the load-bearing bits — verb "tap-place" + the resulting
    // index ("index=0" since this is the first placement) + size.
    REQUIRE(containsLog(entries, "ui.score", "tap-place ms="));
    REQUIRE(containsLog(entries, "ui.score", "index=0"));
    REQUIRE(containsLog(entries, "ui.score", "size=1"));
}

TEST_CASE("ui.score: tap-place at duplicate ms is logged as 'ignored'",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();

    QTest::mouseClick(lw.playButton, Qt::LeftButton);
    QTest::mouseClick(lw.stopButton, Qt::LeftButton);

    // First tap places at 0; second tap at the same position should
    // be rejected by the model and logged as ignored. The model's
    // duplicate-rejection contract (BarlineModel::add returns the
    // existing index) is what makes this case observable in the log.
    QTest::keyClick(lw.window.get(), Qt::Key_B);

    CapturedLogs logs;
    QTest::keyClick(lw.window.get(), Qt::Key_B);

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.score", "ignored (duplicate)"));
}

TEST_CASE("ui.score: undo-last logs success and empty no-op cases",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();

    QTest::mouseClick(lw.playButton, Qt::LeftButton);
    QTest::mouseClick(lw.stopButton, Qt::LeftButton);
    QTest::keyClick(lw.window.get(), Qt::Key_B);

    SECTION("with at least one barline placed: 'undo-last size=…'") {
        CapturedLogs logs;
        QTest::keyClick(lw.window.get(), Qt::Key_Z, Qt::ControlModifier);

        const auto entries = logs.snapshot();
        REQUIRE(containsLog(entries, "ui.score", "undo-last size=0"));
        REQUIRE_FALSE(containsLog(entries, "ui.score", "no-op"));
    }

    SECTION("with empty model: 'undo-last empty (no-op)'") {
        // Drain the one bar we placed, then try undo on empty.
        QTest::keyClick(lw.window.get(), Qt::Key_Z, Qt::ControlModifier);

        CapturedLogs logs;
        QTest::keyClick(lw.window.get(), Qt::Key_Z, Qt::ControlModifier);

        const auto entries = logs.snapshot();
        REQUIRE(containsLog(entries, "ui.score", "undo-last empty"));
    }
}

TEST_CASE("ui.score: time-sig pick logs label + numerator + denominator",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();
    CapturedLogs logs;

    // setCurrentIndex doesn't emit `activated`; emit it explicitly to
    // simulate a user pick.
    const int jigIndex = lw.tuneCombo->findText("Single Jig (6/8)");
    REQUIRE(jigIndex >= 0);
    lw.tuneCombo->setCurrentIndex(jigIndex);
    emit lw.tuneCombo->activated(jigIndex);

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.score", "time-sig label=Single Jig"));
    REQUIRE(containsLog(entries, "ui.score", "numerator=6"));
    REQUIRE(containsLog(entries, "ui.score", "denominator=8"));
}

TEST_CASE("ui.score: clicking a barline tick logs 'seek via=waveform-click'",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();

    QTest::mouseClick(lw.playButton, Qt::LeftButton);
    QTest::mouseClick(lw.stopButton, Qt::LeftButton);
    QTest::keyClick(lw.window.get(), Qt::Key_B);

    CapturedLogs logs;

    // Click on the placed barline (it's at 0 ms → x=0 in waveform).
    lw.waveform->resize(800, 100);
    QTest::mouseClick(lw.waveform, Qt::LeftButton, Qt::NoModifier,
                      QPoint(2, 50));

    const auto entries = logs.snapshot();
    // Two consequences of the click — both are user-relevant log
    // events and both should appear.
    REQUIRE(containsLog(entries, "ui.score", "seek"));
    REQUIRE(containsLog(entries, "ui.score", "via=waveform-click"));
}

TEST_CASE("ui.score: clicking on the staff logs 'seek via=staff-click'",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();

    CapturedLogs logs;
    lw.staff->resize(800, 80);
    QTest::mouseClick(lw.staff, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 40));

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.score", "seek"));
    REQUIRE(containsLog(entries, "ui.score", "via=staff-click"));
}

TEST_CASE("ui.score: selection mirror logs *one* line, not two",
          "[event-logging][gui][ui.score]") {
    // MEMO: this is the regression test for the mirroringSelection_
    // guard in MainWindow. Without the guard, a single user click on
    // the waveform would log "select via=waveform" *and* "select via=
    // staff" because the mirror triggers a second
    // barlineSelectionChanged from the staff. The guard makes the
    // mirror one-shot, so only the originating direction logs.
    qtApp();
    auto lw = makeLoadedWindow();
    QTest::mouseClick(lw.playButton, Qt::LeftButton);
    QTest::mouseClick(lw.stopButton, Qt::LeftButton);
    QTest::keyClick(lw.window.get(), Qt::Key_B);  // places + auto-selects

    CapturedLogs logs;

    // Clear the selection first, then click on the bar to re-select
    // it. The click is the user action whose logging we're pinning.
    lw.waveform->setFocus();
    QTest::keyClick(lw.waveform, Qt::Key_Escape);

    logs.clear();
    lw.waveform->resize(800, 100);
    QTest::mouseClick(lw.waveform, Qt::LeftButton, Qt::NoModifier,
                      QPoint(2, 50));

    const auto entries = logs.snapshot();
    REQUIRE(countLog(entries, "ui.score", "via=waveform") >= 1);
    REQUIRE(countLog(entries, "ui.score", "via=staff")    == 0);
}

TEST_CASE("ui.score: window-level Del shortcut logs 'delete via=window-shortcut'",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();

    QTest::mouseClick(lw.playButton, Qt::LeftButton);
    QTest::mouseClick(lw.stopButton, Qt::LeftButton);
    QTest::keyClick(lw.window.get(), Qt::Key_B);

    CapturedLogs logs;
    QTest::keyClick(lw.window.get(), Qt::Key_Delete);

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.score", "delete index=0"));
    REQUIRE(containsLog(entries, "ui.score", "via=window-shortcut"));
    REQUIRE(containsLog(entries, "ui.score", "size=0"));
}

TEST_CASE("ui.score: Del with no selection logs 'no-selection'",
          "[event-logging][gui][ui.score]") {
    qtApp();
    auto lw = makeLoadedWindow();

    CapturedLogs logs;
    // No tap placed → no selection. Shortcut should fire and log a
    // no-op variant.
    QTest::keyClick(lw.window.get(), Qt::Key_Delete);

    const auto entries = logs.snapshot();
    REQUIRE(containsLog(entries, "ui.score", "no-selection"));
}
