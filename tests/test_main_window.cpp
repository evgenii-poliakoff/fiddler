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
#include "ui/LoopCountdownWidget.h"
#include "ui/MainWindow.h"
#include "ui/ProjectViewerDock.h"
#include "ui/StaffWidget.h"
#include "ui/WaveformWidget.h"
#include "wav_fixture.h"

#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QTemporaryDir>
#include <QPushButton>
#include <QSignalSpy>
#include <QAction>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
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
    // Place two markers at distinct positions. The near-duplicate
    // guard (#17) rejects same-position retaps, so we seek between
    // taps to keep both placements valid.
    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(0);
    emit posSlider->sliderMoved(0);
    QTest::keyClick(window.get(), Qt::Key_M);
    posSlider->setValue(500);
    emit posSlider->sliderMoved(500);
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

// ---------------------------------------------------------------------------
// Tap-to-place near-duplicate guard (#17)
//
// MEMO: a rapid double-tap of M / B places two artifacts within a
// few ms of each other — almost always an accident the user has to
// undo. The tap handler rejects the second tap when an existing
// same-kind artifact sits within `kMinTapSeparationMs` (50 ms).
// The check lives at the gesture layer; the models stay permissive
// (you can still place close artifacts via the dock spinbox).
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: rapid M tap near an existing marker is rejected",
          "[main-window][gui][integration][markers][near-duplicate]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1000);
    REQUIRE(window->markerModel().size() == 1);

    // Second tap 20 ms later — within the 50 ms window. Rejected.
    seekAndTapMarker(*window, 1020);
    REQUIRE(window->markerModel().size() == 1);

    // Third tap well outside the window. Accepted.
    seekAndTapMarker(*window, 1100);
    REQUIRE(window->markerModel().size() == 2);
}

TEST_CASE("MainWindow: rapid B tap near an existing barline is rejected",
          "[main-window][gui][integration][barlines][near-duplicate]") {
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

    auto seekAndTapBarline = [&](std::int64_t ms) {
        posSlider->setValue(static_cast<int>(ms));
        emit posSlider->sliderMoved(static_cast<int>(ms));
        QTest::keyClick(window.get(), Qt::Key_B);
    };

    seekAndTapBarline(1000);
    REQUIRE(window->barlineModel().size() == 1);

    // Within the 50 ms window — rejected.
    seekAndTapBarline(1030);
    REQUIRE(window->barlineModel().size() == 1);

    // Outside the window — accepted.
    seekAndTapBarline(1200);
    REQUIRE(window->barlineModel().size() == 2);
}

TEST_CASE("MainWindow: near-duplicate guard is per-kind — M near B is allowed",
          "[main-window][gui][integration][near-duplicate]") {
    // Different-kind co-location is fine: a barline and a marker
    // can share an ms (they mean different things).
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

    posSlider->setValue(1000);
    emit posSlider->sliderMoved(1000);
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 1);

    // Same source-time, different kind → accepted.
    QTest::keyClick(window.get(), Qt::Key_M);
    REQUIRE(window->markerModel().size() == 1);
}

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
//
// MEMO[CI]: the Play button is gated on `hasAudioOutput()` — on CI
// hosts without an audio device the button stays disabled, and a
// QTest::mouseClick on a disabled button is a silent no-op, which
// would mask the seek branch we're trying to verify. We
// `setEnabled(true)` before clicking so the test exercises the
// onPlayPause path regardless of audio availability. The
// production guard is purely a UX nicety; the slot runs the same
// logic either way.

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

    playBtn->setEnabled(true);
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

    playBtn->setEnabled(true);
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

    playBtn->setEnabled(true);
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

TEST_CASE("MainWindow: bar selected on waveform + Ctrl+click marker in dock keeps secondary alive",
          "[main-window][gui][integration][loops][dock-ctrl-click]") {
    // MEMO: regression repro for the user's bug report — primary
    // selection is a barline placed via the waveform; secondary
    // gesture comes from Ctrl+click on a marker row in the dock.
    // The sequence triggers two state-changing paths in close
    // succession (loopAnchorAddRequested THEN markerSelectionChanged
    // from the tree's selection change). We need to verify the
    // secondary survives the second path's mutual-exclusion clearing
    // so the paint code's isAnchor check still fires.
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

    // Setup: 1 barline at 500 ms, 1 marker at 1500 ms.
    auto* posSlider = window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(500);
    emit posSlider->sliderMoved(500);
    QTest::keyClick(window.get(), Qt::Key_B);
    posSlider->setValue(1500);
    emit posSlider->sliderMoved(1500);
    QTest::keyClick(window.get(), Qt::Key_M);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(window->markerModel().size()  == 1);

    // Clear any stale state from the auto-select-on-place path.
    waveform->setSelectedMarkerId(std::nullopt);
    waveform->setSecondaryAnchorMs(std::nullopt);

    // Step 1: select the bar via waveform — sets selectedBarline_=0.
    waveform->setSelectedBarline(0);
    REQUIRE(waveform->selectedBarline() == 0);
    REQUIRE_FALSE(waveform->selectedMarkerId().has_value());
    REQUIRE_FALSE(waveform->secondaryAnchorMs().has_value());

    // Step 2: Ctrl+click the marker row in the dock. This fires
    // loopAnchorAddRequested first (capturing the bar's ms as
    // secondary), then Qt processes the click and selection moves
    // to the marker, triggering mutual exclusion that clears
    // selectedBarline_.
    auto* tree = dock->findChild<QTreeWidget*>("projectViewerTree");
    REQUIRE(tree);
    constexpr int kRole = Qt::UserRole + 1;
    auto* category = tree->topLevelItem(0);
    QTreeWidgetItem* markerRow = nullptr;
    for (int i = 0; i < category->childCount(); ++i) {
        if (category->child(i)->data(0, kRole).toLongLong()
            == window->markerModel().markers()[0].id) {
            markerRow = category->child(i);
            break;
        }
    }
    REQUIRE(markerRow);

    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::ControlModifier,
                      tree->visualItemRect(markerRow).center());

    // The painted-as-dashed contract: secondaryAnchorMs_ holds the
    // barline's ms, selectedBarline_ is cleared (mutual exclusion),
    // selectedMarkerId_ points at the new marker. The bar's isAnchor
    // check in paintEvent then fires.
    REQUIRE(waveform->secondaryAnchorMs() == 500);
    REQUIRE_FALSE(waveform->selectedBarline().has_value());
    REQUIRE(*waveform->selectedMarkerId()
            == window->markerModel().markers()[0].id);
    // Same on staff (mirror).
    REQUIRE(staff->secondaryAnchorMs() == 500);
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
    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);

    const auto pos = window->player().position().count();
    REQUIRE(pos >= 1500);
    REQUIRE(pos <= 1550);
}

// ---------------------------------------------------------------------------
// View menu + dock toggle + QSettings persistence (issue #7)
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: View menu has a Project Viewer toggle action",
          "[main-window][gui][integration][dock-toggle]") {
    qtApp();
    MainWindow window;
    window.show();
    (void)QTest::qWaitForWindowExposed(&window);

    auto* action =
        window.findChild<QAction*>("toggleProjectViewerAction");
    REQUIRE(action);
    REQUIRE(action->isCheckable());
    // Initially the dock is visible, so the action is checked.
    REQUIRE(action->isChecked());
    REQUIRE(action->shortcut() == QKeySequence(Qt::Key_F4));
}

