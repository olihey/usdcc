#include "test_harness.h"

#include <QCoreApplication>

#include <cstring>

namespace usdcc::test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int& failures() {
    static int count = 0;
    return count;
}

int& passes() {
    static int count = 0;
    return count;
}

}  // namespace usdcc::test

int main(int argc, char** argv) {
    // Some code under test (UndoManager's QUndoStack) expects a Qt application
    // object; nothing here needs a GUI.
    QCoreApplication app(argc, argv);

    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& test : usdcc::test::registry()) {
        if (filter && !std::strstr(test.name, filter)) {
            continue;
        }
        const int failuresBefore = usdcc::test::failures();
        std::printf("[%s]\n", test.name);
        test.run();
        if (usdcc::test::failures() != failuresBefore) {
            std::printf("  -> %d failure(s)\n", usdcc::test::failures() - failuresBefore);
        }
        ++ran;
    }
    std::printf("\n%d test(s), %d checks passed, %d failed\n", ran, usdcc::test::passes(), usdcc::test::failures());
    return usdcc::test::failures() == 0 && ran > 0 ? 0 : 1;
}
