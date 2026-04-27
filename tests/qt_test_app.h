// Lazy QApplication for GUI tests. The first call selects the
// "offscreen" platform plugin (so the suite runs headless without an
// X server / wayland compositor) and then constructs the QApplication
// — both happen exactly once, regardless of how many tests touch
// widgets. Subsequent calls just hand back the same instance.
//
// Usage from a test case:
//
//     TEST_CASE("...", "[gui]") {
//         fiddler::test::qtApp();   // ensure QApp exists
//         MyWidget w;
//         QTest::mouseClick(&w, Qt::LeftButton);
//         ...
//     }

#pragma once

class QApplication;

namespace fiddler::test {

QApplication& qtApp();

} // namespace fiddler::test