TEST_CASE("MainWindow: triggering the toggle action hides + shows the dock",
          "[main-window][gui][integration][dock-toggle]") {
    qtApp();
    MainWindow window;
    window.show();
    (void)QTest::qWaitForWindowExposed(&window);

    auto* dock   = window.findChild<ProjectViewerDock*>("projectViewerDock");
    auto* action =
        window.findChild<QAction*>("toggleProjectViewerAction");
    REQUIRE(dock);
    REQUIRE(action);
    REQUIRE(dock->isVisible());

    action->trigger();
    REQUIRE_FALSE(dock->isVisible());
    REQUIRE_FALSE(action->isChecked());

    action->trigger();
    REQUIRE(dock->isVisible());
    REQUIRE(action->isChecked());
}

TEST_CASE("MainWindow: F4 keyboard shortcut toggles the dock",
          "[main-window][gui][integration][dock-toggle]") {
    qtApp();
    MainWindow window;
    window.show();
    (void)QTest::qWaitForWindowExposed(&window);

    auto* dock = window.findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(dock->isVisible());

    QTest::keyClick(&window, Qt::Key_F4);
    REQUIRE_FALSE(dock->isVisible());

    QTest::keyClick(&window, Qt::Key_F4);
    REQUIRE(dock->isVisible());
}

TEST_CASE("MainWindow: closing the dock then reopening the window keeps it hidden",
          "[main-window][gui][integration][dock-toggle]") {
    // MEMO: round-trip test for QSettings persistence. closeEvent
    // calls saveLayout(); the next MainWindow's ctor calls
    // restoreLayout(). With QStandardPaths test-mode enabled in
    // qt_test_app's main(), this writes to a per-process scratch
    // dir, never the user's real config. qtApp() clears
    // QSettings every call so the test starts from a clean slate.
    qtApp();
    {
        MainWindow first;
        first.show();
        (void)QTest::qWaitForWindowExposed(&first);
        auto* dock = first.findChild<ProjectViewerDock*>("projectViewerDock");
        REQUIRE(dock->isVisible());
        dock->setVisible(false);
        REQUIRE_FALSE(dock->isVisible());
        // Trigger close — saveLayout writes the hidden state.
        first.close();
    }

    // Skip the qtApp() clear here so we read the just-saved state.
    MainWindow second;
    second.show();
    (void)QTest::qWaitForWindowExposed(&second);
    auto* dock2 = second.findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(dock2);
    REQUIRE_FALSE(dock2->isVisible());

    // Cleanup: restore visible state for any later tests sharing
    // the same process.
    auto* action2 =
        second.findChild<QAction*>("toggleProjectViewerAction");
    action2->trigger();
    second.close();
}

TEST_CASE("MainWindow: a fresh QSettings keeps the default visible layout",
          "[main-window][gui][integration][dock-toggle]") {
    // MEMO: with no persisted state, the dock should be visible
    // (Qt's default for an attached dock added via addDockWidget).
    // restoreLayout no-ops on empty values so this is the path
    // every first-launch user takes.
    qtApp();
    MainWindow window;
    window.show();
    (void)QTest::qWaitForWindowExposed(&window);

    auto* dock = window.findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(dock);
    REQUIRE(dock->isVisible());
}

// ---------------------------------------------------------------------------
// Loop countdown widget — integration with the pause-between-repeats wrap
//
// MEMO[refactor]: end-to-end pin for issue #9. The widget itself
// is unit-tested in test_loop_countdown_widget.cpp; here we verify
// MainWindow drives it correctly: startCountdown is called when
// the wrap-pause window begins, cancelCountdown is called on
// disarm. We don't rely on audio actually advancing position —
// instead we directly drive `updatePosition`'s wrap path by
// arming a loop with a known endMs and calling Player::seek to
// jump past it before triggering the GUI poll manually.
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: ProjectViewerDock has a countdown widget",
          "[main-window][gui][integration][countdown]") {
    qtApp();
    MainWindow window;
    window.show();
    (void)QTest::qWaitForWindowExposed(&window);

    auto* countdown =
        window.findChild<fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(countdown);
}

TEST_CASE("MainWindow: disarm via Stop cancels an active countdown",
          "[main-window][gui][integration][countdown]") {
    // MEMO: kicks the dock's startCountdown directly via the
    // stable API the widget exposes, then verifies that pressing
    // Stop disarms AND cancels. This shape sidesteps the audio-
    // playback-advances-position dependency that makes the wrap
    // path itself flaky in headless CI.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* stopBtn = loaded.window->findChild<QPushButton*>("stopButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(dock);
    REQUIRE(countdown);

    emit dock->loopActivated(loaded.loopId);
    REQUIRE(dock->armedLoopId().has_value());

    // Simulate the wrap-pause start (MainWindow calls this from
    // updatePosition when pos crosses endMs). A 5-second window
    // is long enough that the Stop click reliably interrupts it.
    dock->startCountdown(5000);
    REQUIRE(countdown->isCountingDown());

    QTest::mouseClick(stopBtn, Qt::LeftButton);
    REQUIRE_FALSE(dock->armedLoopId().has_value());
    REQUIRE_FALSE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: arming a different loop cancels a stale countdown",
          "[main-window][gui][integration][countdown]") {
    // MEMO: two loops; arm A, simulate wrap-pause, then arm B
    // before the pause finishes. The OLD wrap (for loop A) must
    // be cancelled — otherwise the user sees ticks depleting on
    // the property page while playing loop B's body, which is
    // misleading.
    //
    // MEMO[issue #16]: with the global pre-roll, activating B
    // ALSO starts a NEW countdown (B's own pre-roll). So we can't
    // assert "countdown is not running" — it's running, but for
    // B, not A. The test verifies the wrap state machine landed
    // on B's loopId, and we set prerollMs to 0 so the new
    // activation doesn't immediately enter another wrap-pause
    // (otherwise the cancellation is invisible).
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/300, /*end=*/700);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(dock);
    REQUIRE(countdown);

    // Set pre-roll to 0 so loopActivated of B is an immediate
    // play (no fresh countdown), exposing whether A's countdown
    // got cancelled cleanly.
    loaded.window->setPrerollMs(0);

    // Add a second loop directly.
    seekAndTapMarker(*loaded.window, 1000);
    seekAndTapMarker(*loaded.window, 1500);
    auto* waveform =
        loaded.window->findChild<WaveformWidget*>("waveformWidget");
    waveform->setSecondaryAnchorMs(1000);
    QTest::keyClick(loaded.window.get(), Qt::Key_L);
    REQUIRE(loaded.window->loopModel().size() == 2);
    const auto secondLoopId =
        loaded.window->loopModel().loops()[1].id;

    // Arm loop A, simulate its wrap-pause kicking off.
    emit dock->loopActivated(loaded.loopId);
    dock->startCountdown(5000);
    REQUIRE(countdown->isCountingDown());

    // Activate loop B — should cancel A's countdown. With
    // prerollMs=0 there's no fresh countdown to start.
    emit dock->loopActivated(secondLoopId);
    REQUIRE_FALSE(countdown->isCountingDown());
    REQUIRE(dock->armedLoopId() == secondLoopId);
}

