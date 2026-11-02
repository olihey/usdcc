// LogHub (levels, ring buffer, collapsing, sinks, re-entrancy, threads), the
// USDCC_LOG_* macros and FileLogSink — docs/LOGGING.md phase 1.
#include "test_harness.h"

#include "usdcc/core/file_log_sink.h"
#include "usdcc/core/log.h"

#include <QCoreApplication>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using usdcc::core::FileLogSink;
using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;
using usdcc::core::LogSink;

namespace {
struct CollectingSink : LogSink {
    std::vector<LogRecord> records;
    int repeats = 0;
    void onRecord(const LogRecord& record) override { records.push_back(record); }
    void onRepeat(const LogRecord& record) override {
        ++repeats;
        for (LogRecord& existing : records) {
            if (existing.sequence == record.sequence) existing = record;
        }
    }
};

LogRecord makeRecord(std::string source, std::string message, LogLevel level = LogLevel::Info) {
    LogRecord record;
    record.level = level;
    record.source = std::move(source);
    record.category = "test";
    record.message = std::move(message);
    return record;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// An empty folder under the system temp folder, deleted again when this goes
// out of scope.
struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(const char* name)
        : path(std::filesystem::temp_directory_path() /
               ("usdcc_log_test_" + std::to_string(QCoreApplication::applicationPid()) + "_" + name)) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

int countOf(const std::string& text, const std::string& part) {
    int count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}
}  // namespace

USDCC_TEST(log_levels) {
    LogHub hub;
    CHECK(hub.isEnabled(LogLevel::Info, "usdcc", "usdcc.stage"), "Info is on by default");
    CHECK(!hub.isEnabled(LogLevel::Debug, "usdcc", "usdcc.stage"), "Debug is off by default");
    CHECK(hub.isEnabled(LogLevel::Fatal, "usdcc", "x") , "Fatal is always on");

    CHECK(hub.configureLevels("warning, usdcc.viewport=debug ,usd=error"), "valid spec");
    CHECK(hub.defaultLevel() == LogLevel::Warning, "bare level sets the default");
    CHECK(!hub.isEnabled(LogLevel::Info, "usdcc", "usdcc.stage"), "default now Warning");
    CHECK(hub.isEnabled(LogLevel::Debug, "usdcc", "usdcc.viewport"), "category key");
    CHECK(hub.isEnabled(LogLevel::Debug, "usdcc", "usdcc.viewport.picking"), "covers dotted sub-categories");
    CHECK(!hub.isEnabled(LogLevel::Debug, "usdcc", "usdcc.viewportx"), "but not a mere string prefix");
    CHECK(!hub.isEnabled(LogLevel::Warning, "usd", "TF_DIAGNOSTIC_WARNING_TYPE"), "source key");
    CHECK(hub.isEnabled(LogLevel::Error, "usd", "TF_DIAGNOSTIC_RUNTIME_ERROR_TYPE"), "source key allows Error");

    hub.setLevel("usdcc.viewport.picking", LogLevel::Error);
    CHECK(!hub.isEnabled(LogLevel::Info, "usdcc", "usdcc.viewport.picking"), "longest key wins");
    CHECK(hub.isEnabled(LogLevel::Debug, "usdcc", "usdcc.viewport.camera"), "sibling keeps the shorter key");

    CHECK(!hub.configureLevels("loud,usdcc.x=sometimes,=info,info"), "bad parts are reported");
    CHECK(hub.defaultLevel() == LogLevel::Info, "valid parts still apply");

    CollectingSink* sink = new CollectingSink;
    hub.addSink(std::shared_ptr<LogSink>(sink));
    hub.log(LogLevel::Debug, "usdcc", "usdcc.stage", "dropped");
    hub.log(LogLevel::Info, "usdcc", "usdcc.stage", "kept");
    CHECK(sink->records.size() == 1 && sink->records[0].message == "kept", "post() filters too: %zu",
          sink->records.size());
}

USDCC_TEST(log_ring_buffer_and_backlog) {
    LogHub hub;
    hub.setCapacity(3);
    for (int i = 0; i < 5; ++i) hub.post(makeRecord("usdcc", "message " + std::to_string(i)));

    const std::vector<LogRecord> records = hub.snapshot();
    CHECK(records.size() == 3, "capacity bounds the buffer: %zu", records.size());
    CHECK(records.front().message == "message 2" && records.back().message == "message 4", "oldest dropped");
    CHECK(records[0].sequence < records[1].sequence && records[1].sequence < records[2].sequence,
          "sequence increases");
    CHECK(records[0].threadId != 0, "thread id stamped");
    CHECK(records[0].time != std::chrono::system_clock::time_point(), "time stamped");

    auto late = std::make_shared<CollectingSink>();
    hub.addSink(late);
    CHECK(late->records.size() == 3, "a late sink gets the backlog: %zu", late->records.size());

    auto live = std::make_shared<CollectingSink>();
    hub.addSink(live, false);
    hub.post(makeRecord("usdcc", "after"));
    CHECK(live->records.size() == 1, "without replay only new records: %zu", live->records.size());

    hub.removeSink(live.get());
    hub.post(makeRecord("usdcc", "after removal"));
    CHECK(live->records.size() == 1, "a removed sink gets nothing");
    CHECK(late->records.size() == 5, "others still do: %zu", late->records.size());
}

USDCC_TEST(log_collapses_repeats) {
    LogHub hub;
    auto sink = std::make_shared<CollectingSink>();
    hub.addSink(sink);

    const auto start = std::chrono::system_clock::now();
    auto at = [&](std::string source, std::string message, int ms) {
        LogRecord record = makeRecord(std::move(source), std::move(message), LogLevel::Warning);
        record.time = start + std::chrono::milliseconds(ms);
        hub.post(std::move(record));
    };

    at("usd", "GL error", 0);
    at("usd", "GL error", 10);
    at("qt", "unrelated", 15);  // another source doesn't break the run
    at("usd", "GL error", 20);
    CHECK(sink->records.size() == 2, "two records: %zu", sink->records.size());
    CHECK(sink->records[0].repeatCount == 3, "collapsed: x%u", sink->records[0].repeatCount);
    CHECK(sink->records[0].lastTime == start + std::chrono::milliseconds(20), "lastTime follows");
    CHECK(sink->repeats == 2, "onRepeat per collapse: %d", sink->repeats);

    at("usd", "GL error", 5000);  // past the window
    CHECK(sink->records.size() == 3, "a late repeat starts a new record");

    at("usd", "other error", 5001);
    at("usd", "GL error", 5002);
    CHECK(sink->records.size() == 5, "only the source's latest record collapses: %zu", sink->records.size());

    LogRecord sameTextOtherLevel = makeRecord("usd", "GL error", LogLevel::Error);
    sameTextOtherLevel.time = start + std::chrono::milliseconds(5003);
    hub.post(sameTextOtherLevel);
    CHECK(sink->records.size() == 6, "a different level doesn't collapse");
}

USDCC_TEST(log_sink_that_logs) {
    LogHub hub;
    struct EchoSink : LogSink {
        LogHub* hub = nullptr;
        std::vector<std::string> seen;
        void onRecord(const LogRecord& record) override {
            seen.push_back(record.message);
            // Would deadlock if delivered at once; must arrive after, and its
            // own echo must be dropped instead of looping forever.
            hub->log(LogLevel::Info, "usdcc", "echo", "echo of " + record.message);
        }
    };
    auto sink = std::make_shared<EchoSink>();
    sink->hub = &hub;
    hub.addSink(sink);

    hub.log(LogLevel::Info, "usdcc", "test", "hello");
    CHECK(sink->seen.size() == 2, "the sink's own post is delivered once: %zu", sink->seen.size());
    CHECK(sink->seen.size() == 2 && sink->seen[1] == "echo of hello", "after the outer one");

    hub.log(LogLevel::Info, "usdcc", "test", "again");
    CHECK(sink->seen.size() == 4, "the next post works normally: %zu", sink->seen.size());
}

USDCC_TEST(log_from_many_threads) {
    LogHub hub;
    auto sink = std::make_shared<CollectingSink>();
    hub.addSink(sink);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&hub, t] {
            usdcc::core::setThreadName("worker-" + std::to_string(t));
            for (int i = 0; i < kPerThread; ++i) {
                hub.log(LogLevel::Info, "usdcc", "test", std::to_string(t) + ":" + std::to_string(i));
            }
        });
    }
    for (auto& thread : threads) thread.join();

    CHECK(sink->records.size() == kThreads * kPerThread, "nothing lost: %zu", sink->records.size());
    bool ordered = true;
    for (std::size_t i = 1; i < sink->records.size(); ++i) {
        ordered = ordered && sink->records[i].sequence == sink->records[i - 1].sequence + 1;
    }
    CHECK(ordered, "the sink sees records in sequence order");
    CHECK(!sink->records.empty() && sink->records.front().threadName.starts_with("worker-"),
          "each record carries its thread's name");
}

