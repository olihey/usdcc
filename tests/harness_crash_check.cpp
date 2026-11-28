// A test program that crashes on purpose, so CTest can check that the harness reports
// a crash: the test it was in and where (test_harness.cpp). Its CTest passes on that
// report, not on the exit code.

#include "test_harness.h"

// Not inlined, so the report has a frame of its own to name, and not static: on Linux
// backtrace_symbols() names only what the program exports (-rdynamic).
int usdccHarnessReadThrough(volatile int* pointer);
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
int usdccHarnessReadThrough(volatile int* pointer) {
    return *pointer;
}

USDCC_TEST(harness_reports_a_crash) {
    volatile int* nowhere = nullptr;
    CHECK(usdccHarnessReadThrough(nowhere) == 0, "never reached");
}

int main(int argc, char** argv) { return usdcc::test::runTests(argc, argv); }
