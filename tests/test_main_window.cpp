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
#include "score/MarkerModel.h"
#include "ui/MainWindow.h"
#include "ui/ProjectViewerDock.h"
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
using fiddler::ui::ProjectViewerDock;
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

// ---------------------------------------------------------------------------
// Step 5.5 — markers + project viewer dock integration
// ---------------------------------------------------------------------------
//
// MEMO[refactor]: each TEST_CASE pins one rule that crosses model /
// widgets / dock / shortcuts. When refactoring any single layer
// (e.g. simplifying MarkerModel, redoing the dock), read the
// `MEMO:` comments in each test to know which assertions are the
// load-bearing ones.

TEST_CASE("MainWindow: starts with an empty MarkerModel and an attached ProjectViewerDock",
          "[main-window][gui][integration][markers]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    REQUIRE(window->markerModel().empty());

    // The dock is constructed in MainWindow::buildCentralWidget and
    // added via addDockWidget(RightDockWidgetArea, ...). Tests find
    // it via objectName so they don't depend on Qt's internal dock
    // layout.
    auto* dock = window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(dock != nullptr);
    REQUIRE(dock->markerModel() != nullptr);
}

TEST_CASE("MainWindow: 'M' shortcut places a marker at the player's position",
          "[main-window][gui][integration][markers]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    const QString fixturePath =
        QString::fromStdString(fixtureWav().string());
    REQUIRE(window->loadFile(fixturePath));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // Lock the player to position 0 so the assertion is deterministic
    // on hosts with audio (Stop rewinds even when audio is absent).
    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);

    QTest::keyClick(window.get(), Qt::Key_M);

    REQUIRE(window->markerModel().size() == 1);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 0);
    // MEMO: load-bearing — the auto-name format is the user-visible
    // contract for first-launch markers. If you change it, update
    // memory/project_tap_to_place.md and the equivalent assertion
    // in test_marker_model.cpp in lockstep.
    REQUIRE(window->markerModel().markers()[0].name == "Mark 1");
}

TEST_CASE("MainWindow: tap M auto-selects the new marker across waveform / staff / dock",
          "[main-window][gui][integration][markers]") {
    // MEMO: three-way mirror — selection of the freshly-placed
    // marker propagates from the originating widget (waveform, in
    // this case, since onTapMarker calls waveform_->setSelectedMarkerId)
    // to the staff and the dock via mirroringSelection_-guarded slots.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    auto* dock     =
        window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    QTest::mouseClick(window->findChild<QPushButton*>("playButton"),
                      Qt::LeftButton);
    QTest::mouseClick(window->findChild<QPushButton*>("stopButton"),
                      Qt::LeftButton);
    QTest::keyClick(window.get(), Qt::Key_M);

    const auto id = window->markerModel().markers()[0].id;
    REQUIRE(waveform->selectedMarkerId() == id);
    REQUIRE(staff->selectedMarkerId()    == id);
    REQUIRE(dock->selectedMarkerId()     == id);
}

TEST_CASE("MainWindow: combined Ctrl+Z LIFO peels the last placement regardless of kind",
          "[main-window][gui][integration][markers][undo]") {
    // The user's mental model: "Z = undo last", regardless of
    // whether last was a barline or a marker. The placementHistory_
    // LIFO at MainWindow level is what makes this work — neither
    // model alone knows the global order.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));
    const auto durationMs = window->player().duration().count();

    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);

    // Place B at 0 ms. Then seek to 1/2 and place M. Then seek to
    // 1/4 and place B. Sequence: B, M, B.
    QTest::keyClick(window.get(), Qt::Key_B);
    posSlider->setValue(static_cast<int>(durationMs / 2));
    emit posSlider->sliderMoved(static_cast<int>(durationMs / 2));
    QTest::keyClick(window.get(), Qt::Key_M);
    posSlider->setValue(static_cast<int>(durationMs / 4));
    emit posSlider->sliderMoved(static_cast<int>(durationMs / 4));
    QTest::keyClick(window.get(), Qt::Key_B);

    REQUIRE(window->barlineModel().size() == 2);
    REQUIRE(window->markerModel().size()  == 1);

    // First Ctrl+Z peels the most recent — the second barline.
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(window->markerModel().size()  == 1);

    // Second Ctrl+Z peels the marker (placed before the barline we
    // just removed but after the first barline).
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(window->markerModel().size()  == 0);

    // Third Ctrl+Z peels the first barline.
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->barlineModel().empty());
    REQUIRE(window->markerModel().empty());

    // Fourth Ctrl+Z is a quiet no-op (history drained).
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->barlineModel().empty());
    REQUIRE(window->markerModel().empty());
}

TEST_CASE("MainWindow: window-level Del removes the selected marker",
          "[main-window][gui][integration][markers]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTest::keyClick(window.get(), Qt::Key_M);
    REQUIRE(window->markerModel().size() == 1);

    // Marker auto-selected on placement → Del removes it via the
    // window-level shortcut, regardless of focus.
    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->markerModel().empty());
}

