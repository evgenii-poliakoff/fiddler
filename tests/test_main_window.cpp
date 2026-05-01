// Integration tests for MainWindow. Drive a real file load → wait
// for the async overview build to complete → simulate a click on the
// waveform → verify the player has moved to the clicked position.
//
// Self-contained: writes a tiny PCM-WAV fixture (16-bit, 44.1 kHz
// stereo, 440 Hz sine, ~2 s) into the build's binary dir at startup,
// so the suite doesn't depend on a populated tests/data/audio/.

#include "audio/Player.h"
#include "qt_test_app.h"
#include "score/BarlineModel.h"
#include "ui/MainWindow.h"
#include "ui/StaffWidget.h"
#include "ui/WaveformWidget.h"
#include "wav_fixture.h"

#include <QComboBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QString>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>

using fiddler::test::fixtureWav;
using fiddler::test::qtApp;
using fiddler::ui::MainWindow;
using fiddler::ui::StaffWidget;
using fiddler::ui::WaveformWidget;

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
// Tap-then-Del flow: regression coverage for "I cannot delete the only
// barline I just placed via Del". Two compounding causes — Del only
// worked when a score widget was focused (focus tended to be on the
// transport buttons after a click), and a freshly-placed bar wasn't
// selected so Del had nothing to remove. Both fixes are exercised
// here.
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: tapping B auto-selects the newly-placed barline",
          "[main-window][gui][integration][tap-then-del]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    REQUIRE(waveform != nullptr);
    REQUIRE(staff    != nullptr);
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // No barlines yet → no selection.
    REQUIRE_FALSE(waveform->selectedBarline().has_value());
    REQUIRE_FALSE(staff->selectedBarline().has_value());

    // Tap B once. The placement should also become the active
    // selection in BOTH widgets (the staff via the mirror plumbing).
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(waveform->selectedBarline() == 0);
    REQUIRE(staff->selectedBarline()    == 0);
}

TEST_CASE("MainWindow: tap B then Del removes the bar without a click",
          "[main-window][gui][integration][tap-then-del]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // The exact scenario from the smoke-test bug report:
    //   1. open a file
    //   2. tap B once
    //   3. press Del — expect the bar to be gone
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 1);

    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->barlineModel().empty());
    REQUIRE_FALSE(waveform->selectedBarline().has_value());
}

TEST_CASE("MainWindow: Del shortcut works when focus is on the Play button",
          "[main-window][gui][integration][tap-then-del]") {
    // The original bug surfaced precisely because the user clicked
    // Play, then tapped B, then pressed Del — and Del did nothing.
    // The keyboard target was the Play button, which doesn't handle
    // Del. The window-level QShortcut we added must intercept.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform   = window->findChild<WaveformWidget*>("waveformWidget");
    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    REQUIRE(waveform   != nullptr);
    REQUIRE(playButton != nullptr);
    REQUIRE(stopButton != nullptr);
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // Click Play + Stop to mimic the real workflow and to land on a
    // deterministic position. Focus is now on stopButton.
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);
    REQUIRE(stopButton->hasFocus());

    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 1);

    // The keyboard target is still the Stop button. Without the
    // window-level Del shortcut, this keypress would be consumed
    // (or ignored) by the button rather than removing the bar.
    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->barlineModel().empty());
}

TEST_CASE("MainWindow: each tap shifts auto-selection to the latest bar",
          "[main-window][gui][integration][tap-then-del]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform  = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff     = window->findChild<StaffWidget*>("staffWidget");
    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    auto* playBtn   = window->findChild<QPushButton*>("playButton");
    auto* stopBtn   = window->findChild<QPushButton*>("stopButton");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));
    const auto durationMs = window->player().duration().count();

    // Lock the player to position 0 deterministically.
    QTest::mouseClick(playBtn, Qt::LeftButton);
    QTest::mouseClick(stopBtn, Qt::LeftButton);

    // Place at 0. Selection follows the placement.
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(waveform->selectedBarline() == 0);
    REQUIRE(staff->selectedBarline()    == 0);

    // Place at durationMs / 2. The new entry sorts to index 1; the
    // selection moves to it.
    posSlider->setValue(static_cast<int>(durationMs / 2));
    emit posSlider->sliderMoved(static_cast<int>(durationMs / 2));
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 2);
    REQUIRE(waveform->selectedBarline() == 1);
    REQUIRE(staff->selectedBarline()    == 1);

    // Place at durationMs / 4 — sorts BETWEEN the two existing
    // entries (index 1). Both pre-existing entries shift; selection
    // tracks the just-placed bar.
    posSlider->setValue(static_cast<int>(durationMs / 4));
    emit posSlider->sliderMoved(static_cast<int>(durationMs / 4));
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 3);
    REQUIRE(waveform->selectedBarline() == 1);  // the new middle bar
    REQUIRE(staff->selectedBarline()    == 1);

    // One Del peels the just-placed quarter-mark bar; the remaining
    // two are at 0 and durationMs/2.
    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->barlineModel().size() == 2);
    REQUIRE(window->barlineModel().barlines()[0] == 0);
    REQUIRE(window->barlineModel().barlines()[1] == durationMs / 2);
}

