// fiddler-screenshots — generates docs/img/*.png for the user
// manual. Reads tools/screenshots/scenes.yaml, runs each scene
// against a real MainWindow under offscreen rendering, saves the
// captures into docs/img/.
//
// Usage:
//   fiddler-screenshots [--output <dir>] [--manifest <path>]
//
// Defaults are baked at configure time:
//   --output    docs/img/
//   --manifest  tools/screenshots/scenes.yaml

#include "manifest.h"
#include "scene_runner.h"
#include "ui/MainWindow.h"
#include "util/Log.h"

#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QString>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = fiddler::screenshots;

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Test-mode QSettings so the tool is independent of whatever
    // saved layout the local user has — same isolation pattern the
    // GUI tests use.
    QStandardPaths::setTestModeEnabled(true);

    QApplication app(argc, argv);
    fiddler::log::init({});

    QString outputDirStr = QString::fromUtf8(
        FIDDLER_SCREENSHOTS_DEFAULT_OUTPUT);
    QString manifestPath = QString::fromUtf8(
        FIDDLER_SCREENSHOTS_DEFAULT_MANIFEST);

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--output" && i + 1 < argc) {
            outputDirStr = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--manifest" && i + 1 < argc) {
            manifestPath = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: fiddler-screenshots [--output <dir>] "
                "[--manifest <path>]\n";
            return 0;
        }
    }

    QDir outputDir(outputDirStr);
    if (!outputDir.exists() && !outputDir.mkpath(".")) {
        std::cerr << "Could not create output directory: "
                  << outputDirStr.toStdString() << "\n";
        return 1;
    }

    std::vector<fs::SceneSpec> scenes;
    try {
        scenes = fs::parseManifest(
            std::filesystem::path(manifestPath.toStdString()));
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    std::cout << "fiddler-screenshots\n"
              << "  manifest: " << manifestPath.toStdString() << "\n"
              << "  output:   " << outputDir.absolutePath().toStdString()
              << "\n  scenes:   " << scenes.size() << "\n\n";

    fiddler::ui::MainWindow window;
    fs::resetWindow(window);

    int failures = 0;
    for (const auto& scene : scenes) {
        try {
            if (!fs::runScene(window, outputDir, scene)) ++failures;
        } catch (const std::exception& e) {
            std::cerr << "screenshot: '" << scene.id.toStdString()
                      << "' threw: " << e.what() << "\n";
            ++failures;
        }
    }

    if (failures > 0) {
        std::cerr << "\n" << failures << " scene(s) failed.\n";
        return 1;
    }
    std::cout << "\nAll scenes captured.\n";
    return 0;
}
