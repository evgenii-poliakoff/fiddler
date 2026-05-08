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
#include "score/LoopModel.h"
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
#include <QTreeWidget>
#include <QTreeWidgetItem>

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

// ---------------------------------------------------------------------------
// Loop creation (L gesture) + Ctrl+Z + Del integration
//
// MEMO[refactor]: these are end-to-end tests against MainWindow —
// they exercise the L shortcut, the secondary anchor mirror, the
// combined-Ctrl+Z LIFO with the new Loop kind, and the
// onDeleteSelectedArtifact dispatch on the Loop branch. Per
// feedback_logs_drive_tests.md, each test pins a user gesture
// reproducible from the logs alone.
// ---------------------------------------------------------------------------

namespace {

// Convenience: tap a marker at the player's current position via M.
// The widgets pick it up and auto-select on the waveform; tests
// chain a seek + tap to drop a marker at a known ms.
void seekAndTapMarker(MainWindow& window, std::int64_t ms) {
    auto* slider = window.findChild<QSlider*>("positionSlider");
    REQUIRE(slider);
    slider->setValue(static_cast<int>(ms));
    // sliderMoved is what the widget connects to onSeek; setValue
    // alone wouldn't trigger the player. Emit it directly.
    emit slider->sliderMoved(static_cast<int>(ms));
    QTest::keyClick(&window, Qt::Key_M);
}

} // namespace

TEST_CASE("MainWindow: L creates a loop spanning the two anchored markers",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 500);
    seekAndTapMarker(*window, 1500);
    REQUIRE(window->markerModel().size() == 2);

    // After tapping the second marker, it's the auto-selected
    // primary. We need to also have a secondary anchor — set it
    // directly via the widget API (the user gesture would be
    // Ctrl+click on the first marker, but driving that via QTest is
    // covered in the widget-level tests; here we focus on what
    // MainWindow does once both anchors are present).
    waveform->setSecondaryAnchorMs(500);
    REQUIRE(waveform->primaryAnchorMs()   == 1500);
    REQUIRE(waveform->secondaryAnchorMs() == 500);

    QTest::keyClick(window.get(), Qt::Key_L);

    REQUIRE(window->loopModel().size() == 1);
    const auto& loop = window->loopModel().loops()[0];
    REQUIRE(loop.startMs == 500);
    REQUIRE(loop.endMs   == 1500);
    REQUIRE(loop.pauseMs == 500);   // model default
    // Loop is auto-selected on creation so the dock's property
    // page jumps straight to it.
    REQUIRE(waveform->selectedLoopId().has_value());
    // Secondary anchor was consumed.
    REQUIRE_FALSE(waveform->secondaryAnchorMs().has_value());
}

TEST_CASE("MainWindow: L without two anchors is a no-op",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // Just one marker; no secondary anchor.
    seekAndTapMarker(*window, 1000);
    QTest::keyClick(window.get(), Qt::Key_L);
    REQUIRE(window->loopModel().empty());
}

TEST_CASE("MainWindow: L on identical primary + secondary refuses degenerate range",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1000);
    waveform->setSecondaryAnchorMs(1000);   // same as primary

    QTest::keyClick(window.get(), Qt::Key_L);
    REQUIRE(window->loopModel().empty());
}

TEST_CASE("MainWindow: Ctrl+Z peels the most-recently-created loop",
          "[main-window][gui][integration][loops]") {
    // MEMO: load-bearing — the combined LIFO must include Loop and
    // dispatch to LoopModel::undoLastAdd on Ctrl+Z. Without the new
    // PlacementKind::Loop branch this would fall through to one of
    // the marker / barline models and leave the loop in place.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 500);
    seekAndTapMarker(*window, 1500);
    waveform->setSecondaryAnchorMs(500);
    QTest::keyClick(window.get(), Qt::Key_L);
    REQUIRE(window->loopModel().size() == 1);

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->loopModel().empty());
    // Markers are untouched — Ctrl+Z peeled the loop only.
    REQUIRE(window->markerModel().size() == 2);
}

TEST_CASE("MainWindow: Del with a selected loop removes it",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 500);
    seekAndTapMarker(*window, 1500);
    waveform->setSecondaryAnchorMs(500);
    QTest::keyClick(window.get(), Qt::Key_L);
    REQUIRE(waveform->selectedLoopId().has_value());

    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->loopModel().empty());
}

TEST_CASE("MainWindow: secondary anchor mirrors waveform → staff",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    waveform->setSecondaryAnchorMs(1234);
    REQUIRE(staff->secondaryAnchorMs() == 1234);

    waveform->setSecondaryAnchorMs(std::nullopt);
    REQUIRE_FALSE(staff->secondaryAnchorMs().has_value());
}

