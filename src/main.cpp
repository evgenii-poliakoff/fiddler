// Fiddler — entry point.
//
// Step 0: just bring up an empty window so the build pipeline is exercised.

#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Fiddler");
    QApplication::setOrganizationName("Fiddler");

    fiddler::ui::MainWindow window;
    window.resize(960, 600);
    window.show();
    return QApplication::exec();
}
