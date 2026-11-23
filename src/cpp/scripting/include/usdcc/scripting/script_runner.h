#pragma once

#include "usdcc/scripting/python_runtime.h"

#include <cstdint>
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
    struct Options {
        // Names it in tracebacks, and the undo step: a file's path gives the step
        // only its file name.
        std::string name = "<script>";
        // The line of the file that the code starts on.
        int firstLine = 1;
        PythonRuntime::Namespace scope = PythonRuntime::Namespace::Fresh;
    };
    struct Result {
        bool ok = false;
        ScriptError error;  // when it isn't
        int undoSteps = 0;
        std::int64_t ms = 0;
    };

    ScriptRunner(PythonRuntime& runtime, usdcc::usd::StageManager& stages, usdcc::usd::UndoManager& undo);

    Result execute(const std::string& code, const Options& options);
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
