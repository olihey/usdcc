#pragma once

// A deliberately tiny test harness (no extra dependency): USDCC_TEST(name)
// registers a test function, CHECK(cond, fmt, ...) records a pass or prints a
// failure with a printf-style explanation, and runTests() (test_harness.cpp)
// runs everything (or only the tests whose names contain argv[1]) and returns
// non-zero on any failure, which is what CTest looks at. Each test executable
// has its own small main() that creates the Qt application object it needs —
// test_main.cpp (usdcc_tests, no GUI) or ui_test_main.cpp (usdcc_ui_tests).
//
// A crash prints a report to stderr before the process dies: the test it was in and
// the stack, with files and lines on Windows (DbgHelp and the PDBs), symbols elsewhere
// (backtrace()). CI keeps no crash dumps, so that report is all a rare crash leaves.
// usdcc_harness_crash_check (harness_crash_check.cpp) crashes on purpose to check it.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace usdcc::test {

struct TestCase {
    const char* name;
    std::function<void()> run;
};

std::vector<TestCase>& registry();
int& failures();
int& passes();

// Runs the registered tests whose names contain argv[1] (all without it);
// returns main()'s exit code.
int runTests(int argc, char** argv);

struct Registrar {
    Registrar(const char* name, std::function<void()> run) { registry().push_back({name, std::move(run)}); }
};

}  // namespace usdcc::test

#define USDCC_TEST_CONCAT2(a, b) a##b
#define USDCC_TEST_CONCAT(a, b) USDCC_TEST_CONCAT2(a, b)

#define USDCC_TEST(name)                                                                              \
    static void usdcc_test_##name();                                                                  \
    static const ::usdcc::test::Registrar USDCC_TEST_CONCAT(usdcc_registrar_, name)(#name, usdcc_test_##name); \
    static void usdcc_test_##name()

#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        if (cond) {                                                                        \
            ++::usdcc::test::passes();                                                     \
        } else {                                                                           \
            ++::usdcc::test::failures();                                                   \
            std::printf("  FAIL %s:%d  %s  -- ", __FILE__, __LINE__, #cond);               \
            std::printf(__VA_ARGS__);                                                      \
            std::printf("\n");                                                             \
        }                                                                                  \
    } while (0)
