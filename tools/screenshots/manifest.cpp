#include "manifest.h"

#include <yaml-cpp/yaml.h>

#include <stdexcept>
#include <string>

namespace fiddler::screenshots {

namespace {

[[noreturn]] void throwSchema(const std::string& msg, const YAML::Node& node) {
    std::string out = "scenes.yaml: " + msg;
    if (node.Mark().line >= 0) {
        out += " (line " + std::to_string(node.Mark().line + 1) + ")";
    }
    throw std::runtime_error(out);
}

// Parse the `capture:` field. Four shapes:
//   capture: window
//   capture: { region: [a, b, c] }
//   capture: { element: name }
//   capture: { popup: combo-name }
CaptureSpec parseCapture(const YAML::Node& cap) {
    CaptureSpec out;
    if (cap.IsScalar()) {
        const auto s = cap.as<std::string>();
        if (s == "window") {
            out.kind = CaptureKind::Window;
            return out;
        }
        throwSchema("capture: scalar must be 'window' (got '" + s + "')", cap);
    }
    if (!cap.IsMap()) {
        throwSchema("capture: must be 'window' or a map with "
                    "'region:', 'element:', or 'popup:'", cap);
    }
    if (auto region = cap["region"]) {
        out.kind = CaptureKind::Region;
        if (!region.IsSequence()) {
            throwSchema("capture.region: must be a sequence of "
                        "object names", region);
        }
        for (const auto& n : region) {
            out.objectNames.append(QString::fromStdString(
                n.as<std::string>()));
        }
        return out;
    }
    if (auto element = cap["element"]) {
        out.kind = CaptureKind::Element;
        out.objectNames.append(QString::fromStdString(
            element.as<std::string>()));
        return out;
    }
    if (auto popup = cap["popup"]) {
        out.kind = CaptureKind::Popup;
        out.objectNames.append(QString::fromStdString(
            popup.as<std::string>()));
        return out;
    }
    throwSchema("capture: map must contain 'region', 'element', "
                "or 'popup'", cap);
}

// Parse the `setup:` list. Each entry is either:
//   - action-name                       (no arg)
//   - action-name: arg-string           (positional arg)
std::vector<ActionStep> parseSetup(const YAML::Node& setup) {
    std::vector<ActionStep> out;
    if (!setup.IsSequence()) {
        throwSchema("setup: must be a sequence", setup);
    }
    for (const auto& entry : setup) {
        ActionStep step;
        if (entry.IsScalar()) {
            step.name = QString::fromStdString(entry.as<std::string>());
        } else if (entry.IsMap() && entry.size() == 1) {
            const auto pair = *entry.begin();
            step.name = QString::fromStdString(pair.first.as<std::string>());
            step.arg  = QString::fromStdString(pair.second.as<std::string>());
        } else {
            throwSchema("setup: entries must be 'name' or "
                        "single-key map 'name: arg'", entry);
        }
        out.push_back(std::move(step));
    }
    return out;
}

MarginSpec parseMargin(const YAML::Node& m) {
    MarginSpec out;
    if (!m.IsMap()) {
        throwSchema("margin: must be a map with optional "
                    "'horizontal' and 'vertical' keys", m);
    }
    if (auto h = m["horizontal"]) out.horizontal = h.as<int>();
    if (auto v = m["vertical"])   out.vertical   = v.as<int>();
    return out;
}

SceneSpec parseScene(const YAML::Node& sc) {
    if (!sc.IsMap()) {
        throwSchema("scene entry must be a map", sc);
    }
    SceneSpec out;
    if (auto id = sc["id"]) {
        out.id = QString::fromStdString(id.as<std::string>());
    } else {
        throwSchema("scene missing required 'id'", sc);
    }
    if (auto output = sc["output"]) {
        out.output = QString::fromStdString(output.as<std::string>());
    } else {
        throwSchema("scene '" + out.id.toStdString() +
                    "' missing required 'output'", sc);
    }
    if (auto cap = sc["capture"]) {
        out.capture = parseCapture(cap);
    } else {
        throwSchema("scene '" + out.id.toStdString() +
                    "' missing required 'capture'", sc);
    }
    if (auto setup = sc["setup"]) {
        out.setup = parseSetup(setup);
    }
    if (auto m = sc["margin"]) {
        out.margin = parseMargin(m);
    }
    if (auto d = sc["dpi-scale"]) {
        out.dpiScale = d.as<double>();
    }
    return out;
}

} // namespace

std::vector<SceneSpec>
parseManifest(const std::filesystem::path& manifestPath) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(manifestPath.string());
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(
            "Failed to load " + manifestPath.string() +
            ": " + e.what());
    }
    if (!root.IsSequence()) {
        throw std::runtime_error(
            "scenes.yaml: top-level must be a sequence of scenes");
    }
    std::vector<SceneSpec> scenes;
    scenes.reserve(root.size());
    for (const auto& node : root) {
        scenes.push_back(parseScene(node));
    }
    return scenes;
}

} // namespace fiddler::screenshots
