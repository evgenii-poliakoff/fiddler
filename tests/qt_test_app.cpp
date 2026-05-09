#include "qt_test_app.h"

#include <QApplication>
#include <QByteArray>
#include <QSettings>
#include <QStandardPaths>

#include <catch2/catch_session.hpp>

namespace fiddler::test {

namespace {
QApplication* g_app = nullptr;
} // namespace

QApplication& qtApp() {
    // MEMO: clear QSettings every time a test reaches for the
    // QApplication. MainWindow saves layout state in closeEvent
    // and restores it in the constructor; without this clear,
    // a test that closes the dock would leak its state into the
    // next test (which would then see a hidden dock and fail
    // assertions about default visibility). The clear is cheap
    // and the test mode redirects to a per-process temp dir, so
    // we never touch the user's real config.
    QSettings().clear();
    return *g_app;
}

} // namespace fiddler::test

// Custom main() so QApplication is stack-allocated and torn down
// before any of Qt's static globals — function-local-static
// QApplication runs into destruction-order bugs with Qt's internal
// statics on Linux (Qt 6.4 / Ubuntu 24.04).
//
// We also force QT_QPA_PLATFORM=offscreen here so the suite runs
// headless. CMake also exports the same env via catch_discover_tests
// PROPERTIES — both as belt-and-suspenders so direct invocation
// (./build/tests/fiddler_tests) works without needing a wrapper.
//
// MEMO: enable QStandardPaths test mode BEFORE constructing the
// QApplication so QSettings (and any other path-resolving Qt API)
// redirects to a per-process scratch directory under
// /tmp/.../qttest. Without this, MainWindow's saveLayout() in
// closeEvent would write into ~/.config/Fiddler/Fiddler.conf —
// polluting the real user's config when the suite runs locally.
// The application + organization names match main.cpp so the
// QSettings path resolves the same way (modulo the test-mode
// redirect).
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication::setApplicationName("Fiddler");
    QApplication::setOrganizationName("Fiddler");
    QApplication app(argc, argv);
    fiddler::test::g_app = &app;
    return Catch::Session().run(argc, argv);
}