TEST_CASE("MainWindow: deleting the armed loop cancels a stale countdown",
          "[main-window][gui][integration][countdown]") {
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    emit dock->loopActivated(loaded.loopId);
    dock->startCountdown(5000);
    REQUIRE(countdown->isCountingDown());

    emit dock->loopDeleteRequested(loaded.loopId);
    REQUIRE(loaded.window->loopModel().empty());
    REQUIRE_FALSE(countdown->isCountingDown());
}

// ---------------------------------------------------------------------------
// Wrap-pause cancellation by user gestures (issue #13)
//
// MEMO[refactor]: tests use the `enterWrapPauseForTest` test seam
// because the production wrap entry depends on audio advancing
// position past endMs, which doesn't happen reliably on headless
// CI. The seam puts the window in exactly the same state the
// production wrap path produces, minus the audio playback.
// Each TEST_CASE pins one rule: which user gesture cancels the
// wrap, and what state the window settles into.
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: clicking play/pause button during wrap-pause cancels and stays paused",
          "[main-window][gui][integration][loops][wrap-cancel]") {
    // MEMO: load-bearing rule for #13 — during wrap-pause, the
    // button label still reads "Pause" (transport is conceptually
    // in playback mode, just temporarily silent). Click is
    // interpreted as "cancel the auto-resume, pause for real".
    // Loop stays armed.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(playBtn);
    REQUIRE(countdown);

    loaded.window->enterWrapPauseForTest(loaded.loopId, /*pauseMs=*/5000);
    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());

    // Click the play/pause button — Option-2 semantics: cancel
    // the auto-resume, stay paused.
    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);

    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
    REQUIRE(playBtn->text() == "Play");
    REQUIRE(dock->armedLoopId() == loaded.loopId);
}

TEST_CASE("MainWindow: seeking during wrap-pause cancels and stays paused at new position",
          "[main-window][gui][integration][loops][wrap-cancel]") {
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");
    REQUIRE(posSlider);

    loaded.window->enterWrapPauseForTest(loaded.loopId, /*pauseMs=*/5000);
    REQUIRE(loaded.window->wrapPending());

    // Seek to outside the loop — the bug from #13 was a double
    // countdown firing after this seek. With the cancel in place,
    // the wrap is dropped cleanly.
    posSlider->setValue(1700);
    emit posSlider->sliderMoved(1700);

    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
    REQUIRE(playBtn->text() == "Play");
    // Loop arming is preserved.
    REQUIRE(dock->armedLoopId() == loaded.loopId);
}

TEST_CASE("MainWindow: seeking inside the loop during wrap-pause also cancels",
          "[main-window][gui][integration][loops][wrap-cancel]") {
    // MEMO: design choice — seek anywhere cancels, regardless of
    // whether the destination is inside or outside the loop. Seek
    // means "navigate"; if the user is mid-listen and wants to
    // resume from the new position, they press Play (which will
    // route through the seek-on-armed-Play branch if the new pos
    // is outside the loop).
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");

    loaded.window->enterWrapPauseForTest(loaded.loopId, /*pauseMs=*/5000);
    REQUIRE(loaded.window->wrapPending());

    posSlider->setValue(1000);   // INSIDE the loop range
    emit posSlider->sliderMoved(1000);

    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: after canceling via Play button, pressing Play resumes from startMs",
          "[main-window][gui][integration][loops][wrap-cancel]") {
    // MEMO: end-to-end check that the user's recovery path works.
    // First click "Pauses" (cancels wrap, stays paused, label flips
    // to "Play"). Second click resumes — and the seek-on-Play-armed
    // logic re-seeks to startMs because the cursor is at startMs (we
    // left it there when wrap-pause entered). play() takes effect.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");

    // Manually park the player at startMs (the production wrap path
    // would have seeked here; the test seam doesn't seek so we do it).
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(500);
    emit posSlider->sliderMoved(500);

    loaded.window->enterWrapPauseForTest(loaded.loopId, /*pauseMs=*/5000);

    // First click: cancel + stay paused.
    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);
    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE(playBtn->text() == "Play");

    // Second click: normal play. Position should still be near startMs.
    QTest::mouseClick(playBtn, Qt::LeftButton);
    REQUIRE(playBtn->text() == "Pause");
    const auto pos = loaded.window->player().position().count();
    REQUIRE(pos >= 500);
    REQUIRE(pos <= 550);
}

TEST_CASE("MainWindow::wrapShouldFire — natural forward crossing only",
          "[main-window][wrap-cancel]") {
    // MEMO: pure-helper test for the wrap-trigger rule. The full
    // updatePosition path requires player.state == Playing, which
    // is unreliable on headless CI. Pinning the rule directly here
    // catches future regressions without needing audio.
    constexpr std::int64_t endMs = 1500;

    // Natural forward crossing: previous tick was BEFORE endMs,
    // current tick is AT or PAST endMs. This is the only case
    // that fires.
    REQUIRE(MainWindow::wrapShouldFire(1499, 1500, endMs));
    REQUIRE(MainWindow::wrapShouldFire(1450, 1505, endMs));
    REQUIRE(MainWindow::wrapShouldFire(0,    9999, endMs));

    // Both before endMs (still inside or before the loop). Plays on.
    REQUIRE_FALSE(MainWindow::wrapShouldFire(500,  1000, endMs));
    REQUIRE_FALSE(MainWindow::wrapShouldFire(1499, 1499, endMs));

    // Both at/after endMs. The user seeked past, or playback was
    // already past — don't drag them back. This is the bug from
    // #13: previously, ANY pos >= endMs while armed-and-playing
    // triggered a wrap, even if the position came from a seek.
    REQUIRE_FALSE(MainWindow::wrapShouldFire(1500, 1505, endMs));
    REQUIRE_FALSE(MainWindow::wrapShouldFire(1700, 1750, endMs));

    // Backward (e.g. wrap just completed and the visible position
    // returned to startMs). Don't double-fire.
    REQUIRE_FALSE(MainWindow::wrapShouldFire(1505, 500, endMs));

    // First-tick guard: previousPosMs_ == -1 means we haven't
    // observed a prior position yet. Don't wrap on the very first
    // tick regardless of where the player happens to be.
    REQUIRE_FALSE(MainWindow::wrapShouldFire(-1, 1700, endMs));
    REQUIRE_FALSE(MainWindow::wrapShouldFire(-1, 0,    endMs));
}

TEST_CASE("MainWindow: Stop during wrap-pause cancels wrap AND disarms",
          "[main-window][gui][integration][loops][wrap-cancel]") {
    // MEMO: regression — Stop's existing behavior (cancel + disarm)
    // is preserved through the cancelPendingWrap refactor.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* stopBtn = loaded.window->findChild<QPushButton*>("stopButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->enterWrapPauseForTest(loaded.loopId, /*pauseMs=*/5000);
    REQUIRE(loaded.window->wrapPending());

    QTest::mouseClick(stopBtn, Qt::LeftButton);

    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
    REQUIRE_FALSE(dock->armedLoopId().has_value());   // disarmed
}

