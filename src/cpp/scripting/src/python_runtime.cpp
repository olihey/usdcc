// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#if USDCC_HAS_PYTHON
#include <pybind11/embed.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "usdcc/scripting/python_runtime.h"

#include "usdcc/core/install_paths.h"
#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace usdcc::scripting {

namespace {
PythonRuntime* s_instance = nullptr;
// The run on the main thread that can be stopped, the outermost one; 0 for none. Each has
// its own number, so a request to stop one that has ended does nothing.
std::atomic<std::intptr_t> s_stoppableRun = 0;

#if USDCC_HAS_PYTHON
// Called by Python itself, on the main thread, between two instructions.
int raiseIfRunning(void* run) {
    if (s_stoppableRun != reinterpret_cast<std::intptr_t>(run)) return 0;
    PyErr_SetString(PyExc_KeyboardInterrupt, "stopped");
    return -1;
}
#endif

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

#if USDCC_HAS_PYTHON
namespace py = pybind11;
using usdcc::core::LogLevel;

std::optional<std::filesystem::path> environmentPath(const char* name) {
#if defined(_WIN32)
    std::size_t length = 0;
    getenv_s(&length, nullptr, 0, name);
    if (length <= 1) return std::nullopt;
    std::string value(length, '\0');
    getenv_s(&length, value.data(), value.size(), name);
    value.resize(length - 1);
    return std::filesystem::u8path(value);
#else
    const char* value = std::getenv(name);
    if (!value || !*value) return std::nullopt;
    return std::filesystem::u8path(value);
#endif
}

// A standard library under `home`, so that initializing doesn't hit
// Python's fatal "Failed to import encodings" (which aborts the process).
bool hasStandardLibrary(const std::filesystem::path& home) {
    std::error_code error;
    if (std::filesystem::exists(home / "Lib" / "encodings" / "__init__.py", error)) return true;  // Windows layout
    for (const auto& entry : std::filesystem::directory_iterator(home / "lib", error)) {           // POSIX layout
        if (std::filesystem::exists(entry.path() / "encodings" / "__init__.py", error)) return true;
    }
    return false;
}

#if defined(_WIN32)
// The folder of a loaded module; of the running executable for null.
std::filesystem::path moduleDir(HMODULE module) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);  // truncated: try a bigger buffer
    }
    return std::filesystem::path(buffer).parent_path();
}

// pxr.UsdAbc and pxr.UsdDraco link their plugins, usdAbc.dll and usdDraco.dll, and
// Windows doesn't look for a module's DLLs in plugin/usd, beside OpenUSD's DLLs' folder.
// On Linux the modules' RUNPATH names it (tools/deploy.py).
void addUsdPluginsDllDirectory() {
    const HMODULE plug = GetModuleHandleW(L"usd_plug.dll");
    if (!plug) return;
    std::filesystem::path folder = moduleDir(plug).parent_path() / "plugin" / "usd";
    folder.make_preferred();  // AddDllDirectory takes no forward slashes
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return;
    if (AddDllDirectory(folder.c_str())) return;
    const DWORD code = GetLastError();
    USDCC_LOG_WARNING("usdcc.python", "{} can't be added to the DLL search path (error {}), so pxr.UsdAbc and pxr.UsdDraco may not import",
                      toUtf8(folder), code);
}
#endif

// The folder of the running executable.
std::filesystem::path executableDir() {
#if defined(_WIN32)
    return moduleDir(nullptr);
#elif defined(__APPLE__)
    // macOS has no /proc.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // fails, and says how long the path is
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code error;
    const std::filesystem::path path = std::filesystem::canonical(buffer.c_str(), error);
    return error ? std::filesystem::path() : path.parent_path();
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
}
#endif
}  // namespace

PythonSettings PythonSettings::defaults() {
    PythonSettings settings;
    settings.userSite = usdcc::core::Settings::instance().get<bool>(kPythonUserSiteKey, false);
#if USDCC_HAS_PYTHON
    // A deployed usdcc (tools/deploy.py) brings its own Python. On Windows,
    // bin/usdcc.exe next to python/, whose Lib/site-packages holds usdcc, pxr and
    // PySide6; elsewhere the deploy is the Python's prefix itself: bin/usdcc next
    // to lib/python3.XY/site-packages (in a macOS bundle, Contents/Resources/lib).
#if defined(_WIN32)
    const std::filesystem::path bundled = executableDir().parent_path() / "python";
    const std::filesystem::path bundledSite = bundled / "Lib" / "site-packages";
#else
    const std::filesystem::path bundled = usdcc::core::installRoot(executableDir());
    std::filesystem::path bundledSite;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(bundled / "lib", error)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("python3", 0) == 0 && std::filesystem::exists(entry.path() / "encodings" / "__init__.py", error)) {
            bundledSite = entry.path() / "site-packages";
            break;
        }
    }
