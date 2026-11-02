// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#if USDCC_HAS_PYTHON
#include <pybind11/embed.h>
#endif

#include "usdcc/scripting/python_runtime.h"

#include "usdcc/core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace usdcc::scripting {

namespace {
PythonRuntime* s_instance = nullptr;

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
#endif
}  // namespace

PythonSettings PythonSettings::defaults() {
    PythonSettings settings;
#if USDCC_HAS_PYTHON
    settings.home = std::filesystem::u8path(USDCC_PYTHON_HOME);
    if (auto home = environmentPath("USDCC_PYTHONHOME")) settings.home = *home;
    settings.paths = {std::filesystem::u8path(USDCC_PYTHON_PACKAGE_ROOT), std::filesystem::u8path(USDCC_USD_PYTHON_DIR)};
#ifdef USDCC_PYSIDE_DIR
    settings.paths.push_back(std::filesystem::u8path(USDCC_PYSIDE_DIR));  // PySide6, shiboken6
#endif
#endif
    return settings;
}

struct PythonRuntime::Impl {
    PythonSettings settings;
    bool available = false;
    bool pxr = false;
    std::string reason;
    std::string version;
#if USDCC_HAS_PYTHON
    PyThreadState* mainThreadState = nullptr;  // while the main thread doesn't hold the GIL
    std::optional<py::dict> session;            // Namespace::Session; only touched with the GIL
    bool initialized = false;

    void start();
    void logException(py::error_already_set& error);
    void flushStreams();
    bool run(const std::string& code, const std::string& fileName, const std::filesystem::path* file,
             Namespace scope);
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
           const std::string& threadName) {
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
            usdcc::core::LogHub::instance().post(std::move(record));
        },
        py::arg("level"), py::arg("source"), py::arg("category"), py::arg("message"), py::arg("details") = "",
        py::arg("file") = "", py::arg("line") = 0, py::arg("function") = "", py::arg("thread_name") = "");
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
        py::module_::import("usdcc.log").attr("log_exception")(error.type(), error.value(), error.trace());
    } catch (py::error_already_set& nested) {
        // usdcc.log itself is broken: at least say what happened.
        USDCC_LOG_ERROR("python.exception", "{}", error.what());
        USDCC_LOG_ERROR("usdcc.python", "usdcc.log.log_exception failed too: {}", nested.what());
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
                              const std::filesystem::path* file, Namespace scope) {
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
        py::object compiled = builtins.attr("compile")(code, fileName, "exec");
        builtins.attr("exec")(compiled, globals);
    } catch (py::error_already_set& error) {
        if (!error.matches(PyExc_SystemExit)) {
            logException(error);
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

bool PythonRuntime::runString(const std::string& code, const std::string& fileName, Namespace scope) {
#if USDCC_HAS_PYTHON
    return m_impl->run(code, fileName, nullptr, scope);
#else
    (void)code;
    (void)fileName;
    (void)scope;
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
#if USDCC_HAS_PYTHON
    const auto start = std::chrono::steady_clock::now();
    const bool ok = m_impl->run(code.str(), toUtf8(file), &file, scope);
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