TEST_CASE("MainWindow: Del with nothing selected is a quiet no-op",
          "[main-window][gui][integration][tap-then-del]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // No bar placed yet → no selection. Pressing Del must not crash
    // or do anything to the (empty) model.
    REQUIRE(window->barlineModel().empty());
    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->barlineModel().empty());
}

TEST_CASE("MainWindow: starts with the default time-sig preset (Reel 4/4)",
          "[main-window][gui][integration]") {
    // Regression for a smoke-test finding: the staff's tune-type
    // label was blank on first launch because the combo's initial
    // index 0 wasn't synced to the model. The model should always
    // carry a populated tuneType so the staff has something to draw
    // above the time-signature digits, even before the user picks
    // from the combo or opens a file.
    qtApp();
    MainWindow w;
    const auto ts = w.barlineModel().timeSignature();
    REQUIRE(ts.numerator   == 4);
    REQUIRE(ts.denominator == 4);
    REQUIRE(ts.tuneType    == "Reel");
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

// ---------------------------------------------------------------------------
// Full session flow — drives every testable user interaction in one
// long test. Mirrors what an actual transcription session looks like.
// ---------------------------------------------------------------------------
//
// What we *can* simulate headlessly:
//   - file load (via the loadFile() seam — the OS file dialog isn't
//     reachable from QTest)
//   - tap-to-place ('B' key) and Ctrl+Z undo, with QShortcut routing
//   - clicks on transport buttons, the waveform, the staff
//   - keyboard input on focused widgets (Del, Esc, arrows)
//   - QComboBox selection changes via setCurrentIndex + activated()
//   - position-slider drag via QSlider::sliderMoved emission
//   - window close
//
// What we *cannot* verify on a no-audio CI host:
//   - actual audio playback (Player::play() is a no-op when there's
//     no PortAudio output device — we click Play and verify the UI
//     plumbing doesn't crash, not that sound came out)

TEST_CASE("MainWindow: full simulated user session — open, tap, undo, "
          "signature, click-seek, select+Del, slider-seek, close",
          "[main-window][gui][integration][session]") {
    qtApp();

    // 1. Open the window and load the fixture WAV. We hold the
    //    window via unique_ptr so step 9 (close) can drop it
    //    deterministically inside the test rather than at scope-exit.
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    const QString fixturePath = QString::fromStdString(fixtureWav().string());
    REQUIRE(window->loadFile(fixturePath));

    auto* waveform   = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff      = window->findChild<StaffWidget*>("staffWidget");
    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    auto* posSlider  = window->findChild<QSlider*>("positionSlider");
    auto* tuneCombo  = window->findChild<QComboBox*>("tuneTypeCombo");
    REQUIRE(waveform   != nullptr);
    REQUIRE(staff      != nullptr);
    REQUIRE(playButton != nullptr);
    REQUIRE(stopButton != nullptr);
    REQUIRE(posSlider  != nullptr);
    REQUIRE(tuneCombo  != nullptr);

    // 2. Wait for the async overview build.
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));
    const auto durationMs = window->player().duration().count();
    REQUIRE(durationMs > 0);
    REQUIRE(staff->durationMs() == durationMs);
    REQUIRE(window->barlineModel().empty());

    // 3. Click Play, then immediately click Stop. On a dev machine
    //    with a real audio device, Play actually starts playback —
    //    so by the time we get to step 4 the player position would
    //    have drifted past zero. Stop rewinds deterministically to
    //    0 on any host (audio or not), giving us a known-good
    //    starting position for tap-to-place.
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);
    REQUIRE(window->player().position().count() == 0);

    // 4. Tap-to-place. Send 'B'; the WindowShortcut on MainWindow
    //    intercepts the key before any focused child widget sees it,
    //    so the focused widget doesn't matter. The model receives
    //    add(player.position()) — which is 0 right now.
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(window->barlineModel().barlines()[0] == 0);

    // 5. Drag the position slider to mid-file, then tap B again.
    //    sliderMoved → onSeek → player.seek(target) — and because
    //    the player is Stopped, framesPlayed_ stays at 0, so
    //    position() stays exactly at the seek target. Deterministic.
    posSlider->setValue(static_cast<int>(durationMs / 2));
    emit posSlider->sliderMoved(static_cast<int>(durationMs / 2));
    REQUIRE(window->player().position().count()
            == durationMs / 2);
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 2);
    REQUIRE(window->barlineModel().barlines()[0] == 0);
    REQUIRE(window->barlineModel().barlines()[1] == durationMs / 2);

    // 6. Ctrl+Z undoes the most recent placement (the mid-file one).
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(window->barlineModel().barlines()[0] == 0);

    // Place a quarter-way barline so we have two for the
    // selection-mirror + Del tests below.
    posSlider->setValue(static_cast<int>(durationMs / 4));
    emit posSlider->sliderMoved(static_cast<int>(durationMs / 4));
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 2);

    // 6. Pick a different time signature via the combo. activated()
    //    is the right signal — currentIndexChanged would also fire
    //    on programmatic setCurrentIndex calls, but emitting
    //    activated explicitly mimics a real user pick.
    const int jigIndex = tuneCombo->findText("Single Jig (6/8)");
    REQUIRE(jigIndex >= 0);
    tuneCombo->setCurrentIndex(jigIndex);
    emit tuneCombo->activated(jigIndex);
    REQUIRE(window->barlineModel().timeSignature().numerator   == 6);
    REQUIRE(window->barlineModel().timeSignature().denominator == 8);
    REQUIRE(window->barlineModel().timeSignature().tuneType    == "Single Jig");

    // 7. Click on the waveform near the second (last) barline tick →
    //    it should select + seek, and the staff's selection should
    //    mirror. We pick the trailing barline on purpose so that the
    //    Del in step 8 invalidates the index (size==1 → 0 is valid;
    //    selecting trailing index 1 means removing it leaves index
    //    1 out of range, which the widget's contract clears).
    waveform->resize(1000, 100);
    staff->resize(1000,    80);
    const int xOfTrailingBar =
        waveform->msToX(durationMs / 4);   // index 1 in [0, durationMs/4]
    QTest::mouseClick(waveform, Qt::LeftButton, Qt::NoModifier,
                      QPoint(xOfTrailingBar + 1, 50));
    REQUIRE(waveform->selectedBarline() == 1);
    REQUIRE(staff->selectedBarline()    == 1);   // mirror landed

    // 8. Press Del while the staff has focus → barline removed,
    //    selection cleared in both views (index 1 is now out of range).
    staff->setFocus();
    QTest::keyClick(staff, Qt::Key_Delete);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(window->barlineModel().barlines()[0] == 0);
    REQUIRE_FALSE(waveform->selectedBarline().has_value());
    REQUIRE_FALSE(staff->selectedBarline().has_value());

    // 9. Drag the position slider — the player should follow.
    const int dragTargetMs = static_cast<int>(durationMs * 3 / 4);
    posSlider->setValue(dragTargetMs);
    emit posSlider->sliderMoved(dragTargetMs);
    REQUIRE(window->player().position().count() >= dragTargetMs - 10);
    REQUIRE(window->player().position().count() <= dragTargetMs + 10);

    // 10. Click Stop — position rewinds, transport state goes back
    //     to Stopped. Stop is null-safe even on no-audio hosts.
    QTest::mouseClick(stopButton, Qt::LeftButton);
    REQUIRE(window->player().position().count() == 0);

    // 11. Close the window. unique_ptr's reset() runs the destructor
    //     synchronously; if anything were leaking or double-deleting,
    //     this is where we'd find out.
    window->close();
    window.reset();
    SUCCEED();
}
