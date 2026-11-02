#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/python_runtime.h"

int main(int argc, char** argv) {
    usdcc::core::setThreadName("main");
    // One interpreter for the whole run: Python can't be restarted within a
    // process. Tests reach it through PythonRuntime::instance().
    usdcc::scripting::PythonRuntime python;
    return usdcc::test::runTests(argc, argv);
}