#endif
    if (!bundledSite.empty() && hasStandardLibrary(bundled) && std::filesystem::is_directory(bundledSite)) {
        settings.home = bundled;
        settings.bundled = true;
        settings.paths = {bundledSite};
    } else {
        settings.home = std::filesystem::u8path(USDCC_PYTHON_HOME);
        settings.paths = {std::filesystem::u8path(USDCC_PYTHON_PACKAGE_ROOT),
                          std::filesystem::u8path(USDCC_USD_PYTHON_DIR)};
#ifdef USDCC_PYSIDE_DIR
        settings.paths.push_back(std::filesystem::u8path(USDCC_PYSIDE_DIR));  // PySide6, shiboken6
#endif
    }
    if (auto home = environmentPath("USDCC_PYTHONHOME")) settings.home = *home;
#endif
    return settings;
}

struct PythonRuntime::Impl {
    PythonSettings settings;
    bool available = false;
    bool pxr = false;
    std::string reason;
    std::string version;
    std::atomic<int> running = 0;
    std::thread::id mainThread = std::this_thread::get_id();
    int stoppableDepth = 0;  // main thread only
    std::intptr_t lastRun = 0;
#if USDCC_HAS_PYTHON
    PyThreadState* mainThreadState = nullptr;  // while the main thread doesn't hold the GIL
    std::optional<py::dict> session;            // Namespace::Session; only touched with the GIL
    bool initialized = false;

    void start();
    void logException(py::error_already_set& error);
    void flushStreams();
    bool run(const std::string& code, const std::string& fileName, const std::filesystem::path* file,
             Namespace scope, int firstLine = 1, ScriptError* error = nullptr);
    static void describe(py::error_already_set& raised, ScriptError& error);
    static py::dict freshNamespace();
#endif
};

#if USDCC_HAS_PYTHON

// The C++ end of usdcc.log (src/python/usdcc/log.py): Python's records into
// the LogHub. Compiled into this library, so it exists only inside the app
// (and the tests); a plain `python` has no _usdcc_log and usdcc.log does
// nothing there.
PYBIND11_EMBEDDED_MODULE(_usdcc_log, m) {
    m.doc() = "Posts records to the usdcc LogHub (docs/LOGGING.md). Use usdcc.log, not this.";
    m.def(
        "post",
        [](int level, const std::string& source, const std::string& category, const std::string& message,
           const std::string& details, const std::string& file, int line, const std::string& function,
           const std::string& threadName, const std::string& toast, const std::string& toastTitle,
           const std::string& toastKind) {
            usdcc::core::LogRecord record;
            record.level = static_cast<LogLevel>(std::clamp(level, 0, static_cast<int>(LogLevel::Fatal)));
            record.source = source;
            record.category = category;
            record.message = message;
            record.details = details;
            record.file = file;
            record.line = line;
            record.function = function;
            record.threadName = threadName;
            // toast: "" (none), "timed" or "sticky" (docs/NOTIFICATIONS.md).
            if (toast == "timed" || toast == "sticky") {
                usdcc::core::ToastRequest request;
                request.title = toastTitle;
                if (toast == "sticky") request.durationMs = usdcc::core::ToastRequest::kSticky;
                usdcc::core::ToastKind kind{};
                if (!toastKind.empty()) {
                    if (!usdcc::core::parseToastKind(toastKind, &kind)) {
                        throw py::value_error("unknown toast kind '" + toastKind +
                                              "' (info, success, warning or error)");
                    }
                    request.kind = kind;
                }
                record.toast = std::move(request);
            } else if (!toast.empty()) {
                throw py::value_error("toast must be '', 'timed' or 'sticky'");
            }
            usdcc::core::LogHub::instance().post(std::move(record));
        },
        py::arg("level"), py::arg("source"), py::arg("category"), py::arg("message"), py::arg("details") = "",
        py::arg("file") = "", py::arg("line") = 0, py::arg("function") = "", py::arg("thread_name") = "",
        py::arg("toast") = "", py::arg("toast_title") = "", py::arg("toast_kind") = "");
    m.def(
        "is_enabled",
        [](int level, const std::string& source, const std::string& category) {
            return usdcc::core::LogHub::instance().isEnabled(
                static_cast<LogLevel>(std::clamp(level, 0, static_cast<int>(LogLevel::Fatal))), source, category);
        },
        py::arg("level"), py::arg("source"), py::arg("category"));
    m.def(
        "configure_levels", [](const std::string& spec) { return usdcc::core::LogHub::instance().configureLevels(spec); },
        py::arg("spec"), "Same format as USDCC_LOG; False if a part couldn't be parsed.");
}

