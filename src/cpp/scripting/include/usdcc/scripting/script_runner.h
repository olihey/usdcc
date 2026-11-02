#pragma once

#include "usdcc/scripting/python_runtime.h"

#include <filesystem>
#include <string>

namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd

namespace usdcc::scripting {

// Runs a script the way the user expects from a DCC (docs/SCRIPTING.md
// §4.2–4.3): everything it does becomes one undo step on each stage it
// changes, including stages it opens itself ("Run <name>"). If it raises, the
// traceback is logged and its changes so far stay, as one step named
// "Run <name> (failed)", so a single Undo reverts them. Main thread only.
class ScriptRunner {
public:
    ScriptRunner(PythonRuntime& runtime, usdcc::usd::StageManager& stages, usdcc::usd::UndoManager& undo);

    // Returns true if the script ran without an exception. `name` is used in
    // tracebacks and the undo step's description.
    bool run(const std::string& code, const std::string& name,
             PythonRuntime::Namespace scope = PythonRuntime::Namespace::Fresh);
    bool runFile(const std::filesystem::path& file, PythonRuntime::Namespace scope = PythonRuntime::Namespace::Fresh);

    // How many undo steps the last run recorded.
    int lastUndoSteps() const { return m_lastUndoSteps; }

private:
    template <typename Run>
    bool runAsOneStep(const std::string& name, Run&& run);

    PythonRuntime& m_runtime;
    usdcc::usd::StageManager& m_stages;
    usdcc::usd::UndoManager& m_undo;
    int m_lastUndoSteps = 0;
};

}  // namespace usdcc::scripting
