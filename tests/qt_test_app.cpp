#include "qt_test_app.h"

#include <QApplication>
#include <QByteArray>

#include <catch2/catch_session.hpp>

namespace fiddler::test {

namespace {
QApplication* g_app = nullptr;
} // namespace

QApplication& qtApp() { return *g_app; }

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
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    fiddler::test::g_app = &app;
    return Catch::Session().run(argc, argv);
}
