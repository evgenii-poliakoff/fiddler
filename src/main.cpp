// Fiddler — entry point.
//
// Parses logging flags, initialises the logging facade, then starts the
// Qt event loop with the main window.

#include "ui/MainWindow.h"
#include "util/Log.h"

#include <QApplication>
#include <QCommandLineParser>

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Fiddler");
    QApplication::setOrganizationName("Fiddler");
    QApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Transcription aid for self-taught fiddle players");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption logLevelOpt({"l", "log-level"},
        "Log threshold: trace|debug|info|warn|error|off (default: warn). "
        "Overridden by FIDDLER_LOG_LEVEL env var.",
        "level");
    QCommandLineOption logFilterOpt("log-filter",
        "Category glob filter, e.g. 'player.*' or 'decoder' (default: '*').",
        "glob", "*");
    QCommandLineOption logFileOpt("log-file",
        "Also write logs to this file (rotated at 5 MiB, 3 backups).",
        "path");

    parser.addOption(logLevelOpt);
    parser.addOption(logFilterOpt);
    parser.addOption(logFileOpt);
    parser.process(app);

    fiddler::log::Config logCfg;
    logCfg.level  = fiddler::log::Level::Warn;
    logCfg.filter = parser.value(logFilterOpt).toStdString();

    // Env var first, then CLI overrides it.
    if (const char* env = std::getenv("FIDDLER_LOG_LEVEL")) {
        if (auto lvl = fiddler::log::parseLevel(env)) {
            logCfg.level = *lvl;
        }
    }
    if (parser.isSet(logLevelOpt)) {
        const auto s = parser.value(logLevelOpt).toStdString();
        if (auto lvl = fiddler::log::parseLevel(s)) {
            logCfg.level = *lvl;
        } else {
            std::cerr << "Unknown log level: " << s << "\n";
            return 2;
        }
    }
    if (parser.isSet(logFileOpt)) {
        logCfg.logFile = parser.value(logFileOpt).toStdString();
    }

    fiddler::log::init(logCfg);
    FLOG_INFO("app", "fiddler {} starting up",
              QApplication::applicationVersion().toStdString());

    fiddler::ui::MainWindow window;
    window.resize(960, 600);
    window.show();
    const int rc = QApplication::exec();
    FLOG_INFO("app", "exiting with code {}", rc);
    return rc;
}
