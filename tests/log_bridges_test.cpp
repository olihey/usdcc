// TfDiagnosticBridge and QtMessageBridge: OpenUSD's and Qt's messages arrive
// in the usdcc log with the right level, source, category and call site —
// docs/LOGGING.md phase 2.
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/qt_message_bridge.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/tf_diagnostic_bridge.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usd/stage.h>

#include <QCoreApplication>
#include <QLoggingCategory>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

// The TF_* macros name Tf's helpers unqualified.
PXR_NAMESPACE_USING_DIRECTIVE

using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;
using usdcc::core::LogSink;

namespace {
// Collects what reaches LogHub::instance() while it exists.
struct Capture : LogSink {
    std::mutex mutex;
    std::vector<LogRecord> records;
    Capture() = default;
    void onRecord(const LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    std::vector<LogRecord> from(const std::string& source) {
        std::lock_guard lock(mutex);
        std::vector<LogRecord> matching;
        for (const LogRecord& record : records) {
            if (record.source == source) matching.push_back(record);
        }
        return matching;
    }
};

struct ScopedCapture {
    std::shared_ptr<Capture> sink = std::make_shared<Capture>();
    ScopedCapture() { LogHub::instance().addSink(sink, false); }
    ~ScopedCapture() { LogHub::instance().removeSink(sink.get()); }
};

Q_LOGGING_CATEGORY(lcBridgeTest, "usdcc.test.qt")
}  // namespace

USDCC_TEST(tf_bridge_levels_and_categories) {
    usdcc::usd::TfDiagnosticBridge bridge;
    ScopedCapture capture;

    const int line = __LINE__ + 1;
    TF_STATUS("status message");
    TF_WARN("warning message");
    TF_CODING_ERROR("coding error message");
    TF_RUNTIME_ERROR("runtime error message");

    const std::vector<LogRecord> records = capture.sink->from("usd");
    CHECK(records.size() == 4, "one record each: %zu", records.size());
    if (records.size() != 4) return;

    const LogLevel levels[] = {LogLevel::Info, LogLevel::Warning, LogLevel::Error, LogLevel::Error};
    const char* categories[] = {"TF_DIAGNOSTIC_STATUS_TYPE", "TF_DIAGNOSTIC_WARNING_TYPE",
                                "TF_DIAGNOSTIC_CODING_ERROR_TYPE", "TF_DIAGNOSTIC_RUNTIME_ERROR_TYPE"};
    const char* messages[] = {"status message", "warning message", "coding error message", "runtime error message"};
    for (int i = 0; i < 4; ++i) {
        const LogRecord& record = records[i];
        CHECK(record.level == levels[i], "[%d] level %s", i, std::string(logLevelName(record.level)).c_str());
        CHECK(record.category == categories[i], "[%d] category %s", i, record.category.c_str());
        CHECK(record.message == messages[i], "[%d] message %s", i, record.message.c_str());
        CHECK(record.file.ends_with("log_bridges_test.cpp") && record.line == line + i, "[%d] call site %s:%d", i,
              record.file.c_str(), record.line);
        CHECK(!record.function.empty(), "[%d] function", i);
    }
}

USDCC_TEST(tf_bridge_error_marks) {
    usdcc::usd::TfDiagnosticBridge bridge;
    ScopedCapture capture;

    {
        PXR_NS::TfErrorMark mark;
        TF_RUNTIME_ERROR("handled error");
        CHECK(capture.sink->from("usd").empty(), "held back while the mark is active");
        mark.Clear();
    }
    CHECK(capture.sink->from("usd").empty(), "a cleared error never reaches the log");

    {
        PXR_NS::TfErrorMark mark;
        TF_RUNTIME_ERROR("unhandled error");
    }
    const std::vector<LogRecord> records = capture.sink->from("usd");
    CHECK(records.size() == 1 && records[0].message == "unhandled error",
          "an error left in the mark is logged when the mark goes away: %zu", records.size());
}

USDCC_TEST(tf_bridge_worker_thread) {
    usdcc::usd::TfDiagnosticBridge bridge;
    ScopedCapture capture;

    std::uint64_t mainThread = 0;
    {
        LogHub::instance().log(LogLevel::Info, "usdcc", "test", "from main");
        mainThread = capture.sink->from("usdcc").back().threadId;
    }
    std::thread worker([] {
        usdcc::core::setThreadName("tf-worker");
        TF_WARN("warning from a worker");
        TF_RUNTIME_ERROR("error from a worker");
    });
    worker.join();

    const std::vector<LogRecord> records = capture.sink->from("usd");
    CHECK(records.size() == 2, "both arrive: %zu", records.size());
    for (const LogRecord& record : records) {
        CHECK(record.threadName == "tf-worker" && record.threadId != mainThread, "worker thread recorded: %s",
              record.threadName.c_str());
    }
}

USDCC_TEST(tf_bridge_stage_open_errors) {
    usdcc::usd::TfDiagnosticBridge bridge;
    ScopedCapture capture;

    const auto file = std::filesystem::temp_directory_path() /
                      ("usdcc_broken_" + std::to_string(QCoreApplication::applicationPid()) + ".usda");
    std::ofstream(file) << "#usda 1.0\ndef Xform \"A\" {\n    this is not usda\n";

    std::vector<std::string> errors;
    const PXR_NS::UsdStageRefPtr stage = usdcc::usd::StageManager::loadUsdStage(file.string(), &errors);
    std::filesystem::remove(file);

    CHECK(!stage, "a broken file doesn't open");
    CHECK(!errors.empty(), "the caller gets the reasons");
    const std::vector<LogRecord> usd = capture.sink->from("usd");
    CHECK(!usd.empty() && usd.size() == errors.size(), "the same errors are in the log: %zu vs %zu", usd.size(),
          errors.size());
    const std::vector<LogRecord> ours = capture.sink->from("usdcc");
    CHECK(ours.size() == 1 && ours[0].level == LogLevel::Error && ours[0].category == "usdcc.stage" &&
              !errors.empty() && ours[0].message.ends_with(errors.front()),
          "usdcc's summary names the first error: %s", ours.empty() ? "" : ours[0].message.c_str());
}

USDCC_TEST(qt_bridge) {
    ScopedCapture capture;
    {
        usdcc::ui::QtMessageBridge bridge;
        qWarning("plain warning");
        qCritical("critical message");
        qCInfo(lcBridgeTest) << "category info";
        qDebug("debug message");  // Debug isn't enabled by default

        LogHub::instance().setLevel("qt", LogLevel::Debug);
        qDebug("debug when enabled");
        LogHub::instance().clearLevels();
    }
    qWarning("after the bridge is gone");

    const std::vector<LogRecord> records = capture.sink->from("qt");
    CHECK(records.size() == 4, "four kept: %zu", records.size());
    if (records.size() != 4) return;
    CHECK(records[0].level == LogLevel::Warning && records[0].category == "default" &&
              records[0].message == "plain warning",
          "qWarning: %s/%s", records[0].category.c_str(), records[0].message.c_str());
    CHECK(records[1].level == LogLevel::Error && records[1].message == "critical message", "qCritical is an Error");
    CHECK(records[2].level == LogLevel::Info && records[2].category == "usdcc.test.qt" &&
              records[2].message == "category info",
          "qCInfo: %s/%s", records[2].category.c_str(), records[2].message.c_str());
    CHECK(records[3].level == LogLevel::Debug && records[3].message == "debug when enabled", "qDebug when enabled");
}