void PythonRuntime::Impl::start() {
    if (Py_IsInitialized()) {
        reason = "Python is already running in this process";
        return;
    }
    if (!hasStandardLibrary(settings.home)) {
        reason = "No Python standard library in " + toUtf8(settings.home) +
                 " (the Python OpenUSD was built with; USDCC_PYTHONHOME overrides it)";
        return;
    }

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.install_signal_handlers = 0;  // Ctrl+C and friends stay the app's
    config.parse_argv = 0;
    config.user_site_directory = settings.userSite ? 1 : 0;
#if defined(__APPLE__)
    // Nothing is written into a signed bundle: a __pycache__ there would break its
    // signature. Its own modules are compiled already (tools/deploy.py).
    if (settings.home == usdcc::core::installRoot(executableDir()) && usdcc::core::isMacBundleDir(executableDir())) {
        config.write_bytecode = 0;
    }
#endif
    PyConfig_SetString(&config, &config.program_name, L"usdcc");
    PyConfig_SetString(&config, &config.home, settings.home.wstring().c_str());
    const PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        reason = std::string("Python failed to start: ") + (status.err_msg ? status.err_msg : "unknown error");
        return;
    }
    initialized = true;
    available = true;

    {
        py::module_ sys = py::module_::import("sys");
        py::list path = sys.attr("path");
        for (std::size_t i = 0; i < settings.paths.size(); ++i) {
            path.insert(static_cast<py::ssize_t>(i), toUtf8(settings.paths[i]));
        }
        const std::string fullVersion = py::str(sys.attr("version"));
        version = fullVersion.substr(0, fullVersion.find(' '));

        // Before anything else runs, so nothing it prints or raises is lost.
        try {
            py::module_::import("usdcc.log").attr("install")();
        } catch (py::error_already_set& error) {
            USDCC_LOG_ERROR("usdcc.python", "Python's output can't be routed to the log: {}", error.what());
        }
        try {
            py::module_::import("pxr.Usd");
            pxr = true;
#if defined(_WIN32)
            addUsdPluginsDllDirectory();  // now that OpenUSD's DLLs are loaded
#endif
        } catch (py::error_already_set& error) {
            logException(error);
            USDCC_LOG_ERROR("usdcc.python", "OpenUSD's Python modules (pxr) can't be imported from {}",
                            toUtf8(settings.paths.size() > 1 ? settings.paths[1] : settings.home));
        }
    }
    USDCC_LOG_INFO("usdcc.python", "Python {} from {}{}", version, toUtf8(settings.home), pxr ? ", with pxr" : "");

    // From here on the main thread holds the GIL only while it runs Python.
    mainThreadState = PyEval_SaveThread();
}

void PythonRuntime::Impl::logException(py::error_already_set& error) {
    try {
        // An exception raised before any code ran — a SyntaxError from
        // compile() — has no traceback, and pybind11 refuses to pass a null
        // handle (it throws py::cast_error, which would end the app).
        const auto orNone = [](const py::object& object) -> py::object { return object ? object : py::none(); };
        py::module_::import("usdcc.log")
            .attr("log_exception")(orNone(error.type()), orNone(error.value()), orNone(error.trace()));
    } catch (py::error_already_set& nested) {
        // usdcc.log itself is broken: at least say what happened.
        USDCC_LOG_ERROR("python.exception", "{}", error.what());
        USDCC_LOG_ERROR("usdcc.python", "usdcc.log.log_exception failed too: {}", nested.what());
    }
}