USDCC_TEST(log_macros) {
    LogHub& hub = LogHub::instance();
    auto sink = std::make_shared<CollectingSink>();
    hub.addSink(sink, false);

    int evaluated = 0;
    auto expensive = [&evaluated] { return ++evaluated; };
    USDCC_LOG_DEBUG("usdcc.test", "never formatted {}", expensive());
    CHECK(evaluated == 0, "a disabled level doesn't evaluate its arguments");

    const int line = __LINE__ + 1;
    USDCC_LOG_WARNING("usdcc.test", "value {} of {}", 3, "four");
    hub.removeSink(sink.get());

    CHECK(sink->records.size() == 1, "one record: %zu", sink->records.size());
    if (sink->records.size() == 1) {
        const LogRecord& record = sink->records[0];
        CHECK(record.message == "value 3 of four", "formatted: %s", record.message.c_str());
        CHECK(record.level == LogLevel::Warning && record.source == "usdcc" && record.category == "usdcc.test",
              "level, source and category");
        CHECK(record.line == line && record.file.ends_with("log_test.cpp"), "call site: %s:%d",
              record.file.c_str(), record.line);
    }
}

USDCC_TEST(log_file_format) {
    LogRecord record = makeRecord("usd", "first line\nsecond line", LogLevel::Warning);
    record.category = "TF_DIAGNOSTIC_WARNING_TYPE";
    record.file = "C:/src/pxr/usd/stage.cpp";
    record.line = 42;
    record.threadName = "main";
    record.details = "Traceback:\n  frame";
    record.time = std::chrono::system_clock::now();

    const std::string text = FileLogSink::formatRecord(record);
    CHECK(text.find(" W [main] usd/TF_DIAGNOSTIC_WARNING_TYPE: first line (stage.cpp:42)\n") != std::string::npos,
          "first line: %s", text.c_str());
    CHECK(text.find("\n    second line\n    Traceback:\n      frame\n") != std::string::npos,
          "continuation and details indented: %s", text.c_str());
    CHECK(text.size() > 23 && text[4] == '-' && text[10] == ' ' && text[19] == '.', "timestamp: %s",
          text.c_str());
}

