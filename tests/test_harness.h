#pragma once

// A deliberately tiny test harness (no extra dependency): USDCC_TEST(name)
// registers a test function, CHECK(cond, fmt, ...) records a pass or prints a
// failure with a printf-style explanation, and test_main.cpp runs everything
// (or only the tests whose names contain argv[1]) and returns non-zero on any
// failure, which is what CTest looks at.

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
