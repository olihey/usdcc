// pybind11 (and with it Python.h) first: Python.h must precede the standard headers.
#if USDCC_HAS_PYTHON
#include <pybind11/embed.h>
#endif

#include "usdcc/scripting/debugger.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/python_runtime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <atomic>

namespace usdcc::scripting::debugger {

namespace {
std::atomic<int> s_port = 0;
}

std::filesystem::path folder() {
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../tools/debugpy"));
    for (const QString& candidate : {QDir::cleanPath(bundled),
#ifdef USDCC_DEBUGPY_DIR
                                     QStringLiteral(USDCC_DEBUGPY_DIR)
#endif
         }) {
        if (QFileInfo::exists(candidate + QStringLiteral("/debugpy/__init__.py"))) return std::filesystem::path(candidate.toStdU16String());
    }
    return {};
}

int port() { return s_port; }

void setPort(int port) { s_port = port; }

int listen(PythonRuntime& python, std::string* error) {
    if (s_port) return s_port;
    if (!python.isAvailable()) {
        if (error) *error = python.unavailableReason();
        return 0;
    }
#if USDCC_HAS_PYTHON
    namespace py = pybind11;
    py::gil_scoped_acquire gil;
    try {
        const int port = py::module_::import("usdcc.debug").attr("listen")().cast<int>();
        USDCC_LOG_INFO("usdcc.debug", "A debugger can attach on port {}", port);
        return port;
    } catch (py::error_already_set& raised) {
        std::string why;
        try {
            why = py::str(raised.value());
        } catch (py::error_already_set&) {
            why = raised.what();
        }
        USDCC_LOG_WARNING("usdcc.debug", "A debugger can't attach: {}", why);
        if (error) *error = why;
    }
#endif
    return 0;
}

}  // namespace usdcc::scripting::debugger