USDCC_TEST(log_file_sink) {
    const ScratchDir dir("sink");
    const auto path = dir.path / "test.log";
    LogHub hub;
    {
        auto sink = std::make_shared<FileLogSink>(path);
        CHECK(sink->isOpen(), "opened %s", path.string().c_str());
        hub.addSink(sink);
        hub.log(LogLevel::Info, "usdcc", "usdcc.test", "hello file");
        for (int i = 0; i < 5; ++i) hub.log(LogLevel::Warning, "usd", "gl", "GL error");
        hub.log(LogLevel::Error, "usd", "gl", "something else");
        sink->flush();

        const std::string text = readFile(path);
        CHECK(text.find("usdcc log started") != std::string::npos, "header line");
        CHECK(text.find("I [") != std::string::npos && text.find("usdcc/usdcc.test: hello file") != std::string::npos,
              "info line: %s", text.c_str());
        CHECK(countOf(text, "usd/gl: GL error") == 1, "a repeat run is written once: %s", text.c_str());
        CHECK(text.find("(previous message repeated 4 more times)") != std::string::npos, "then summarized: %s",
              text.c_str());
        CHECK(text.find("usd/gl: something else") != std::string::npos, "error line");

        hub.log(LogLevel::Warning, "qt", "qpa", "tail");
        hub.log(LogLevel::Warning, "qt", "qpa", "tail");
        hub.removeSink(sink.get());
    }  // destruction writes what's queued and the open run's summary

    const std::string text = readFile(path);
    CHECK(text.find("qt/qpa: tail") != std::string::npos, "queued lines written on close");
    CHECK(text.find("(previous message repeated 1 more time)") != std::string::npos, "open run summarized on close: %s",
          text.c_str());
}

USDCC_TEST(log_file_rotation) {
    const ScratchDir scratch("rotation");
    const auto& dir = scratch.path;
    for (int i = 0; i < 12; ++i) {
        std::ofstream(dir / ("usdcc-20200101-0000" + std::string(i < 10 ? "0" : "") + std::to_string(i) + "-1.log"));
    }
    std::ofstream(dir / "unrelated.txt");

    auto sink = FileLogSink::openInDirectory(dir, 10);
    CHECK(sink != nullptr, "opened a new file");
    int logs = 0;
    bool oldestGone = true;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".log")) ++logs;
        if (name == "usdcc-20200101-000000-1.log" || name == "usdcc-20200101-000002-1.log") oldestGone = false;
    }
    CHECK(logs == 10, "ten logs kept, the new one included: %d", logs);
    CHECK(oldestGone, "the oldest were deleted");
    CHECK(std::filesystem::exists(dir / "unrelated.txt"), "other files untouched");
    CHECK(std::filesystem::exists(dir / "usdcc-20200101-000011-1.log"), "the newest old log kept");
}