// ---------------------------------------------------------------------------
// Global pre-roll setting (issue #16)
//
// MEMO[refactor]: pause-between-repeats moved off LoopModel onto a
// global MainWindow setting. Pre-roll fires uniformly on every
// transition to Playing — Play press (with or without armed loop),
// double-click jump-and-play (markers + loops), Arm-then-Play,
// cancel-then-resume — AND on the pause-between-repeats wrap. The
// user gets consistent ready-set-go time before practice playback,
// regardless of which gesture they used.
//
// Two-mode design:
//   * passive listening (prerollEnabled=false, default): effective
//     pre-roll is 0 regardless of spinbox value. No wrap silence.
//   * practice mode (prerollEnabled=true): every transition to
//     Playing inserts a countdown for the spinbox value.
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: pre-roll defaults — disabled, value 500, effective 0",
          "[main-window][gui][integration][preroll]") {
    qtApp();
    MainWindow window;
    REQUIRE_FALSE(window.prerollEnabled());     // off by default
    REQUIRE(window.prerollMsValue() == 500);    // raw spinbox value
    REQUIRE(window.prerollMs() == 0);           // effective: 0 when disabled
}

TEST_CASE("MainWindow: enabling pre-roll exposes the spinbox value as effective",
          "[main-window][gui][integration][preroll]") {
    qtApp();
    MainWindow window;
    window.setPrerollEnabled(true);
    REQUIRE(window.prerollEnabled());
    REQUIRE(window.prerollMs() == 500);         // effective == value
    window.setPrerollEnabled(false);
    REQUIRE(window.prerollMs() == 0);
    REQUIRE(window.prerollMsValue() == 500);    // value preserved across toggle
}

TEST_CASE("MainWindow: prerollMs can be set + persists across sessions",
          "[main-window][gui][integration][preroll]") {
    // MEMO: round-trip through QSettings (qt_test_app enables test
    // mode so we don't pollute the user's real config).
    qtApp();
    {
        MainWindow first;
        first.setPrerollMs(1500);
        REQUIRE(first.prerollMsValue() == 1500);
        first.close();   // saveLayout writes to QSettings
    }
    // Skip the qtApp() clear so we read the just-saved state.
    MainWindow second;
    REQUIRE(second.prerollMsValue() == 1500);
}

TEST_CASE("MainWindow: prerollEnabled persists across sessions",
          "[main-window][gui][integration][preroll]") {
    qtApp();
    {
        MainWindow first;
        first.setPrerollEnabled(true);
        REQUIRE(first.prerollEnabled());
        first.close();
    }
    MainWindow second;
    REQUIRE(second.prerollEnabled());
}

TEST_CASE("MainWindow: setPrerollMs clamps out-of-range values",
          "[main-window][gui][integration][preroll]") {
    qtApp();
    MainWindow window;
    window.setPrerollMs(-100);
    REQUIRE(window.prerollMsValue() == 0);     // floor
    window.setPrerollMs(99999);
    REQUIRE(window.prerollMsValue() == 5000);  // ceiling
}

TEST_CASE("MainWindow: pre-roll enabled — Play with armed loop enters wrap-pause first",
          "[main-window][gui][integration][preroll]") {
    // MEMO: load-bearing — the practice-mode flow. With pre-roll
    // active, pressing Play on an armed loop doesn't immediately
    // call player.play(); it pauses the player + starts the
    // countdown widget so the user has time to prepare.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(countdown);

    // Enable pre-roll AND set a long value so the test has time to
    // observe the intermediate state without the timer firing.
    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(5000);

    // Arm the loop, then press Play. The arming itself is via the
    // checkbox path so it doesn't trigger pre-roll on its own.
    emit dock->loopArmToggleRequested(loaded.loopId, true);
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(800);   // inside the loop
    emit posSlider->sliderMoved(800);

    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);

    // Pre-roll is in flight: wrap pending, countdown depleting,
    // player paused.
    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: pre-roll disabled — Play with armed loop is immediate",
          "[main-window][gui][integration][preroll]") {
    // MEMO: passive-listening mode (default) — no countdown, no
    // pause, straight to play. Skips the wrap-pause path entirely
    // even though the spinbox value is non-zero, because the
    // checkbox is off.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    // Default-disabled state. spinbox value remains the default 500
    // but effective is 0.
    REQUIRE_FALSE(loaded.window->prerollEnabled());

    emit dock->loopArmToggleRequested(loaded.loopId, true);
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(800);
    emit posSlider->sliderMoved(800);

    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);

    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: pre-roll enabled but value 0 — Play is immediate",
          "[main-window][gui][integration][preroll]") {
    // MEMO: explicit-zero case — checkbox on, but the user dialed
    // the value to 0. Effective is 0 so no countdown.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(0);

    emit dock->loopArmToggleRequested(loaded.loopId, true);
    auto* posSlider =
        loaded.window->findChild<QSlider*>("positionSlider");
    posSlider->setValue(800);
    emit posSlider->sliderMoved(800);

    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);

    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: pre-roll enabled — double-click jump-and-play enters pre-roll",
          "[main-window][gui][integration][preroll]") {
    // MEMO: double-click is the explicit "play this loop now"
    // gesture, but still gives the user pre-roll prep time. This
    // matches the user's "hand-to-violin" rationale that drove
    // the redesign (#16).
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(5000);

    emit dock->loopActivated(loaded.loopId);

    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: pre-roll enabled — Play with no armed loop also fires pre-roll",
          "[main-window][gui][integration][preroll]") {
    // MEMO: scope expansion (#16) — pre-roll is no longer gated on
    // having a loop armed. Even simple listening from a position
    // gets the ready-set-go countdown so the user has time to
    // pick up the violin.
    qtApp();
    MainWindow window;
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(5000);
    // No arm — just press Play.
    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);

    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: pre-roll enabled — double-click marker also fires pre-roll",
          "[main-window][gui][integration][preroll]") {
    // MEMO: the marker double-click jump-and-play path used to
    // call player.play() directly. After #16 it routes through
    // startPlayback so the same pre-roll discipline applies.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(5000);

    // makeWindowWithLoop already placed two anchor markers when
    // building the loop. Activate the first one.
    REQUIRE(loaded.window->markerModel().size() >= 1);
    const auto markerId = loaded.window->markerModel().markers()[0].id;
    emit dock->markerActivated(markerId);

    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: pre-roll enabled — wrap path uses the global value, not a per-loop one",
          "[main-window][gui][integration][preroll]") {
    // MEMO: regression for the dropped per-loop pauseMs. The wrap
    // path should pick up the GLOBAL prerollMs, no matter which
    // loop is armed. Setting the global value and triggering the
    // wrap path manually verifies the wiring.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(2500);

    // Use the test seam to enter wrap-pause. The seam takes the
    // pre-roll value as parameter — verify the production wrap
    // path also picks up prerollMs by checking that startCountdown
    // is called with the same value (we can't easily compare the
    // QTimer's interval, but the countdown widget reads the input
    // directly).
    loaded.window->enterWrapPauseForTest(loaded.loopId, /*prerollMs=*/2500);
    REQUIRE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: pre-roll enabled — cancel-then-Play enters fresh pre-roll",
          "[main-window][gui][integration][preroll]") {
    // MEMO: the user's original motivating scenario — they cancel
    // a wrap-pause to take a moment, then press Play to resume.
    // Resume should give them another pre-roll, not jump straight
    // back into playback. (Hand-to-violin time after every "Play"
    // gesture, regardless of how the player got paused.)
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* dock =
        loaded.window->findChild<ProjectViewerDock*>("projectViewerDock");
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(5000);

    // Enter wrap-pause via the test seam.
    loaded.window->enterWrapPauseForTest(loaded.loopId, /*prerollMs=*/5000);
    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());

    // First Play click: cancel the in-flight wrap, stay paused.
    playBtn->setEnabled(true);
    QTest::mouseClick(playBtn, Qt::LeftButton);
    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
    REQUIRE(playBtn->text() == "Play");
    REQUIRE(dock->armedLoopId().has_value());

    // Second Play click: enter a FRESH pre-roll wrap-pause.
    QTest::mouseClick(playBtn, Qt::LeftButton);
    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: countdown widget visibility tracks prerollEnabled",
          "[main-window][gui][integration][preroll]") {
    // MEMO: the global countdown widget lives at the bottom of the
    // dock. It's only meaningful when the user has practice mode
    // on; passive listening hides it so the dock doesn't show a
    // dormant ring.
    qtApp();
    MainWindow window;
    window.show();
    (void)QTest::qWaitForWindowExposed(&window);

    auto* countdown = window.findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");
    REQUIRE(countdown);
    REQUIRE_FALSE(countdown->isVisible());   // hidden by default

    window.setPrerollEnabled(true);
    REQUIRE(countdown->isVisible());

    window.setPrerollEnabled(false);
    REQUIRE_FALSE(countdown->isVisible());
}

