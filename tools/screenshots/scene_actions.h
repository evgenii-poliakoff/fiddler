// scene_actions — the registry of named setup actions a scene's
// YAML can reference. Each action is a function that mutates the
// MainWindow into a particular state (e.g. "load-fixture",
// "preroll-disable") before the runner captures.
//
// Adding a new action: write the function, register it in
// scene_actions.cpp's table at the bottom of the file. The YAML
// then references it by name; no parser change needed.

#pragma once

#include "manifest.h"

namespace fiddler::ui { class MainWindow; }

namespace fiddler::screenshots {

// Run a single action against the window. Throws std::runtime_error
// if the action name isn't registered.
void runAction(ui::MainWindow& window, const ActionStep& step);

// Run a chain of actions sequentially. Stops at the first failure.
void runActions(ui::MainWindow& window,
                const std::vector<ActionStep>& steps);

} // namespace fiddler::screenshots