void PythonRuntime::Impl::describe(py::error_already_set& raised, ScriptError& error) {
    try {
        error.type = py::str(raised.type().attr("__name__"));
        error.message = py::str(raised.value());
        if (raised.trace()) {
            const py::object frames = py::module_::import("traceback").attr("extract_tb")(raised.trace());
            for (const py::handle frame : frames) {
                const py::object line = frame.attr("lineno");
                error.frames.push_back({py::str(frame.attr("filename")), line.is_none() ? 0 : line.cast<int>(),
                                        py::str(frame.attr("name"))});
            }
        }
        // Raised by compile(), so there's no frame in the file itself.
        if (raised.matches(PyExc_SyntaxError)) {
            const py::object file = raised.value().attr("filename");
            const py::object line = raised.value().attr("lineno");
            if (!file.is_none() && !line.is_none()) error.frames.push_back({py::str(file), line.cast<int>(), "<module>"});
        }
    } catch (py::error_already_set&) {
        // An exception whose own __str__ raises: what was read so far will do.
    }
}

void PythonRuntime::Impl::flushStreams() {
    try {
        py::module_ sys = py::module_::import("sys");
        sys.attr("stdout").attr("flush")();
        sys.attr("stderr").attr("flush")();
    } catch (py::error_already_set&) {
        // A script replaced the streams with something that can't flush.
    }
}

py::dict PythonRuntime::Impl::freshNamespace() {
    py::dict globals;
    globals["__name__"] = "__main__";
    globals["__builtins__"] = py::module_::import("builtins");
    return globals;
}

bool PythonRuntime::Impl::run(const std::string& code, const std::string& fileName,
                              const std::filesystem::path* file, Namespace scope, int firstLine, ScriptError* error) {
    if (!available) return false;
    py::gil_scoped_acquire gil;
    bool ok = true;
    try {
        py::module_ builtins = py::module_::import("builtins");
        py::dict globals;
        if (scope == Namespace::Session) {
            if (!session) session = freshNamespace();
            globals = *session;
        } else {
            globals = freshNamespace();
        }
        if (file) {
            globals["__file__"] = toUtf8(*file);
        } else if (globals.contains("__file__")) {
            PyDict_DelItemString(globals.ptr(), "__file__");
        }
        // Empty lines before it, so its first line has the number asked for.
        py::object compiled = firstLine > 1 ? builtins.attr("compile")(std::string(std::size_t(firstLine - 1), '\n') + code, fileName, "exec")
                                            : builtins.attr("compile")(code, fileName, "exec");
        builtins.attr("exec")(compiled, globals);
    } catch (py::error_already_set& raised) {
        if (!raised.matches(PyExc_SystemExit)) {
            if (error) describe(raised, *error);
            logException(raised);
            ok = false;
        }
    }
    flushStreams();
    return ok;
}

#endif  // USDCC_HAS_PYTHON

PythonRuntime::PythonRuntime(const PythonSettings& settings) : m_impl(std::make_unique<Impl>()) {
    m_impl->settings = settings;
#if USDCC_HAS_PYTHON
    if (s_instance) {
        m_impl->reason = "Python is already running in this process";
    } else {
        m_impl->start();
    }
#else
    m_impl->reason = "usdcc was built without Python (OpenUSD's Python wasn't found)";
#endif
    if (!m_impl->available) {
        USDCC_LOG_ERROR("usdcc.python", "Scripting is unavailable: {}", m_impl->reason);
    }
    if (!s_instance && m_impl->available) s_instance = this;
}

PythonRuntime::~PythonRuntime() {
#if USDCC_HAS_PYTHON
    if (m_impl->initialized) {
        PyEval_RestoreThread(m_impl->mainThreadState);
        m_impl->session.reset();
        m_impl->flushStreams();
        if (Py_FinalizeEx() != 0) {
            USDCC_LOG_WARNING("usdcc.python", "Python reported errors while shutting down");
        }
    }
#endif
    if (s_instance == this) s_instance = nullptr;
}

PythonRuntime* PythonRuntime::instance() { return s_instance; }

bool PythonRuntime::isAvailable() const { return m_impl->available; }

const std::string& PythonRuntime::unavailableReason() const { return m_impl->reason; }

bool PythonRuntime::hasPxr() const { return m_impl->pxr; }

const std::string& PythonRuntime::version() const { return m_impl->version; }

const PythonSettings& PythonRuntime::settings() const { return m_impl->settings; }

