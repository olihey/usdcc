#include "test_harness.h"

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

int runTests(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& test : registry()) {
        if (filter && !std::strstr(test.name, filter)) {
            continue;
        }
        const int failuresBefore = failures();
        std::printf("[%s]\n", test.name);
        test.run();
        if (failures() != failuresBefore) {
            std::printf("  -> %d failure(s)\n", failures() - failuresBefore);
        }
        ++ran;
    }
    std::printf("\n%d test(s), %d checks passed, %d failed\n", ran, passes(), failures());
    return failures() == 0 && ran > 0 ? 0 : 1;
}

}  // namespace usdcc::test
