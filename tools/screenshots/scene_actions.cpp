#include "scene_actions.h"

#include "scene_runner.h"
#include "screenshot_fixture.h"
#include "score/LoopModel.h"
#include "score/MarkerModel.h"
#include "ui/MainWindow.h"
#include "ui/ProjectViewerDock.h"
#include "ui/WaveformWidget.h"

#include <QComboBox>
#include <QHash>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QTest>

#include <functional>
#include <optional>
#include <stdexcept>

namespace fiddler::ui {
class ProjectViewerDock;
}

namespace fiddler::screenshots {

namespace {

// Each action receives the window and the optional positional arg
// from the YAML. Actions that don't take arguments ignore it.
// Actions that take comma-separated lists parse them out of `arg`.
using ActionFn =
    std::function<void(ui::MainWindow&, const QString& arg)>;

// ---- helpers -----------------------------------------------------------

// Drive the position slider so the player seeks to `ms`. The
// slider's sliderMoved signal is what MainWindow listens on for
// seek-by-drag, so emitting it produces the same effect as a
// human dragging the slider.
void seekTo(ui::MainWindow& window, qint64 ms) {
    auto* slider = window.findChild<QSlider*>("positionSlider");
    if (!slider) return;
    slider->setValue(static_cast<int>(ms));
    emit slider->sliderMoved(static_cast<int>(ms));
    flushEvents();
}

// Send a synthetic key click at the window so the existing
// QShortcut path runs. Equivalent to a user pressing the key.
void pressKey(ui::MainWindow& window, Qt::Key key) {
    QTest::keyClick(&window, key);
    flushEvents();
}

// Parse "a,b,c" -> [a, b, c] as ms values.
std::vector<qint64> parseMsList(const QString& arg) {
    std::vector<qint64> out;
    for (const auto& part : arg.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 v = part.trimmed().toLongLong(&ok);
        if (ok) out.push_back(v);
    }
    return out;
}

// Parse "<a>,<b>" -> std::pair, both as ints. Returns nullopt if
// either side is missing or non-numeric.
std::optional<std::pair<int, int>> parseIntPair(const QString& arg) {
    const auto parts = arg.split(',', Qt::SkipEmptyParts);
    if (parts.size() != 2) return std::nullopt;
    bool ok1 = false, ok2 = false;
    const int a = parts[0].trimmed().toInt(&ok1);
    const int b = parts[1].trimmed().toInt(&ok2);
    if (!ok1 || !ok2) return std::nullopt;
    return std::make_pair(a, b);
}

// ---- individual actions ------------------------------------------------

void doLoadFixture(ui::MainWindow& window, const QString&) {
    loadFixture(window);
}

void doPrerollEnable(ui::MainWindow& window, const QString&) {
    window.setPrerollEnabled(true);
    flushEvents();
}

void doPrerollDisable(ui::MainWindow& window, const QString&) {
    window.setPrerollEnabled(false);
    flushEvents();
}

// Set the tempo slider to a given percent (25–100). Argument: the
// percent as an integer string, e.g. `set-tempo: 70`.
void doSetTempo(ui::MainWindow& window, const QString& arg) {
    bool ok = false;
    const int percent = arg.toInt(&ok);
    if (!ok) {
        throw std::runtime_error(
            "set-tempo: arg must be an integer percent (got '" +
            arg.toStdString() + "')");
    }
    auto* slider = window.findChild<QSlider*>("tempoSlider");
    if (!slider) return;
    slider->setValue(percent);
    emit slider->valueChanged(percent);
    flushEvents();
}

// Seek to a specific ms. Argument: the ms as an integer string.
void doSeekMs(ui::MainWindow& window, const QString& arg) {
    bool ok = false;
    const qint64 ms = arg.toLongLong(&ok);
    if (!ok) {
        throw std::runtime_error(
            "seek-ms: arg must be an integer (got '" +
            arg.toStdString() + "')");
    }
    seekTo(window, ms);
}

// Zoom the waveform + staff to a source-time range. Argument:
// "<startMs>,<endMs>". Used by the user-manual scene that shows
// the zoomed-in viewport with the horizontal scrollbar visible.
void doZoomToRange(ui::MainWindow& window, const QString& arg) {
    const auto parts = arg.split(',', Qt::SkipEmptyParts);
    if (parts.size() != 2) {
        throw std::runtime_error(
            "zoom-to-range: arg must be 'startMs,endMs' (got '" +
            arg.toStdString() + "')");
    }
    bool okStart = false, okEnd = false;
    const qint64 startMs = parts[0].toLongLong(&okStart);
    const qint64 endMs   = parts[1].toLongLong(&okEnd);
    if (!okStart || !okEnd) {
        throw std::runtime_error(
            "zoom-to-range: both args must be integer ms (got '" +
            arg.toStdString() + "')");
    }
    window.applyViewport(startMs, endMs);
    flushEvents();
}

// Place barlines at a comma-separated list of ms positions.
// Each position seeks the player and presses B.
void doTapBarlinesAt(ui::MainWindow& window, const QString& arg) {
    for (const qint64 ms : parseMsList(arg)) {
        seekTo(window, ms);
        pressKey(window, Qt::Key_B);
    }
}

// Place markers at a comma-separated list of ms positions.
void doTapMarkersAt(ui::MainWindow& window, const QString& arg) {
    for (const qint64 ms : parseMsList(arg)) {
        seekTo(window, ms);
        pressKey(window, Qt::Key_M);
    }
}

// Select the Nth marker (1-based) on the waveform. After this the
// dock mirrors the selection and the marker tick paints brighter.
void doSelectMarker(ui::MainWindow& window, const QString& arg) {
    bool ok = false;
    const int ordinal = arg.toInt(&ok);
    if (!ok || ordinal < 1) {
        throw std::runtime_error(
            "select-marker: arg must be a 1-based ordinal (got '" +
            arg.toStdString() + "')");
    }
    const auto& mm = window.markerModel();
    if (mm.size() < static_cast<std::size_t>(ordinal)) return;
    const auto id = mm.markers()[ordinal - 1].id;
    auto* waveform = window.findChild<ui::WaveformWidget*>("waveformWidget");
    if (waveform) waveform->setSelectedMarkerId(id);
    flushEvents();
}

// Select the Nth loop (1-based) on the waveform.
void doSelectLoop(ui::MainWindow& window, const QString& arg) {
    bool ok = false;
    const int ordinal = arg.toInt(&ok);
    if (!ok || ordinal < 1) {
        throw std::runtime_error(
            "select-loop: arg must be a 1-based ordinal (got '" +
            arg.toStdString() + "')");
    }
    const auto& lm = window.loopModel();
    if (lm.size() < static_cast<std::size_t>(ordinal)) return;
    const auto id = lm.loops()[ordinal - 1].id;
    auto* waveform = window.findChild<ui::WaveformWidget*>("waveformWidget");
    if (waveform) waveform->setSelectedLoopId(id);
    flushEvents();
}

// Build a loop spanning two existing markers (1-based ordinals).
// Mimics the user gesture: select marker B as primary, capture
// marker A's ms as the secondary anchor, press L.
void doBuildLoopMarkerPair(ui::MainWindow& window, const QString& arg) {
    const auto pair = parseIntPair(arg);
    if (!pair) {
        throw std::runtime_error(
            "build-loop-marker-pair: arg must be 'a,b' "
            "(1-based ordinals)");
    }
    const auto& mm = window.markerModel();
    const int a = pair->first;
    const int b = pair->second;
    if (mm.size() < static_cast<std::size_t>(std::max(a, b))) return;
    const auto& markerA = mm.markers()[a - 1];
    const auto& markerB = mm.markers()[b - 1];

    auto* waveform = window.findChild<ui::WaveformWidget*>("waveformWidget");
    if (!waveform) return;
    waveform->setSelectedMarkerId(markerB.id);
    waveform->setSecondaryAnchorMs(markerA.sourceMs);
    flushEvents();
    pressKey(window, Qt::Key_L);
}

// Open the named QComboBox's popup so its dropdown list becomes a
// top-level QFrame the runner can grab. The combo's view container
// (`combo->view()->parentWidget()`) is the popup; once shown, it
// has its own grab-able geometry. Argument: the combo's objectName.
void doShowPopup(ui::MainWindow& window, const QString& arg) {
    if (arg.isEmpty()) {
        throw std::runtime_error(
            "show-popup: arg must be a combo-box objectName");
    }
    auto* combo = window.findChild<QComboBox*>(arg);
    if (!combo) {
        throw std::runtime_error(
            "show-popup: no QComboBox named '" +
            arg.toStdString() + "'");
    }
    combo->showPopup();
    flushEvents();
}

// Enter the wrap-pause / pre-roll countdown for the Nth loop with
// the current pre-roll value. Uses MainWindow's existing test
// seam so the countdown widget is visibly mid-state for capture.
void doEnterCountdown(ui::MainWindow& window, const QString& arg) {
    bool ok = false;
    const int ordinal = arg.toInt(&ok);
    if (!ok || ordinal < 1) {
        throw std::runtime_error(
            "enter-countdown: arg must be a 1-based ordinal");
    }
    const auto& lm = window.loopModel();
    if (lm.size() < static_cast<std::size_t>(ordinal)) return;
    const auto id = lm.loops()[ordinal - 1].id;
    // The countdown widget reads its target from the currently-
    // selected loop in the dock; route through the dock so the
    // visual matches what the user would see.
    auto* dock = window.findChild<ui::ProjectViewerDock*>("projectViewerDock");
    if (dock) dock->setSelectedLoopId(id);
    window.enterWrapPauseForTest(id, /*prerollMs=*/window.prerollMsValue());
    flushEvents();
}

// ---- registry ---------------------------------------------------------

const QHash<QString, ActionFn>& registry() {
    static const QHash<QString, ActionFn> kRegistry = {
        {"load-fixture",            doLoadFixture},
        {"preroll-enable",          doPrerollEnable},
        {"preroll-disable",         doPrerollDisable},
        {"set-tempo",               doSetTempo},
        {"seek-ms",                 doSeekMs},
        {"zoom-to-range",           doZoomToRange},
        {"tap-barlines-at",         doTapBarlinesAt},
        {"tap-markers-at",          doTapMarkersAt},
        {"select-marker",           doSelectMarker},
        {"select-loop",             doSelectLoop},
        {"build-loop-marker-pair",  doBuildLoopMarkerPair},
        {"show-popup",              doShowPopup},
        {"enter-countdown",         doEnterCountdown},
    };
    return kRegistry;
}

} // namespace

void runAction(ui::MainWindow& window, const ActionStep& step) {
    const auto& reg = registry();
    const auto it = reg.find(step.name);
    if (it == reg.end()) {
        throw std::runtime_error(
            "scenes.yaml: unknown setup action '" +
            step.name.toStdString() + "'");
    }
    (*it)(window, step.arg);
}

void runActions(ui::MainWindow& window,
                const std::vector<ActionStep>& steps)
{
    for (const auto& step : steps) {
        runAction(window, step);
    }
}

} // namespace fiddler::screenshots
