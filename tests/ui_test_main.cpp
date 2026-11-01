#include "test_harness.h"

#include <QApplication>

int main(int argc, char** argv) {
    // Widgets need a full QApplication; CTest runs this on Qt's offscreen
    // platform (see CMakeLists.txt), so no window ever appears.
    QApplication app(argc, argv);
    return usdcc::test::runTests(argc, argv);
}