std::filesystem::path PythonRuntime::executable() const {
    const std::filesystem::path& home = m_impl->settings.home;
#if defined(_WIN32)
    return home / "python.exe";
#else
    // "3.12.3" -> bin/python3.12, which a POSIX install always has; else bin/python3.
    const std::string& version = m_impl->version;
    const std::size_t major = version.find('.');
    const std::size_t minor = major == std::string::npos ? major : version.find('.', major + 1);
    std::error_code error;
    if (minor != std::string::npos) {
        const std::filesystem::path versioned = home / "bin" / ("python" + version.substr(0, minor));
        if (std::filesystem::exists(versioned, error)) return versioned;
    }
    return home / "bin" / "python3";
#endif
}

bool PythonRuntime::runString(const std::string& code, const std::string& fileName, Namespace scope, int firstLine,
                              ScriptError* error) {
#if USDCC_HAS_PYTHON
    const Stoppable stoppable(*this);
    return m_impl->run(code, fileName, nullptr, scope, firstLine, error);
#else
    (void)code;
    (void)fileName;
    (void)scope;
    (void)firstLine;
    (void)error;
    return false;
#endif
}

bool PythonRuntime::isRunning() const { return m_impl->running > 0; }

PythonRuntime::Stoppable::Stoppable(PythonRuntime& runtime) : m_runtime(runtime) {
    Impl& impl = *m_runtime.m_impl;
    ++impl.running;
    if (std::this_thread::get_id() == impl.mainThread && ++impl.stoppableDepth == 1) s_stoppableRun = ++impl.lastRun;
}

PythonRuntime::Stoppable::~Stoppable() {
    Impl& impl = *m_runtime.m_impl;
    if (std::this_thread::get_id() == impl.mainThread && --impl.stoppableDepth == 0) s_stoppableRun = 0;
    --impl.running;
}

bool PythonRuntime::stop() {
#if USDCC_HAS_PYTHON
    const std::intptr_t run = s_stoppableRun;
    return run != 0 && Py_AddPendingCall(&raiseIfRunning, reinterpret_cast<void*>(run)) == 0;
#else
    return false;
#endif
}

void PythonRuntime::resetSession() {
#if USDCC_HAS_PYTHON
    if (!m_impl->available) return;
    py::gil_scoped_acquire gil;
    m_impl->session.reset();
#endif
}

void PythonRuntime::addToPath(const std::filesystem::path& folder) {
#if USDCC_HAS_PYTHON
    if (!m_impl->available) return;
    py::gil_scoped_acquire gil;
    py::list path = py::module_::import("sys").attr("path");
    const py::str entry(toUtf8(folder));
    if (!path.contains(entry)) path.append(entry);
#else
    (void)folder;
#endif
}

bool PythonRuntime::runFile(const std::filesystem::path& file, Namespace scope) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        USDCC_LOG_ERROR("usdcc.python", "Can't read {}", toUtf8(file));
        return false;
    }
    std::ostringstream code;
    code << in.rdbuf();
    // A UTF-8 byte order mark, which Windows editors like to write: python skips it in
    // a file, but compiling the text with it in is a SyntaxError (U+FEFF).
    std::string text = code.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
#if USDCC_HAS_PYTHON
    const auto start = std::chrono::steady_clock::now();
    const Stoppable stoppable(*this);
    const bool ok = m_impl->run(text, toUtf8(file), &file, scope);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (ok) {
        USDCC_LOG_INFO("usdcc.python", "Ran {} in {} ms", toUtf8(file.filename()), ms);
    } else {
        USDCC_LOG_WARNING("usdcc.python", "{} stopped with an error after {} ms", toUtf8(file.filename()), ms);
    }
    return ok;
#else
    return false;
#endif
}

std::optional<std::string> PythonRuntime::evaluate(const std::string& expression) {
#if USDCC_HAS_PYTHON
    if (!m_impl->available) return std::nullopt;
    py::gil_scoped_acquire gil;
    try {
        py::dict globals;
        globals["__builtins__"] = py::module_::import("builtins");
        return std::string(py::str(py::eval(expression, globals)));
    } catch (py::error_already_set& error) {
        m_impl->logException(error);
        return std::nullopt;
    }
#else
    (void)expression;
    return std::nullopt;
#endif
}

}  // namespace usdcc::scripting