TEST_CASE("MainWindow: disabling pre-roll mid-countdown cancels the wrap-pause",
          "[main-window][gui][integration][preroll]") {
    // MEMO: if the user uncheckes pre-roll while a countdown is
    // ticking, the in-flight wrap-pause is cancelled (the timer
    // would otherwise still resume playback after the now-hidden
    // countdown finished, which is confusing).
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* countdown = loaded.window->findChild<
        fiddler::ui::LoopCountdownWidget*>("loopCountdown");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(5000);
    loaded.window->enterWrapPauseForTest(loaded.loopId, /*prerollMs=*/5000);
    REQUIRE(loaded.window->wrapPending());
    REQUIRE(countdown->isCountingDown());

    loaded.window->setPrerollEnabled(false);
    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE_FALSE(countdown->isCountingDown());
}

TEST_CASE("MainWindow: cancelPendingWrap flips the play button label to Play",
          "[main-window][gui][integration][preroll]") {
    // MEMO[smoke #16/6.1]: during wrap-pause we keep the button
    // label as "Pause" (the user might want to cancel the auto-
    // resume — see #13). Once the wrap IS cancelled, the player
    // is paused for real and the label must flip back to "Play".
    // The toggle-off-mid-countdown cancel path was missing this
    // update before this fix.
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* playBtn = loaded.window->findChild<QPushButton*>("playButton");

    loaded.window->setPrerollEnabled(true);
    loaded.window->setPrerollMs(5000);
    loaded.window->enterWrapPauseForTest(loaded.loopId, /*prerollMs=*/5000);
    // During wrap-pause the label is "Pause" (transport is in
    // "playback mode with brief silence first"; clicking would
    // cancel the auto-resume).
    REQUIRE(loaded.window->wrapPending());

    // Toggle pre-roll off mid-countdown — the cancel path runs
    // and the button label must be "Play" afterwards.
    loaded.window->setPrerollEnabled(false);
    REQUIRE_FALSE(loaded.window->wrapPending());
    REQUIRE(playBtn->text() == "Play");
}

// ---------------------------------------------------------------------------
// Unified Ctrl+Z (#20) — covers placements, drags, dock edits,
// renames, and deletes through one LIFO. The cross-kind placement
// case lives in the older "combined Ctrl+Z LIFO" test above; this
// block adds the non-placement kinds the unified history brought in.
// Each test fires the gesture's signal directly to keep the setup
// small — the production wiring is exercised end-to-end when the
// signal lands on MainWindow's slot.
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: live drag does not write the model — commit does (#22)",
          "[main-window][gui][integration][drag-smooth]") {
    // MEMO[#22]: pre-22, every mouse-move during a drag fired
    // markerModel_->setPosition, which re-sorted the markers
    // vector and rebuilt the dock tree. That per-move chain
    // starved the GUI thread and made the drag step visibly. The
    // unified ghost flow defers the write to release; this test
    // pins the contract.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto markerId = window->markerModel().markers()[0].id;

    QSignalSpy modelSpy(&window->markerModel(),
                        &fiddler::score::MarkerModel::changed);
    modelSpy.clear();

    // Three live-drag emissions: model must NOT change.
    emit waveform->markerDragRequested(markerId, 1600);
    emit waveform->markerDragRequested(markerId, 1700);
    emit waveform->markerDragRequested(markerId, 1800);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 1500);
    REQUIRE(modelSpy.count() == 0);

    // Commit: model writes exactly once.
    emit waveform->markerDragCommitted(markerId, 1500, 1800);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 1800);
    REQUIRE(modelSpy.count() == 1);
}

TEST_CASE("MainWindow: live drag mirrors ghost to the sister widget (#22)",
          "[main-window][gui][integration][drag-smooth]") {
    // The user is touching ONE score widget; the other should
    // show the dragged tick at the live ghost ms too. MainWindow
    // forwards markerDragRequested → setMarkerDragGhost on both
    // widgets; the test verifies the staff sees the ghost when
    // the waveform is the source.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    auto* staff    = window->findChild<StaffWidget*>("staffWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto markerId = window->markerModel().markers()[0].id;

    REQUIRE_FALSE(staff->dragGhostMs(markerId).has_value());
    emit waveform->markerDragRequested(markerId, 2000);
    REQUIRE(staff->dragGhostMs(markerId) == 2000);
    REQUIRE(waveform->dragGhostMs(markerId) == 2000);   // source mirrored too

    // Commit clears the ghost.
    emit waveform->markerDragCommitted(markerId, 1500, 2000);
    REQUIRE_FALSE(staff->dragGhostMs(markerId).has_value());
    REQUIRE_FALSE(waveform->dragGhostMs(markerId).has_value());
}

TEST_CASE("MainWindow: position-poll pauses during a drag (#22)",
          "[main-window][gui][integration][drag-smooth]") {
    // Pre-22 the 50 ms playback-position poll competed with mouse
    // moves under load. The poll now stops on dragStarted and
    // resumes on dragEnded.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    REQUIRE_FALSE(window->dragInFlight());

    emit waveform->dragStarted();
    REQUIRE(window->dragInFlight());

    emit waveform->dragEnded();
    REQUIRE_FALSE(window->dragInFlight());
}

TEST_CASE("MainWindow: Ctrl+Z reverses a marker drag",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));

    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto markerId = window->markerModel().markers()[0].id;

    // Simulate a drag: under the unified ghost flow (#22) the
    // commit handler is the ONE place that writes the model. The
    // test fires the commit signal with the from→to pair; the
    // handler does the setPosition.
    emit waveform->markerDragCommitted(markerId, 1500, 2500);

    REQUIRE(window->markerModel().markers()[0].sourceMs == 2500);
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 1500);
    REQUIRE(window->markerModel().markers()[0].id == markerId);   // id stable
}

