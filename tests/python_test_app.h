#pragma once

// What usdcc_python_tests' main() sets up once for every test: the embedded
// interpreter and, behind usdcc.usd / usdcc.app / usdcc.tools, an app context
// like the real app's (a StageManager, an UndoManager and a ToolSet) with a
// ScriptRunner for it.

namespace usdcc::scripting {
class PythonRuntime;
class ScriptRunner;
}  // namespace usdcc::scripting
namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd
namespace usdcc::tools {
class ToolSet;
}

namespace usdcc::test {

struct PythonTestApp {
    usdcc::scripting::PythonRuntime& python;
    usdcc::usd::StageManager& stages;
    usdcc::usd::UndoManager& undo;
    usdcc::tools::ToolSet& tools;
    usdcc::scripting::ScriptRunner& runner;
};

PythonTestApp& pythonTestApp();

}  // namespace usdcc::test
