#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include "usdcc/ui/scale_controller.h"

#include <QApplication>
#include <QTemporaryDir>

namespace {
usdcc::test::PythonTestApp* s_app = nullptr;
}

usdcc::test::PythonTestApp& usdcc::test::pythonTestApp() { return *s_app; }

int main(int argc, char** argv) {
    // Widgets for the Script Editor panel tests; CTest runs this on Qt's
    // offscreen platform, so no window appears. The scale controller's
    // settings live in a temporary folder, never in the user's usdcc.ini.
    QApplication qtApp(argc, argv);
    QTemporaryDir settingsDir;
    usdcc::ui::UiScaleController scale(qtApp, settingsDir.filePath(QStringLiteral("ui.ini")));
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