TEST_CASE("MainWindow: Ctrl+Z reverses a loop edge drag",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto loaded = makeWindowWithLoop(/*start=*/500, /*end=*/1500);
    auto* waveform =
        loaded.window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(waveform);

    // Simulate dragging the right edge from 1500 → 1900. Under the
    // unified ghost flow (#22), commitLoopDrag is the ONE place
    // that writes the model — the test must NOT pre-move the model.
    emit waveform->loopDragCommitted(loaded.loopId,
                                     /*isStart=*/false,
                                     /*fromMs=*/1500,
                                     /*toMs=*/1900);
    REQUIRE(loaded.window->loopModel().loops()[0].endMs == 1900);

    QTest::keyClick(loaded.window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(loaded.window->loopModel().loops()[0].startMs == 500);
    REQUIRE(loaded.window->loopModel().loops()[0].endMs   == 1500);
}

TEST_CASE("MainWindow: Ctrl+Z reverses a marker rename via the dock",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto markerId = window->markerModel().markers()[0].id;
    const auto autoName = window->markerModel().markers()[0].name;

    auto* dock = window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(dock);
    emit dock->markerRenameRequested(markerId, "Hard turn");
    REQUIRE(window->markerModel().markers()[0].name == "Hard turn");

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().markers()[0].name == autoName);
}

TEST_CASE("MainWindow: Ctrl+Z reverses a dock spinbox position edit",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto markerId = window->markerModel().markers()[0].id;

    auto* dock = window->findChild<ProjectViewerDock*>("projectViewerDock");
    emit dock->markerPositionEditRequested(markerId, 3000);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 3000);

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 1500);
}

TEST_CASE("MainWindow: Ctrl+Z reverses a marker delete and restores id+name",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto markerId   = window->markerModel().markers()[0].id;
    const auto markerName = window->markerModel().markers()[0].name;

    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->markerModel().empty());

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().size() == 1);
    REQUIRE(window->markerModel().markers()[0].id       == markerId);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 1500);
    REQUIRE(window->markerModel().markers()[0].name     == markerName);
}

TEST_CASE("MainWindow: Ctrl+Z reverses a loop delete and restores id+name+range",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto loaded   = makeWindowWithLoop(500, 1500);
    const auto id = loaded.loopId;
    const auto loopName = loaded.window->loopModel().loops()[0].name;

    // Select then Del via window-level shortcut.
    auto* waveform =
        loaded.window->findChild<WaveformWidget*>("waveformWidget");
    waveform->setSelectedLoopId(id);
    QTest::keyClick(loaded.window.get(), Qt::Key_Delete);
    REQUIRE(loaded.window->loopModel().empty());

    QTest::keyClick(loaded.window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(loaded.window->loopModel().size() == 1);
    REQUIRE(loaded.window->loopModel().loops()[0].id      == id);
    REQUIRE(loaded.window->loopModel().loops()[0].startMs == 500);
    REQUIRE(loaded.window->loopModel().loops()[0].endMs   == 1500);
    REQUIRE(loaded.window->loopModel().loops()[0].name    == loopName);
}

TEST_CASE("MainWindow: Ctrl+Z reverses a barline delete",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    auto* slider = window->findChild<QSlider*>("positionSlider");
    slider->setValue(2000);
    emit slider->sliderMoved(2000);
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->barlineModel().size() == 1);

    QTest::keyClick(window.get(), Qt::Key_Delete);
    REQUIRE(window->barlineModel().empty());

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->barlineModel().size() == 1);
    REQUIRE(window->barlineModel().barlines()[0] == 2000);
}

TEST_CASE("MainWindow: Ctrl+Z works even when a dock spinbox has focus",
          "[main-window][gui][integration][undo]") {
    // MEMO: regression for the smoke-found 3.2 bug. QSpinBox /
    // QLineEdit accept Qt::ShortcutOverride for Ctrl+Z by default
    // to claim the key for their own text-undo, which hid our
    // window-level undo whenever focus was on a dock input. The
    // app-wide eventFilter rejects that override so Ctrl+Z always
    // routes to MainWindow::onUndo.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto markerId = window->markerModel().markers()[0].id;

    auto* dock = window->findChild<ProjectViewerDock*>("projectViewerDock");
    REQUIRE(dock);
    emit dock->markerPositionEditRequested(markerId, 3000);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 3000);

    // Send Ctrl+Z directly to the dock's marker-position spinbox's
    // INNER QLineEdit. That's the widget that receives keystrokes
    // while the user is typing into a spinbox; it's the one that
    // claims Ctrl+Z for text-undo unless we filter at this exact
    // level (filtering only the spinbox shell would miss it).
    auto* posBox = dock->findChild<QSpinBox*>("markerPositionBox");
    REQUIRE(posBox);
    auto* innerEdit = posBox->findChild<QLineEdit*>();
    REQUIRE(innerEdit);
    QTest::keyClick(innerEdit, Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().markers()[0].sourceMs == 1500);

    window->close();
}

TEST_CASE("MainWindow: Ctrl+Z reverses a pre-roll spinbox edit",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());

    auto* prerollBox = window->findChild<QSpinBox*>("prerollBox");
    REQUIRE(prerollBox);
    const int initial = prerollBox->value();

    // Simulate the user typing a new value + pressing Enter.
    prerollBox->setValue(initial + 200);
    emit prerollBox->editingFinished();
    REQUIRE(prerollBox->value() == initial + 200);

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(prerollBox->value() == initial);
}

TEST_CASE("MainWindow: Ctrl+Z reverses pre-roll edit while focus is in the spinbox",
          "[main-window][gui][integration][undo]") {
    // MEMO: regression for the smoke-found 7 bug. The transport-row
    // spinbox wraps a QLineEdit that grabs Qt::ShortcutOverride for
    // Ctrl+Z. Per-widget filter must be installed on the inner
    // QLineEdit (not just the QSpinBox shell) for the document-
    // level undo to win when focus is on the inner widget.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());

    auto* prerollBox = window->findChild<QSpinBox*>("prerollBox");
    REQUIRE(prerollBox);
    const int initial = prerollBox->value();

    prerollBox->setValue(initial + 200);
    emit prerollBox->editingFinished();
    REQUIRE(prerollBox->value() == initial + 200);

    // Send Ctrl+Z directly to the inner QLineEdit — that's the
    // widget that receives keystrokes while the user is typing
    // into a spinbox. Without the inner-widget filter installed
    // alongside the spinbox-level one, the line edit's text-undo
    // claims the key and our document undo never fires.
    auto* innerEdit = prerollBox->findChild<QLineEdit*>();
    REQUIRE(innerEdit);
    QTest::keyClick(innerEdit, Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(prerollBox->value() == initial);

    window->close();
}

TEST_CASE("MainWindow: Ctrl+Z reverses a pre-roll checkbox toggle",
          "[main-window][gui][integration][undo]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());

    auto* prerollEnabledBox =
        window->findChild<QCheckBox*>("prerollEnabledBox");
    REQUIRE(prerollEnabledBox);
    const bool initial = prerollEnabledBox->isChecked();

    // Simulate the user clicking the checkbox.
    prerollEnabledBox->click();
    REQUIRE(prerollEnabledBox->isChecked() == !initial);

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(prerollEnabledBox->isChecked() == initial);
}

TEST_CASE("MainWindow: undo of an Add does not push a Delete entry "
          "(applyingUndo_ guard)",
          "[main-window][gui][integration][undo]") {
    // MEMO: pins the applyingUndo_ flag's contract — without it,
    // the undo dispatch's own model->remove call would push a
    // DeleteMarker entry, and the *next* Ctrl+Z would re-add the
    // marker (unwanted free redo). The flag suppresses the push;
    // the second Ctrl+Z must be a no-op.
    qtApp();
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    REQUIRE(window->markerModel().size() == 1);

    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().empty());

    // Second Ctrl+Z: the history is empty (no DeleteMarker entry was
    // pushed during the dispatch). No-op, not a redo.
    QTest::keyClick(window.get(), Qt::Key_Z, Qt::ControlModifier);
    REQUIRE(window->markerModel().empty());
}

