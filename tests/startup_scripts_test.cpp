// Startup scripts (docs/SCRIPTING.md §6, phase 4): every *.py in the startup
// folder runs, in name order, a failing one doesn't stop the rest, and the
// scripts folder is on sys.path for the modules they share.
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/startup_scripts.h"

#include <QCoreApplication>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using usdcc::core::LogHub;
using usdcc::core::LogRecord;
using usdcc::core::LogSink;
using usdcc::scripting::PythonRuntime;
using usdcc::scripting::runStartupScripts;

namespace {
struct Collect : LogSink {
    std::mutex mutex;
    std::vector<LogRecord> records;
    void onRecord(const LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    int count(const std::string& category, const std::string& text) {
        std::lock_guard lock(mutex);
        int n = 0;
        for (const LogRecord& record : records) {
            if (record.category == category && record.message.find(text) != std::string::npos) ++n;
        }
        return n;
    }
};

struct ScratchFolder {
    std::filesystem::path path;
    explicit ScratchFolder(const std::string& name)
        : path(std::filesystem::temp_directory_path() /
               ("usdcc_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::filesystem::create_directories(path);
    }
    ~ScratchFolder() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void write(const std::filesystem::path& name, const std::string& text) const {
        std::filesystem::create_directories((path / name).parent_path());
        std::ofstream(path / name, std::ios::binary) << text;
    }
};

PythonRuntime& python() { return *PythonRuntime::instance(); }
}  // namespace

USDCC_TEST(startup_scripts) {
    ScratchFolder scripts("startup_scripts");
    // A shared module next to startup/, which each script imports and
    // records itself in, so the order they ran in shows.
    scripts.write("usdcc_test_shared.py", "ran = []\n");
    scripts.write("startup/b_fails.py", "import usdcc_test_shared\nusdcc_test_shared.ran.append('b')\n"
                                        "raise RuntimeError('startup b failed')\n");
    scripts.write("startup/A_first.py", "import usdcc_test_shared\nusdcc_test_shared.ran.append('A')\n");
    scripts.write("startup/c_last.py", "import usdcc_test_shared\nusdcc_test_shared.ran.append('c')\n"
                                       "print('hello from startup c')\n");
    scripts.write("startup/notes.txt", "not a script\n");

    auto sink = std::make_shared<Collect>();
    LogHub::instance().addSink(sink, false);
    const auto result = runStartupScripts(python(), scripts.path);
    LogHub::instance().removeSink(sink.get());

    CHECK(result.ran == 3 && result.failed == 1, "three ran, one failed: %d, %d", result.ran, result.failed);
    const auto order = python().evaluate("__import__('usdcc_test_shared').ran");
    CHECK(order && *order == "['A', 'b', 'c']", "in name order, ignoring case, past the failing one: %s",
          order ? order->c_str() : "?");
    CHECK(sink->count("python.exception", "startup b failed") == 1, "the failure is logged with its exception");
    CHECK(sink->count("python.stdout", "hello from startup c") == 1, "the scripts' output is in the log");
    CHECK(sink->count("usdcc.python", "1 of 3 startup scripts stopped with an error") == 1, "and a summary");

    const auto shadowed = python().evaluate("__import__('usdcc.log').__name__");
    CHECK(shadowed && *shadowed == "usdcc", "the scripts folder comes after the usdcc package");
}

USDCC_TEST(startup_scripts_skipped) {
    ScratchFolder scripts("startup_skipped");
    scripts.write("startup/would_fail.py", "raise RuntimeError('should not run')\n");
    const auto result = runStartupScripts(python(), scripts.path, false);
    CHECK(result.ran == 0 && result.failed == 0, "nothing runs with --no-startup-scripts");

    ScratchFolder empty("startup_missing");
    std::filesystem::remove_all(empty.path);
    CHECK(runStartupScripts(python(), empty.path).ran == 0 && std::filesystem::is_directory(empty.path / "startup"),
          "a missing startup folder is created, and nothing runs");
}
