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

    fiddler::log::CliInputs in;
    in.levelExplicit  = parser.isSet(logLevelOpt);
    in.levelStr       = parser.value(logLevelOpt).toStdString();
    in.filterExplicit = parser.isSet(logFilterOpt);
    in.filterStr      = parser.value(logFilterOpt).toStdString();
    if (parser.isSet(logFileOpt)) {
        in.logFile = parser.value(logFileOpt).toStdString();
    }
    if (const char* env = std::getenv("FIDDLER_LOG_LEVEL")) {
        in.envLevel = env;
    }

    auto resolved = fiddler::log::resolveLogConfig(in);
    if (resolved.error) {
        std::cerr << *resolved.error << "\n";
        return 2;
    }

    fiddler::log::init(resolved.config);
    // MEMO: print the effective config to stderr unconditionally, so
    // the user always knows what level + filter are active —
    // independent of whatever --log-filter they passed. Going through
    // FLOG_INFO would tag this with category "app" which a narrow
    // filter like "ui.*,score" silently excludes; the banner would
    // then be invisible exactly when it's most useful.
    const char* levelStr = "info";
    switch (resolved.config.level) {
    case fiddler::log::Level::Trace: levelStr = "trace"; break;
    case fiddler::log::Level::Debug: levelStr = "debug"; break;
    case fiddler::log::Level::Info:  levelStr = "info";  break;
    case fiddler::log::Level::Warn:  levelStr = "warn";  break;
    case fiddler::log::Level::Error: levelStr = "error"; break;
    case fiddler::log::Level::Off:   levelStr = "off";   break;
    }
    std::cerr << "fiddler "
              << QApplication::applicationVersion().toStdString()
              << " | log level=" << levelStr
              << " filter='"     << resolved.config.filter << "'\n";
    FLOG_INFO("app", "fiddler {} starting up",
              QApplication::applicationVersion().toStdString());

    fiddler::ui::MainWindow window;
    window.resize(960, 600);
    window.show();
    const int rc = QApplication::exec();
    FLOG_INFO("app", "exiting with code {}", rc);
    return rc;
}