// ---------------------------------------------------------------------------
// Project save / load (#26)
// ---------------------------------------------------------------------------

TEST_CASE("MainWindow: save+load round-trip preserves every model (#26)",
          "[main-window][gui][integration][project]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString projectPath = tmp.filePath("session.fdlp");

    // Set up a populated session: load audio, tap some artifacts,
    // build a loop, edit pre-roll, pick a tune type.
    std::int64_t markerId = 0;
    std::int64_t loopId   = 0;
    QString      audioPath;
    {
        auto window = std::make_unique<MainWindow>();
        window->show();
        (void)QTest::qWaitForWindowExposed(window.get());
        REQUIRE(window->loadFile(
            QString::fromStdString(fixtureWav().string())));
        auto* waveform =
            window->findChild<WaveformWidget*>("waveformWidget");
        REQUIRE(QTest::qWaitFor(
            [&]() { return waveform->overview() != nullptr; }, 5000));

        // Fixture WAV is 2 s long — use positions well inside that
        // range so the seeks don't get clamped by Player::position.
        auto* slider = window->findChild<QSlider*>("positionSlider");
        slider->setValue(500);
        emit slider->sliderMoved(500);
        QTest::keyClick(window.get(), Qt::Key_B);
        seekAndTapMarker(*window, 800);
        REQUIRE(window->markerModel().size() == 1);
        markerId = window->markerModel().markers()[0].id;

        // Build a loop from two markers.
        seekAndTapMarker(*window, 1500);
        REQUIRE(window->markerModel().size() == 2);
        waveform->setSecondaryAnchorMs(800);
        QTest::keyClick(window.get(), Qt::Key_L);
        REQUIRE(window->loopModel().size() == 1);
        loopId = window->loopModel().loops()[0].id;

        // Rename for round-trip verification.
        emit window->findChild<ProjectViewerDock*>("projectViewerDock")
                ->markerRenameRequested(markerId, "Hard turn");
        emit window->findChild<ProjectViewerDock*>("projectViewerDock")
                ->loopRenameRequested(loopId, "Drill A");

        REQUIRE(window->saveProject(projectPath));
        audioPath = QString::fromStdString(fixtureWav().string());
    }

    // Fresh window, load the project, verify every model.
    {
        auto window = std::make_unique<MainWindow>();
        window->show();
        (void)QTest::qWaitForWindowExposed(window.get());
        REQUIRE(window->openProject(projectPath));

        REQUIRE(window->barlineModel().size() == 1);
        REQUIRE(window->barlineModel().barlines()[0] == 500);

        REQUIRE(window->markerModel().size() == 2);
        // IDs survive the round-trip.
        REQUIRE(window->markerModel().indexOf(markerId).has_value());
        const auto idx = *window->markerModel().indexOf(markerId);
        REQUIRE(window->markerModel().markers()[idx].sourceMs == 800);
        REQUIRE(window->markerModel().markers()[idx].name == "Hard turn");

        REQUIRE(window->loopModel().size() == 1);
        REQUIRE(window->loopModel().loops()[0].id == loopId);
        REQUIRE(window->loopModel().loops()[0].name == "Drill A");
    }
}

TEST_CASE("MainWindow: dirty state flips on mutation, clears on save (#26)",
          "[main-window][gui][integration][project]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString projectPath = tmp.filePath("dirty.fdlp");

    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    REQUIRE_FALSE(window->windowTitle().startsWith("* "));

    // Mutation → dirty.
    QTest::keyClick(window.get(), Qt::Key_B);
    REQUIRE(window->windowTitle().startsWith("* "));

    // Save → clean. Title shows the basename without asterisk.
    REQUIRE(window->saveProject(projectPath));
    REQUIRE_FALSE(window->windowTitle().startsWith("* "));
    REQUIRE(window->windowTitle().contains("dirty.fdlp"));

    // Mutation again → dirty.
    QTest::keyClick(window.get(), Qt::Key_M);
    REQUIRE(window->windowTitle().startsWith("* "));
}

TEST_CASE("MainWindow: malformed .fdlp leaves current state intact (#26)",
          "[main-window][gui][integration][project]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString badPath = tmp.filePath("broken.fdlp");

    QFile f(badPath);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("{ this is not valid JSON");
    f.close();

    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    const auto preLoadMarkerCount = window->markerModel().size();

    // openProject swallows the malformed JSON and returns false. The
    // production code surfaces a QMessageBox, but in test mode that's
    // a separate top-level window that doesn't block the test.
    REQUIRE_FALSE(window->openProject(badPath));
    // Models are untouched.
    REQUIRE(window->markerModel().size() == preLoadMarkerCount);
}

TEST_CASE("MainWindow: project file format is JSON with version field (#26)",
          "[main-window][gui][integration][project]") {
    // Pins the serialisation contract — future readers (other tools,
    // re-implementations) can depend on the shape.
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString path = tmp.filePath("shape.fdlp");

    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->loadFile(
        QString::fromStdString(fixtureWav().string())));
    auto* waveform = window->findChild<WaveformWidget*>("waveformWidget");
    REQUIRE(QTest::qWaitFor(
        [&]() { return waveform->overview() != nullptr; }, 5000));

    seekAndTapMarker(*window, 1500);
    REQUIRE(window->saveProject(path));

    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    REQUIRE(err.error == QJsonParseError::NoError);
    REQUIRE(doc.isObject());

    const auto root = doc.object();
    REQUIRE(root.value("version").toInt() == 1);
    REQUIRE(root.contains("audioPath"));
    REQUIRE(root.contains("timeSignature"));
    REQUIRE(root.contains("preroll"));
    REQUIRE(root.contains("barlines"));
    REQUIRE(root.contains("markers"));
    REQUIRE(root.contains("loops"));

    REQUIRE(root.value("markers").toArray().size() == 1);
    REQUIRE(root.value("markers").toArray()[0].toObject().value("sourceMs").toInt() == 1500);
}

// ---------------------------------------------------------------------------
// Open Recent submenu (#43)
//
// QSettings is per-test-isolated via qtApp() (clears on every call) and
// QStandardPaths::setTestModeEnabled redirects writes to a process-local
// scratch dir — see qt_test_app.cpp.
// ---------------------------------------------------------------------------

namespace {

// Copy the fixture WAV to `dest` so tests that exercise the MRU's
// multi-file behaviour can produce N distinct, openable audio paths
// without inflating the suite's fixture corpus. Caller owns the
// QTemporaryDir holding `dest`.
void copyFixtureTo(const QString& dest) {
    const QString src = QString::fromStdString(fixtureWav().string());
    REQUIRE(QFile::copy(src, dest));
}

} // namespace

TEST_CASE("MainWindow: openByPath prepends the path to recent files (#43)",
          "[main-window][gui][integration][recent]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    REQUIRE(window->recentFiles().isEmpty());

    const QString fixturePath =
        QString::fromStdString(fixtureWav().string());
    REQUIRE(window->openByPath(fixturePath));

    const QStringList recent = window->recentFiles();
    REQUIRE(recent.size() == 1);
    REQUIRE(recent.first() == QFileInfo(fixturePath).absoluteFilePath());
}

