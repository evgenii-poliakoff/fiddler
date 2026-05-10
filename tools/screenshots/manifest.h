// manifest — parses tools/screenshots/scenes.yaml into in-memory
// SceneSpec records that the runner can dispatch on.
//
// The format is documented at the top of scenes.yaml itself. This
// header just defines the data shapes the parser produces; the
// parser implementation is in manifest.cpp.

#pragma once

#include <QString>
#include <QStringList>

#include <filesystem>
#include <optional>
#include <vector>

namespace fiddler::screenshots {

// What the scene captures. Variant-style: exactly one of the four
// shapes is meaningful per scene, picked by `kind`.
enum class CaptureKind { Window, Region, Element, Popup };

struct CaptureSpec {
    CaptureKind kind = CaptureKind::Window;
    // For Region: the object names of widgets whose union forms
    // the capture rect. For Element: a one-name list (the widget
    // to grab). For Popup: a one-name list (the combo whose popup
    // is open and should be stacked under the closed cell). For
    // Window: empty.
    QStringList objectNames;
};

// One step of a scene's setup chain. The action name is looked up
// in scene_actions's registry; the optional positional argument
// is interpreted by the action.
struct ActionStep {
    QString name;
    QString arg;        // empty when the action takes no arg
};

// Optional per-scene overrides. Each std::optional left empty
// means "use the runner's style-derived default".
struct MarginSpec {
    std::optional<int> horizontal;
    std::optional<int> vertical;
};

struct SceneSpec {
    QString id;
    QString output;
    CaptureSpec capture;
    std::vector<ActionStep> setup;
    MarginSpec margin;
    std::optional<double> dpiScale;
};

// Parse the manifest file. Throws std::runtime_error with a
// readable message on schema violations / YAML errors. The path is
// resolved as-given (absolute or relative to the caller's CWD).
std::vector<SceneSpec>
parseManifest(const std::filesystem::path& manifestPath);

} // namespace fiddler::screenshots
