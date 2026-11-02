#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::scripting {

// Where the embedded interpreter finds Python and the usdcc/pxr packages.
struct PythonSettings {
    // The Python's prefix (holds Lib/ and python3XY.dll): the one OpenUSD was
    // built with (docs/SCRIPTING.md §3.1).
    std::filesystem::path home;
    // Put at the front of sys.path, in this order.
    std::vector<std::filesystem::path> paths;

    // The build's defaults: OpenUSD's Python, then build/python (the usdcc
    // package) and OpenUSD's site-packages (pxr). The environment variable
    // USDCC_PYTHONHOME overrides `home`.
    static PythonSettings defaults();
};

// The application's embedded Python interpreter (docs/SCRIPTING.md §3).
//
// Starting it initializes Python with `settings`, routes Python's output into
// the usdcc log (usdcc.log.install(): print, logging, warnings, uncaught
// exceptions; docs/LOGGING.md §3.4), checks that OpenUSD's pxr modules import,
// and then releases the GIL: the main thread holds it only while it runs
// Python, so OpenUSD's own threads can take it (TfPyLock). Every call below
// takes the GIL itself and works from any thread.
//
// If Python can't start (no standard library at `home`, ...) the runtime is
// unavailable, says why, and the app carries on without scripting. Python
// can only be started once per process, so there is one runtime at most;
// destroying it finalizes the interpreter.
class PythonRuntime {
public:
    explicit PythonRuntime(const PythonSettings& settings = PythonSettings::defaults());
    ~PythonRuntime();
    PythonRuntime(const PythonRuntime&) = delete;
    PythonRuntime& operator=(const PythonRuntime&) = delete;

    // The live runtime, or null.
    static PythonRuntime* instance();

    bool isAvailable() const;
    // Why it isn't available; empty when it is.
    const std::string& unavailableReason() const;
    // Whether OpenUSD's pxr package imported.
    bool hasPxr() const;
    // sys.version's first word, e.g. "3.12.10".
    const std::string& version() const;
    const PythonSettings& settings() const;

    // Runs `code` as a script in a fresh namespace (__name__ == "__main__";
    // `fileName` names it in tracebacks). Returns false if it raised: the
    // exception and its traceback are logged (category python.exception).
    // SystemExit just ends the script and counts as success.
    bool runString(const std::string& code, const std::string& fileName = "<string>");
    // runString() with the file's contents; __file__ is set.
    bool runFile(const std::filesystem::path& file);
    // str() of a Python expression's value, or nothing if it raised (logged).
    std::optional<std::string> evaluate(const std::string& expression);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usdcc::scripting
