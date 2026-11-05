// PythonRuntime and the Python log bridge (docs/SCRIPTING.md phase 1, which
// is also docs/LOGGING.md phase 5): the embedded Python is the one OpenUSD
// was built with, pxr imports, Python's output reaches the log with the right
// source, category and level, exceptions are logged with their traceback,
// and the GIL is free for other threads between runs.
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/python_runtime.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;
using usdcc::core::LogSink;
using usdcc::scripting::PythonRuntime;

namespace {
struct Collect : LogSink {
    std::mutex mutex;
    std::vector<LogRecord> records;
    void onRecord(const LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    std::vector<LogRecord> where(const std::string& category) {
        std::lock_guard lock(mutex);
        std::vector<LogRecord> result;
        for (const LogRecord& record : records) {
            if (record.category == category) result.push_back(record);
        }
        return result;
    }
};

struct ScopedCollect {
    std::shared_ptr<Collect> sink = std::make_shared<Collect>();
    ScopedCollect() { LogHub::instance().addSink(sink, false); }
    ~ScopedCollect() { LogHub::instance().removeSink(sink.get()); }
};

std::string normalized(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.back() == '/') path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
    return path;
}

PythonRuntime& python() { return *PythonRuntime::instance(); }
}  // namespace

USDCC_TEST(python_runtime_starts) {
    CHECK(PythonRuntime::instance() != nullptr, "the runtime started");
    if (!PythonRuntime::instance()) return;
    CHECK(python().isAvailable(), "available: %s", python().unavailableReason().c_str());
    CHECK(python().hasPxr(), "pxr imports");
    CHECK(!python().version().empty() && python().version().rfind("3.", 0) == 0, "version %s",
          python().version().c_str());

    const auto prefix = python().evaluate("__import__('sys').base_prefix");
    const std::string home = python().settings().home.generic_string();
    CHECK(prefix && normalized(*prefix) == normalized(home), "the Python OpenUSD was built with: %s vs %s",
          prefix ? prefix->c_str() : "?", home.c_str());

    const auto usdVersion = python().evaluate("__import__('pxr.Usd').Usd.GetVersion()");
    CHECK(usdVersion && usdVersion->front() == '(', "pxr.Usd works: %s", usdVersion ? usdVersion->c_str() : "?");

    // A second runtime can't start another interpreter.
    PythonRuntime second;
    CHECK(!second.isAvailable() && !second.unavailableReason().empty(), "only one runtime: %s",
          second.unavailableReason().c_str());
    CHECK(PythonRuntime::instance() != &second, "the first stays the instance");
}

USDCC_TEST(python_output_reaches_the_log) {
    ScopedCollect collect;
    const bool ok = python().runString(
        "import logging, sys, warnings\n"
        "print('hello from print')\n"
        "print('two', 'words', end='')\n"          // no newline: flushed at the end of the run
        "logging.getLogger('my.tool').warning('careful %s', 'now')\n"
        "logging.getLogger('my.tool').error('broken', exc_info=ValueError('inner'))\n"
        "warnings.warn('old api', DeprecationWarning)\n"
        "sys.stderr.write('to stderr\\n')\n",
        "<output test>");
    CHECK(ok, "the script ran");

    auto out = collect.sink->where("python.stdout");
    CHECK(out.size() == 2 && out[0].message == "hello from print" && out[1].message == "two words",
          "print lines: %zu", out.size());
    for (const LogRecord& record : out) {
        CHECK(record.source == "python" && record.level == LogLevel::Info, "print: source python, Info");
    }

    auto tool = collect.sink->where("my.tool");
    CHECK(tool.size() == 2, "logger records: %zu", tool.size());
    if (tool.size() == 2) {
        CHECK(tool[0].level == LogLevel::Warning && tool[0].message == "careful now", "logging.warning: %s",
              tool[0].message.c_str());
        CHECK(tool[0].file == "<output test>" && tool[0].line == 4, "with the call site: %s:%d",
              tool[0].file.c_str(), tool[0].line);
        CHECK(tool[1].level == LogLevel::Error, "logging.error is an Error");
    }

    auto warnings = collect.sink->where("py.warnings");
    CHECK(warnings.size() == 1 && warnings[0].level == LogLevel::Warning &&
              warnings[0].message.find("old api") != std::string::npos,
          "warnings.warn: %zu", warnings.size());

    auto err = collect.sink->where("python.stderr");
    CHECK(err.size() == 1 && err[0].message == "to stderr" && err[0].level == LogLevel::Warning, "sys.stderr: %zu",
          err.size());
}

