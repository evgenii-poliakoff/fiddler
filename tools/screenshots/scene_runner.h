// scene_runner — the infrastructure that turns a SceneSpec
// (parsed from scenes.yaml) into a saved PNG. The runner owns
// nothing the manifest can express: no hardcoded sizes, no
// hardcoded margins, no hardcoded coordinates. Defaults that the
// manifest leaves out are computed from the running widgets'
// own properties (style metrics, sizeHint, font metrics).
//
// Capture flavours:
//   * Window  — full main-window grab. Used for chapter-opener
//               and welcome-page hero shots.
//   * Region  — bounding box of one or more named widgets,
//               unioned in main-window coordinates. The dominant
//               shape; matches Apple/Ableton's "cropped close-up
//               of one cluster of related controls" idiom.
//   * Element — one named widget, padded with the style's
//               default margin so the widget sits on its parent's
//               background colour rather than transparent void.

#pragma once

#include "manifest.h"

#include <QDir>

namespace fiddler::ui { class MainWindow; }

namespace fiddler::screenshots {

// Bring the window into a deterministic post-startup state ready
// for the first scene to set up: visible, dock visible at its
// natural width, layout flushed. Window size is derived from the
// window's own sizeHint() — no magic constant.
void resetWindow(ui::MainWindow& window);

// Force the GUI event loop to drain so deferred layout / paint
// settles before geometry queries or grabs.
void flushEvents();

// Load the screenshot tool's fixture WAV via MainWindow::loadFile
// and wait (bounded) for the waveform overview to finish building.
// Used by the `load-fixture` setup action.
void loadFixture(ui::MainWindow& window);

// Capture one scene. Reads `spec.capture` to dispatch on flavour;
// runs `spec.setup` actions before grabbing; saves the result to
// `outputDir/spec.output`. Returns true on success, false if the
// scene's capture target couldn't be resolved (logged to stderr).
bool runScene(ui::MainWindow& window,
              const QDir& outputDir,
              const SceneSpec& spec);

} // namespace fiddler::screenshots
