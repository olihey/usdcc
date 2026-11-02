// NativeStreamCapture: what is written to stdout and stderr at the C level
// arrives in the log, line by line, and stops arriving once capture ends —
// docs/LOGGING.md phase 4. Every CHECK runs after the capture is gone: a
// failing CHECK prints to stdout, which would otherwise be captured itself.
#include "test_harness.h"

#include "usdcc/core/console_log_sink.h"
#include "usdcc/core/log.h"
#include "usdcc/core/native_stream_capture.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;
using usdcc::core::LogSink;
using usdcc::core::NativeStreamCapture;

namespace {
struct Collect : LogSink {
    std::mutex mutex;
    std::vector<LogRecord> records;
    void onRecord(const LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    std::vector<std::string> messages(const std::string& source) {
        std::lock_guard lock(mutex);
        std::vector<std::string> result;
        for (const LogRecord& record : records) {
            if (record.source == source) result.push_back(record.message);
        }
        return result;
    }
};

struct ScopedCollect {
    std::shared_ptr<Collect> sink = std::make_shared<Collect>();
    ScopedCollect() { LogHub::instance().addSink(sink, false); }
    ~ScopedCollect() { LogHub::instance().removeSink(sink.get()); }
};

void rawWrite(int fd, const char* text) {
#if defined(_WIN32)
    _write(fd, text, static_cast<unsigned>(std::strlen(text)));
#else
    (void)!write(fd, text, std::strlen(text));
#endif
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) text += "[" + line + "]";
    return text;
}
}  // namespace

USDCC_TEST(native_capture_lines) {
    ScopedCollect collect;
    bool active = false;
    {
        NativeStreamCapture capture;
        active = capture.isActive();
        std::printf("plain printf\n");
        std::fprintf(stderr, "to stderr\n");
        std::cout << "via std::cout" << std::endl;
        rawWrite(1, "raw descriptor write\r\n");
        std::thread([] { std::printf("from a worker thread\n"); }).join();
        std::printf("one line ");
        std::printf("written in two calls\n\n\n");  // the empty lines are dropped
        std::printf("tail without a newline");
    }
    std::printf("after capture\n");  // the real stdout again
    std::fflush(stdout);

    CHECK(active, "capture is active");
    const std::vector<std::string> out = collect.sink->messages("stdout");
    const std::vector<std::string> expected = {"plain printf",       "via std::cout",
                                               "raw descriptor write", "from a worker thread",
                                               "one line written in two calls", "tail without a newline"};
    CHECK(out == expected, "stdout lines in order: %s", joined(out).c_str());
    const std::vector<std::string> err = collect.sink->messages("stderr");
    CHECK(err == std::vector<std::string>({"to stderr"}), "stderr lines: %s", joined(err).c_str());

    std::lock_guard lock(collect.sink->mutex);
    for (const LogRecord& record : collect.sink->records) {
        if (record.source == "stdout") {
            CHECK(record.level == LogLevel::Info && record.category == "stdout", "stdout: Info, category stdout");
        } else if (record.source == "stderr") {
            CHECK(record.level == LogLevel::Warning && record.category == "stderr", "stderr: Warning, category stderr");
        }
    }
}

USDCC_TEST(native_capture_flood) {
    ScopedCollect collect;
    constexpr int kLines = 20000;
    const auto start = std::chrono::steady_clock::now();
    {
        NativeStreamCapture capture;
        // From two threads at once, far more than a pipe buffer holds.
        std::thread other([] {
            for (int i = 0; i < kLines / 2; ++i) std::fprintf(stderr, "stderr flood %d\n", i);
        });
        for (int i = 0; i < kLines / 2; ++i) std::printf("stdout flood %d\n", i);
        other.join();
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    const std::size_t out = collect.sink->messages("stdout").size();
    const std::size_t err = collect.sink->messages("stderr").size();
    CHECK(out == kLines / 2 && err == kLines / 2, "every line arrives: %zu + %zu", out, err);
    CHECK(elapsed < 10000, "and capture ends promptly: %lld ms", static_cast<long long>(elapsed));
    std::printf("  (%d lines captured in %lld ms)\n", kLines, static_cast<long long>(elapsed));
}

USDCC_TEST(native_capture_console_sink_does_not_loop) {
    ScopedCollect collect;
    // ConsoleLogSink prints every record to stderr; while stderr is captured
    // that would feed each record back in, forever.
    auto console = std::make_shared<usdcc::core::ConsoleLogSink>();
    {
        NativeStreamCapture capture;
        LogHub::instance().addSink(console, false);
        std::printf("captured once\n");
        LogHub::instance().log(LogLevel::Warning, "usdcc", "usdcc.test", "printed by the console sink");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        LogHub::instance().removeSink(console.get());
    }
    CHECK(collect.sink->messages("stdout") == std::vector<std::string>({"captured once"}), "one stdout record: %s",
          joined(collect.sink->messages("stdout")).c_str());
    CHECK(collect.sink->messages("stderr").empty(), "the console sink's output isn't captured: %s",
          joined(collect.sink->messages("stderr")).c_str());
}

USDCC_TEST(native_capture_one_at_a_time_and_off_switch) {
    bool firstActive = false;
    bool secondActive = true;
    {
        NativeStreamCapture first;
        NativeStreamCapture second;
        firstActive = first.isActive();
        secondActive = second.isActive();
    }

#if defined(_WIN32)
    _putenv_s("USDCC_NO_STREAM_CAPTURE", "1");
#else
    setenv("USDCC_NO_STREAM_CAPTURE", "1", 1);
#endif
    bool offActive = true;
    {
        NativeStreamCapture off;
        offActive = off.isActive();
    }
#if defined(_WIN32)
    _putenv_s("USDCC_NO_STREAM_CAPTURE", "");
#else
    unsetenv("USDCC_NO_STREAM_CAPTURE");
#endif

    bool againActive = false;
    {
        NativeStreamCapture again;
        againActive = again.isActive();
    }
    CHECK(firstActive && !secondActive, "only one capture at a time");
    CHECK(!offActive, "USDCC_NO_STREAM_CAPTURE turns it off");
    CHECK(againActive, "and on again once it's unset");
}