USDCC_TEST(python_exceptions_are_logged) {
    ScopedCollect collect;
    const bool ok = python().runString(
        "def compute():\n"
        "    raise ValueError('bad value')\n"
        "compute()\n",
        "<exception test>");
    CHECK(!ok, "a raising script reports failure");

    auto errors = collect.sink->where("python.exception");
    CHECK(errors.size() == 1, "one Error: %zu", errors.size());
    if (errors.size() == 1) {
        const LogRecord& error = errors[0];
        CHECK(error.level == LogLevel::Error && error.source == "python", "Error from python");
        CHECK(error.message == "ValueError: bad value", "the exception as the message: %s", error.message.c_str());
        CHECK(error.details.find("Traceback") != std::string::npos && error.details.find("line 2") != std::string::npos,
              "the traceback as details: %s", error.details.c_str());
        CHECK(error.function == "compute" && error.line == 2, "the innermost frame: %s:%d", error.function.c_str(),
              error.line);
    }

    CHECK(python().runString("raise SystemExit(3)"), "SystemExit ends the script, not the app");
    CHECK(python().runString("x = 1"), "and the runtime keeps working");
}

USDCC_TEST(python_syntax_errors_are_logged) {
    // A script that doesn't compile: the exception has no traceback.
    ScopedCollect collect;
    CHECK(!python().runString("print(sys.9version)\n", "<syntax test>"), "a script that doesn't compile fails");
    auto errors = collect.sink->where("python.exception");
    CHECK(errors.size() == 1, "one Error: %zu", errors.size());
    if (errors.size() == 1) {
        CHECK(errors[0].message.find("SyntaxError") == 0, "SyntaxError as the message: %s", errors[0].message.c_str());
        CHECK(errors[0].file == "<syntax test>" && errors[0].line == 1, "located at its line: %s:%d",
              errors[0].file.c_str(), errors[0].line);
    }
    CHECK(python().runString("x = 1"), "and the runtime keeps working");
}

USDCC_TEST(python_threads) {
    ScopedCollect collect;
    // Between runs the main thread doesn't hold the GIL, so another thread
    // can run Python (as OpenUSD's own threads do through TfPyLock).
    bool workerRan = false;
    std::thread worker([&workerRan] { workerRan = python().runString("import math\nmath.sqrt(2)\n"); });
    worker.join();
    CHECK(workerRan, "Python runs from another thread");

    python().runString(
        "import threading\n"
        "def work():\n"
        "    print('from a Python thread')\n"
        "    raise RuntimeError('thread failed')\n"
        "t = threading.Thread(target=work, name='py-worker')\n"
        "t.start(); t.join()\n");
    auto out = collect.sink->where("python.stdout");
    CHECK(out.size() == 1 && out[0].threadName == "py-worker", "a Python thread's name: %s",
          out.empty() ? "?" : out[0].threadName.c_str());
    auto failed = collect.sink->where("python.thread");
    CHECK(failed.size() == 1 && failed[0].message == "RuntimeError: thread failed",
          "an exception in a Python thread is logged: %zu", failed.size());
}

USDCC_TEST(python_run_file) {
    ScopedCollect collect;
    const auto file = std::filesystem::temp_directory_path() / "usdcc_python_run_file_test.py";
    std::ofstream(file) << "import os\nprint(os.path.basename(__file__))\nraise KeyError('missing')\n";
    const bool ok = python().runFile(file);
    std::filesystem::remove(file);

    CHECK(!ok, "the file's exception fails the run");
    auto out = collect.sink->where("python.stdout");
    CHECK(out.size() == 1 && out[0].message == "usdcc_python_run_file_test.py", "__file__ is set: %s",
          out.empty() ? "?" : out[0].message.c_str());
    auto errors = collect.sink->where("python.exception");
    CHECK(errors.size() == 1 && errors[0].file.find("usdcc_python_run_file_test.py") != std::string::npos &&
              errors[0].line == 3,
          "the traceback names the file and line: %s:%d", errors.empty() ? "?" : errors[0].file.c_str(),
          errors.empty() ? 0 : errors[0].line);
    CHECK(!collect.sink->where("usdcc.python").empty(), "the run itself is logged");
}

USDCC_TEST(python_run_file_with_byte_order_mark) {
    ScopedCollect collect;
    const auto file = std::filesystem::temp_directory_path() / "usdcc_python_bom_test.py";
    std::ofstream(file, std::ios::binary) << "\xEF\xBB\xBF" << "print('after the mark')\n";
    const bool ok = python().runFile(file);
    std::filesystem::remove(file);

    CHECK(ok, "a file starting with a UTF-8 byte order mark runs, as in python");
    auto out = collect.sink->where("python.stdout");
    CHECK(out.size() == 1 && out[0].message == "after the mark", "and prints: %s", out.empty() ? "?" : out[0].message.c_str());
}
