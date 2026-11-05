#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::scripting {

// A setting (docs/PYTHON_PACKAGES.md): whether the user's own Python packages,
// %APPDATA%\Python\PythonXY\site-packages, are on sys.path.
inline constexpr auto kPythonUserSiteKey = "Python/userSite";

// Where the embedded interpreter finds Python and the usdcc/pxr packages.
struct PythonSettings {
    // The Python's prefix (holds Lib/ and python3XY.dll): the one OpenUSD was
    // built with (docs/SCRIPTING.md §3.1).
    std::filesystem::path home;
    // Put at the front of sys.path, in this order.
    std::vector<std::filesystem::path> paths;
    // Off, a plugin can't run here on a package it never asked for and fail
    // elsewhere. Can't change once Python has started.
    bool userSite = false;

    // A deployed usdcc's own Python (../python from the executable, with
    // usdcc, pxr and PySide6 in its Lib/site-packages; tools/deploy.ps1) if
    // there is one. Otherwise the build's: OpenUSD's Python, then
    // build/python (the usdcc package), OpenUSD's site-packages (pxr) and, if
    // it was built, USDCC_PYSIDE_DIR (PySide6). The environment variable
    // USDCC_PYTHONHOME overrides `home`. `userSite` is the setting's value, if
    // it has been defined by now.
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
    // Where a run's top-level names live: a fresh namespace each time, or one
    // kept for the session, so a later run sees what an earlier one defined
    // (the Script Editor panel's "Keep Variables Between Runs").
    enum class Namespace { Fresh, Session };

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
    bool runString(const std::string& code, const std::string& fileName = "<string>",
                   Namespace scope = Namespace::Fresh);
    // runString() with the file's contents; __file__ is set.
    bool runFile(const std::filesystem::path& file, Namespace scope = Namespace::Fresh);
    // Forgets the session namespace's names.
    void resetSession();
    // Appends `folder` to sys.path, if it isn't there yet, so scripts can
    // import modules from it. Goes after the usdcc and pxr packages, so a
    // module there can't shadow them.
    void addToPath(const std::filesystem::path& folder);
    // str() of a Python expression's value, or nothing if it raised (logged).
    std::optional<std::string> evaluate(const std::string& expression);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usdcc::scripting