TEST_CASE("MainWindow: dock click selects the marker in waveform + staff",
          "[main-window][gui][integration][markers]") {
    // MEMO: this is the inbound mirror direction (dock → widgets).
    // The other directions are pinned by other tests; this one
    // proves the dock is a first-class selection source.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    auto* dock     =
        window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);

    // Place two markers so we have something distinct to select.
    QTest::keyClick(window.get(), Qt::Key_M);
    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(1000);
    emit posSlider->sliderMoved(1000);
    QTest::keyClick(window.get(), Qt::Key_M);

    REQUIRE(window->markerModel().size() == 2);
    const auto firstId  = window->markerModel().markers()[0].id;
    const auto secondId = window->markerModel().markers()[1].id;

    // Programmatically select the first marker via the dock —
    // simulates a click in the tree.
    dock->setSelectedMarkerId(firstId);

    REQUIRE(waveform->selectedMarkerId() == firstId);
    REQUIRE(staff->selectedMarkerId()    == firstId);

    // And switching the dock selection mirrors instantly.
    dock->setSelectedMarkerId(secondId);
    REQUIRE(waveform->selectedMarkerId() == secondId);
    REQUIRE(staff->selectedMarkerId()    == secondId);
}

TEST_CASE("MainWindow: barline + marker selections are mutually exclusive",
          "[main-window][gui][integration][markers]") {
    // The property viewer's contract is "the selected artifact" —
    // not "the selected barline AND the selected marker". This test
    // pins that selecting one kind clears the other across all
    // sibling widgets.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);

    // One bar, one marker.
    QTest::keyClick(window.get(), Qt::Key_B);   // selects the bar
    REQUIRE(waveform->selectedBarline() == 0);
    REQUIRE_FALSE(waveform->selectedMarkerId().has_value());

    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(1000);
    emit posSlider->sliderMoved(1000);
    QTest::keyClick(window.get(), Qt::Key_M);   // selects the marker

    // Marker selection should now be set; barline selection cleared
    // (because the marker auto-select mutual-exclusion-clears the bar).
    REQUIRE(waveform->selectedMarkerId().has_value());
    REQUIRE_FALSE(waveform->selectedBarline().has_value());
    REQUIRE_FALSE(staff->selectedBarline().has_value());
}

TEST_CASE("MainWindow: double-clicking a dock marker seeks player + starts playback",
          "[main-window][gui][integration][markers]") {
    // MEMO: end-to-end integration of the markerActivated signal.
    // Verifies that the dock's double-click actually moves the
    // player and (on hosts where Player::play is functional) flips
    // the play button text to "Pause". The seek part is testable on
    // any host; the playback-state side is best-effort because
    // hosts without an audio output stay Stopped — we still verify
    // the UI label flip since onMarkerActivated forces it.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* dock     =
        window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn  = window->findChild<QPushButton*>("playButton");
    auto* stopBtn  = window->findChild<QPushButton*>("stopButton");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // Land at position 0 deterministically, then place a marker
    // at ~1000 ms via slider+M so we have a non-zero target.
    QTest::mouseClick(playBtn, Qt::LeftButton);
    QTest::mouseClick(stopBtn, Qt::LeftButton);
    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(1000);
    emit posSlider->sliderMoved(1000);
    QTest::keyClick(window.get(), Qt::Key_M);
    REQUIRE(window->markerModel().size() == 1);
    const auto markerId = window->markerModel().markers()[0].id;

    // Move the player back to 0 so we can verify the marker
    // double-click actually moved it back to ~1000.
    posSlider->setValue(0);
    emit posSlider->sliderMoved(0);
    REQUIRE(window->player().position().count() == 0);

    // Simulate the dock's double-click. The signal carries the
    // marker ID; MainWindow's onMarkerActivated does the seek +
    // play.
    emit dock->markerActivated(markerId);

    // Player should now be at the marker's position. Player::seek
    // is synchronous (anchor-based) so the new anchor lands
    // immediately. On a host with a real audio device, play() then
    // starts the stream and a few ms can elapse before we read
    // position() — hence the small tolerance window. On a no-audio
    // host play() is a no-op and position is exactly the seek
    // target.
    const auto playerPosMs = window->player().position().count();
    REQUIRE(playerPosMs >= 1000);
    REQUIRE(playerPosMs <= 1050);

    // Whether playback actually runs depends on whether
    // Player::play() succeeds — that requires a real audio output.
    // What we can pin universally is the UI side: the button text
    // is forced to "Pause" by onMarkerActivated regardless of
    // audio availability, so the user gets consistent feedback.
    REQUIRE(playBtn->text() == "Pause");
}

TEST_CASE("MainWindow: opening a new file clears the marker model",
          "[main-window][gui][integration][markers]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    const QString fixturePath =
        QString::fromStdString(fixtureWav().string());
    REQUIRE(window->loadFile(fixturePath));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    auto* playButton = window->findChild<QPushButton*>("playButton");
    auto* stopButton = window->findChild<QPushButton*>("stopButton");
    QTest::mouseClick(playButton, Qt::LeftButton);
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTest::keyClick(window.get(), Qt::Key_M);
    QTest::keyClick(window.get(), Qt::Key_M);
    REQUIRE(window->markerModel().size() == 2);

    // Reload: the previous file's annotations are no longer
    // meaningful, so the marker model + the placement-history LIFO
    // both reset.
    REQUIRE(window->loadFile(fixturePath));
    REQUIRE(window->markerModel().empty());

    // After reload, Ctrl+Z is a no-op (history was cleared).
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().empty());
    REQUIRE(window->barlineModel().empty());
}