TEST_CASE("MainWindow: loop selection mirrors across waveform / staff / dock",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    auto* dock     = window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 500);
    seekAndTapMarker(*window, 1500);
    waveform->setSecondaryAnchorMs(500);
    QTest::keyClick(window.get(), Qt::Key_L);

    const auto loopId = *waveform->selectedLoopId();
    REQUIRE(staff->selectedLoopId()  == loopId);
    REQUIRE(*dock->selectedLoopId()  == loopId);

    // Clear via the dock — the score widgets should follow.
    dock->setSelectedLoopId(std::nullopt);
    REQUIRE_FALSE(waveform->selectedLoopId().has_value());
    REQUIRE_FALSE(staff->selectedLoopId().has_value());
}

TEST_CASE("MainWindow: opening a new file clears the loop model",
          "[main-window][gui][integration][loops]") {
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

    seekAndTapMarker(*window, 500);
    seekAndTapMarker(*window, 1500);
    waveform->setSecondaryAnchorMs(500);
    QTest::keyClick(window.get(), Qt::Key_L);
    REQUIRE(window->loopModel().size() == 1);

    REQUIRE(window->loadFile(fixturePath));
    REQUIRE(window->loopModel().empty());
    REQUIRE(window->markerModel().empty());
}

// ---------------------------------------------------------------------------
// Loop activation (commit 5)
//
// MEMO[refactor]: these tests pin the arming state machine and the
// dock<->MainWindow handshake. The wrap-around itself depends on
// audio playback advancing the position, which isn't reliable in
// headless CI — those paths are exercised end-to-end in manual
// smoke tests, while the unit tests here verify the deterministic
// arm / disarm transitions.
// ---------------------------------------------------------------------------

namespace {

// Build a fresh window with one loop spanning [start, end] ms.
// Returns the (window, loopId) pair so the test can drive arming.
struct LoadedWithLoop {
    std::unique_ptr<MainWindow> window;
    std::int64_t                loopId = 0;
};

LoadedWithLoop makeWindowWithLoop(std::int64_t start = 500,
                                  std::int64_t end   = 1500) {
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, start);
    seekAndTapMarker(*window, end);
    waveform->setSecondaryAnchorMs(start);
    QTest::keyClick(window.get(), Qt::Key_L);

    REQUIRE(window->loopModel().size() == 1);
    LoadedWithLoop out;
    out.loopId = window->loopModel().loops()[0].id;
    out.window = std::move(window);
    return out;
}

} // namespace

TEST_CASE("MainWindow: loopActivated arms the loop, seeks to start, flips Play→Pause",
          "[main-window][gui][integration][loops]") {
    // MEMO: load-bearing — double-click in the dock should be a
    // single user gesture that lands the user at the loop's
    // starting bar with playback running. Mirrors the marker-
    // activated jump-and-play idiom.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");

    // Move the player away from the loop's startMs so we can verify
    // the seek lands.
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(0);
    emit posSlider->sliderMoved(0);

    emit dock->loopActivated(loaded.loopId);

    REQUIRE(*dock->armedLoopId() == loaded.loopId);
    REQUIRE(playBtn->text() == "Pause");
    // Player::seek is synchronous; on no-audio hosts position is
    // exactly the seek target. On an audio host a few ms can have
    // elapsed by the time we read it.
    const auto pos = loaded.window->player().position().count();
    REQUIRE(pos >= 500);
    REQUIRE(pos <= 550);
}

