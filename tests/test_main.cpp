#include "test_harness.h"

#include <QCoreApplication>

int main(int argc, char** argv) {
    // Some code under test (UndoManager's QUndoStack) expects a Qt application
    // object; nothing here needs a GUI.
    QCoreApplication app(argc, argv);
    return usdcc::test::runTests(argc, argv);
}