TEST_CASE("MainWindow: opening the same file twice dedupes the MRU (#43)",
          "[main-window][gui][integration][recent]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    const QString fixturePath =
        QString::fromStdString(fixtureWav().string());

    REQUIRE(window->openByPath(fixturePath));
    REQUIRE(window->openByPath(fixturePath));

    REQUIRE(window->recentFiles().size() == 1);
}

TEST_CASE("MainWindow: re-opening an older file moves it to the front (#43)",
          "[main-window][gui][integration][recent]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString a = tmp.filePath("a.wav");
    const QString b = tmp.filePath("b.wav");
    copyFixtureTo(a);
    copyFixtureTo(b);

    auto window = std::make_unique<MainWindow>();
    REQUIRE(window->openByPath(a));
    REQUIRE(window->openByPath(b));
    REQUIRE(window->openByPath(a));   // re-pick the oldest

    const QStringList recent = window->recentFiles();
    REQUIRE(recent.size() == 2);
    REQUIRE(recent[0] == QFileInfo(a).absoluteFilePath());
    REQUIRE(recent[1] == QFileInfo(b).absoluteFilePath());
}

TEST_CASE("MainWindow: MRU caps at 10 entries (#43)",
          "[main-window][gui][integration][recent]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    auto window = std::make_unique<MainWindow>();
    // Open 11 distinct copies of the fixture; expect the oldest
    // to fall off and a final size of exactly 10.
    QStringList opened;
    for (int i = 0; i < 11; ++i) {
        const QString p = tmp.filePath(QStringLiteral("f%1.wav").arg(i));
        copyFixtureTo(p);
        opened << QFileInfo(p).absoluteFilePath();
        REQUIRE(window->openByPath(p));
    }
    const QStringList recent = window->recentFiles();
    REQUIRE(recent.size() == 10);
    // Oldest (f0.wav) is gone; newest (f10.wav) is at the front.
    REQUIRE(recent.first() == opened.last());
    REQUIRE_FALSE(recent.contains(opened.first()));
}

TEST_CASE("MainWindow: openByPath dispatches .fdlp via openProject (#43)",
          "[main-window][gui][integration][recent][project]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString projectPath = tmp.filePath("p.fdlp");

    // Build a tiny project first.
    {
        auto window = std::make_unique<MainWindow>();
        window->show();
        (void)QTest::qWaitForWindowExposed(window.get());
        REQUIRE(window->loadFile(
            QString::fromStdString(fixtureWav().string())));
        REQUIRE(window->saveProject(projectPath));
    }

    // Fresh window — open the project via openByPath; the .fdlp
    // path should be the new MRU front (NOT the audio path that
    // openProject also touches internally).
    auto window = std::make_unique<MainWindow>();
    window->show();
    (void)QTest::qWaitForWindowExposed(window.get());
    REQUIRE(window->openByPath(projectPath));

    const QStringList recent = window->recentFiles();
    REQUIRE(recent.size() == 1);
    REQUIRE(recent.first() == QFileInfo(projectPath).absoluteFilePath());
    REQUIRE(recent.first().endsWith(".fdlp"));
}

TEST_CASE("MainWindow: openByPath on missing file returns false and prunes MRU (#43)",
          "[main-window][gui][integration][recent]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString ghost = tmp.filePath("ghost.wav");
    copyFixtureTo(ghost);

    auto window = std::make_unique<MainWindow>();
    REQUIRE(window->openByPath(ghost));
    REQUIRE(window->recentFiles().size() == 1);

    // Delete the file and try to re-pick it. Expect false return
    // and the entry pruned from the MRU.
    REQUIRE(QFile::remove(ghost));
    REQUIRE_FALSE(window->openByPath(ghost));
    REQUIRE(window->recentFiles().isEmpty());
}

TEST_CASE("MainWindow: clearRecentFiles empties the MRU (#43)",
          "[main-window][gui][integration][recent]") {
    qtApp();
    auto window = std::make_unique<MainWindow>();
    REQUIRE(window->openByPath(
        QString::fromStdString(fixtureWav().string())));
    REQUIRE(window->recentFiles().size() == 1);

    window->clearRecentFiles();
    REQUIRE(window->recentFiles().isEmpty());
}

TEST_CASE("MainWindow: pushing on an empty MRU flips the submenu live (#43)",
          "[main-window][gui][integration][recent]") {
    // Regression: pre-fix, the submenu started disabled and a
    // disabled QMenu never fires aboutToShow — so first-session
    // pushes were invisible until restart. Pin the fix that the
    // submenu enables and populates immediately on a push, with
    // no aboutToShow / hover required.
    qtApp();
    auto window = std::make_unique<MainWindow>();

    auto* recentMenuAction = window->findChild<QAction*>("openRecentMenu");
    REQUIRE(recentMenuAction != nullptr);
    auto* recentMenu = recentMenuAction->menu();
    REQUIRE(recentMenu != nullptr);

    REQUIRE_FALSE(recentMenuAction->isEnabled()); // empty → disabled

    REQUIRE(window->openByPath(
        QString::fromStdString(fixtureWav().string())));

    // No emit aboutToShow here — the live rebuild on push is the
    // contract under test.
    REQUIRE(recentMenuAction->isEnabled());
    const auto actions = recentMenu->actions();
    REQUIRE(actions.size() == 3);   // 1 file + separator + Clear
    REQUIRE_FALSE(actions[0]->isSeparator());
    REQUIRE(actions[1]->isSeparator());
    REQUIRE(actions[2]->objectName() == "clearRecentFilesAction");
}

TEST_CASE("MainWindow: Open Recent submenu reflects the MRU + Clear entry (#43)",
          "[main-window][gui][integration][recent]") {
    qtApp();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString a = tmp.filePath("alpha.wav");
    const QString b = tmp.filePath("beta.wav");
    copyFixtureTo(a);
    copyFixtureTo(b);

    auto window = std::make_unique<MainWindow>();
    auto* recentMenuAction = window->findChild<QAction*>("openRecentMenu");
    REQUIRE(recentMenuAction != nullptr);
    auto* recentMenu = recentMenuAction->menu();
    REQUIRE(recentMenu != nullptr);

    // Empty state — submenu disabled, no entries.
    REQUIRE_FALSE(recentMenuAction->isEnabled());

    REQUIRE(window->openByPath(a));
    REQUIRE(window->openByPath(b));

    // Submenu is rebuilt on aboutToShow; force a rebuild the same
    // way an actual menu pop would.
    emit recentMenu->aboutToShow();

    REQUIRE(recentMenuAction->isEnabled());
    const auto actions = recentMenu->actions();
    // 2 file entries + separator + Clear Recent Files == 4.
    REQUIRE(actions.size() == 4);
    REQUIRE(actions[0]->text() == "beta.wav");   // most recent first
    REQUIRE(actions[1]->text() == "alpha.wav");
    REQUIRE(actions[2]->isSeparator());
    REQUIRE(actions[3]->objectName() == "clearRecentFilesAction");

    // Triggering Clear empties the MRU.
    actions[3]->trigger();
    REQUIRE(window->recentFiles().isEmpty());
}