TEST_CASE("MainWindow: Arm checkbox arms without seeking or auto-playing",
          "[main-window][gui][integration][loops]") {
    // MEMO: the checkbox path is intentionally less aggressive than
    // double-click — the user might be mid-listen and just want
    // wrap-around enabled at endMs. A seek would be jarring, and
    // auto-play would change transport state without an explicit
    // intent.
    qtApp();
    auto loaded = makeWindowWithLoop(500, 1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");

    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(2000);
    emit posSlider->sliderMoved(2000);
    const auto posBefore = loaded.window->player().position().count();

    emit dock->loopArmToggleRequested(loaded.loopId, true);

    REQUIRE(*dock->armedLoopId() == loaded.loopId);
    // Player did NOT move.
    REQUIRE(loaded.window->player().position().count() == posBefore);
}

TEST_CASE("MainWindow: unchecking the Arm checkbox disarms",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto loaded = makeWindowWithLoop(500, 1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");

    emit dock->loopArmToggleRequested(loaded.loopId, true);
    REQUIRE(dock->armedLoopId().has_value());

    emit dock->loopArmToggleRequested(loaded.loopId, false);
    REQUIRE_FALSE(dock->armedLoopId().has_value());
}

TEST_CASE("MainWindow: Stop disarms the armed loop",
          "[main-window][gui][integration][loops]") {
    // MEMO: per design — Stop is the user's "exit loop mode"
    // gesture. The next Play press resumes normal (non-wrapping)
    // playback.
    qtApp();
    auto loaded = makeWindowWithLoop(500, 1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* stopBtn = loaded.window->findChild<QPushButton*>("stopButton");

    emit dock->loopActivated(loaded.loopId);
    REQUIRE(dock->armedLoopId().has_value());

    QTest::mouseClick(stopBtn, Qt::LeftButton);
    REQUIRE_FALSE(dock->armedLoopId().has_value());
}

TEST_CASE("MainWindow: deleting the armed loop disarms transport",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto loaded = makeWindowWithLoop(500, 1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");

    emit dock->loopActivated(loaded.loopId);
    REQUIRE(dock->armedLoopId().has_value());

    emit dock->loopDeleteRequested(loaded.loopId);
    REQUIRE(loaded.window->loopModel().empty());
    REQUIRE_FALSE(dock->armedLoopId().has_value());
}

TEST_CASE("MainWindow: loadFile clears any armed-loop state",
          "[main-window][gui][integration][loops]") {
    qtApp();
    auto loaded = makeWindowWithLoop(500, 1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");

    emit dock->loopActivated(loaded.loopId);
    REQUIRE(dock->armedLoopId().has_value());

    REQUIRE(loaded.window->loadFile(
        QString::fromStdString(fixtureWav().string())));
    REQUIRE_FALSE(dock->armedLoopId().has_value());
}

TEST_CASE("MainWindow: arming an unknown loop ID is a quiet no-op",
          "[main-window][gui][integration][loops]") {
    // MEMO: defensive — the dock should never emit a stale ID, but
    // we double-check at the MainWindow boundary so a future bug
    // can't land us in an "armed but no loop" inconsistent state.
    qtApp();
    auto loaded = makeWindowWithLoop(500, 1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");

    emit dock->loopArmToggleRequested(99999, true);
    REQUIRE_FALSE(dock->armedLoopId().has_value());
}

// ---------------------------------------------------------------------------
// Press-Play-with-armed-loop seek rule
//
// MEMO[refactor]: load-bearing UX — pressing Play with a loop armed
// should land the user at the loop's startMs unless they're already
// inside the loop (mid-listen continuity). Three branches:
//   * pos < startMs   → seek
//   * pos in loop     → no seek (continuity)
//   * pos >= endMs    → seek
// Without the seek branches, the user either waits 30+ seconds for
// the tune to reach endMs before wrap engages, or sits at a stale
// post-loop position.
// ---------------------------------------------------------------------------

// MEMO: the test fixture WAV is 2 seconds long, so all ms values
// in this block stay strictly inside [0, 2000].

TEST_CASE("MainWindow: Play with armed loop and pos before startMs seeks to startMs",
          "[main-window][gui][integration][loops][play-armed]") {
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/1000, /*end=*/1800);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");

    // Arm via checkbox path (no auto-seek), then move position to
    // BEFORE startMs to set up the "user wants to drill the loop"
    // scenario.
    emit dock->loopArmToggleRequested(loaded.loopId, true);
    posSlider->setValue(300);
    emit posSlider->sliderMoved(300);
    REQUIRE(loaded.window->player().position().count() == 300);

    QTest::mouseClick(playBtn, Qt::LeftButton);

    // Player now sits at startMs (or up to ~50ms past on hosts
    // where audio is actually advancing).
    const auto pos = loaded.window->player().position().count();
    REQUIRE(pos >= 1000);
    REQUIRE(pos <= 1050);
}

TEST_CASE("MainWindow: Play with armed loop and pos inside the loop preserves position",
          "[main-window][gui][integration][loops][play-armed]") {
    // MEMO: the "armed mid-listen and resumed" case. The user was
    // already inside the loop region; pressing Play after a pause
    // should resume from where they paused, not jump back to start.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1800);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");

    emit dock->loopArmToggleRequested(loaded.loopId, true);
    posSlider->setValue(1000);
    emit posSlider->sliderMoved(1000);

    QTest::mouseClick(playBtn, Qt::LeftButton);

    const auto pos = loaded.window->player().position().count();
    REQUIRE(pos >= 1000);
    REQUIRE(pos <= 1050);   // no seek to startMs
}

TEST_CASE("MainWindow: Play with armed loop and pos past endMs seeks to startMs",
          "[main-window][gui][integration][loops][play-armed]") {
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/400, /*end=*/1000);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");

    emit dock->loopArmToggleRequested(loaded.loopId, true);
    posSlider->setValue(1500);
    emit posSlider->sliderMoved(1500);

    QTest::mouseClick(playBtn, Qt::LeftButton);

    const auto pos = loaded.window->player().position().count();
    REQUIRE(pos >= 400);
    REQUIRE(pos <= 450);
}

TEST_CASE("MainWindow: dock Ctrl+click on a marker creates a loop with L",
          "[main-window][gui][integration][loops][dock-ctrl-click]") {
    // MEMO: end-to-end — click marker A in the dock, Ctrl+click
    // marker B in the dock, press L. The window-scoped L shortcut
    // fires regardless of which child has focus, so the whole
    // gesture works without ever touching the score widgets. This
    // pins the user's request: "same selection mechanics in the
    // list of markers in the property editor".
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* dock     =
        window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    // Two markers at known positions.
    seekAndTapMarker(*window, 500);
    seekAndTapMarker(*window, 1500);
    REQUIRE(window->markerModel().size() == 2);

    auto* tree = dock->findChild<QTreeWidget*>("projectViewerTree");
    REQUIRE(tree);
    // After tap-place, the auto-selection put marker 2 (id from
    // idAt(1) — sorted-order index 1) on the primary; reset to
    // marker 1 via plain click in the dock so the test is explicit
    // about both gestures.
    const auto firstId  = window->markerModel().markers()[0].id;
    const auto secondId = window->markerModel().markers()[1].id;

    auto findRow = [&](std::int64_t markerId) -> QTreeWidgetItem* {
        constexpr int kRole = Qt::UserRole + 1;   // mirrors dock impl
        auto* category = tree->topLevelItem(0);   // Markers
        for (int i = 0; i < category->childCount(); ++i) {
            auto* child = category->child(i);
            if (child->data(0, kRole).toLongLong() == markerId) {
                return child;
            }
        }
        return nullptr;
    };

    // Plain click marker 1 in the dock.
    auto* firstRow = findRow(firstId);
    REQUIRE(firstRow);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualItemRect(firstRow).center());
    REQUIRE(*waveform->selectedMarkerId() == firstId);

    // Ctrl+click marker 2 in the dock.
    auto* secondRow = findRow(secondId);
    REQUIRE(secondRow);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::ControlModifier,
                      tree->visualItemRect(secondRow).center());
    REQUIRE(*waveform->selectedMarkerId() == secondId);
    // The dashed tick should be sitting at marker 1's ms now.
    REQUIRE(waveform->secondaryAnchorMs() == 500);

    // Press L — window-scoped shortcut, fires regardless of focus.
    QTest::keyClick(window.get(), Qt::Key_L);

    REQUIRE(window->loopModel().size() == 1);
    const auto& loop = window->loopModel().loops()[0];
    REQUIRE(loop.startMs == 500);
    REQUIRE(loop.endMs   == 1500);
}

TEST_CASE("MainWindow: dock secondary-anchor mirrors to staff",
          "[main-window][gui][integration][loops][dock-ctrl-click]") {
    // MEMO: when the dock fires loopAnchorAddRequested, MainWindow
    // pushes the captured ms to BOTH score widgets directly so the
    // dashed tick is visible in both views.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    auto* dock     =
        window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 700);

    // Simulate the dock's gesture signal directly — the unit tests
    // already cover that the click path emits it.
    emit dock->loopAnchorAddRequested();
    REQUIRE(waveform->secondaryAnchorMs() == 700);
    REQUIRE(staff->secondaryAnchorMs()    == 700);

    emit dock->loopAnchorClearRequested();
    REQUIRE_FALSE(waveform->secondaryAnchorMs().has_value());
    REQUIRE_FALSE(staff->secondaryAnchorMs().has_value());
}

TEST_CASE("MainWindow: Play with no armed loop does not jump",
          "[main-window][gui][integration][loops][play-armed]") {
    // MEMO: regression — the seek branch is gated on armedLoopId_.
    // Disarmed playback should always resume from the current
    // position, never silently jump.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));
    auto* playBtn   = window->findChild<QPushButton*>("playButton");
    auto* posSlider = window->findChild<QSlider*>("positionSlider");

    posSlider->setValue(1500);
    emit posSlider->sliderMoved(1500);
    QTest::mouseClick(playBtn, Qt::LeftButton);

    const auto pos = window->player().position().count();
    REQUIRE(pos >= 1500);
    REQUIRE(pos <= 1550);
}
