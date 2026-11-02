#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QCoreApplication>

namespace {
usdcc::test::PythonTestApp* s_app = nullptr;
}

usdcc::test::PythonTestApp& usdcc::test::pythonTestApp() { return *s_app; }

int main(int argc, char** argv) {
    // UndoManager's QUndoStacks expect a Qt application object; nothing here
    // needs a GUI.
    QCoreApplication qtApp(argc, argv);
    usdcc::core::setThreadName("main");

    // Declared in the same order as in the app's main(): the interpreter
    // after the stage manager, the context after both.
    usdcc::usd::StageManager stages;
    usdcc::usd::UndoManager undo;
    usdcc::tools::ToolSet tools;
    // One interpreter for the whole run: Python can't be restarted within a
    // process. Tests reach it through PythonRuntime::instance() too.
    usdcc::scripting::PythonRuntime python;
    const usdcc::scripting::AppContextScope context({&stages, &undo, &tools});
    usdcc::scripting::ScriptRunner runner(python, stages, undo);

    usdcc::test::PythonTestApp app{python, stages, undo, tools, runner};
    s_app = &app;
    const int result = usdcc::test::runTests(argc, argv);
    s_app = nullptr;
    return result;
}
