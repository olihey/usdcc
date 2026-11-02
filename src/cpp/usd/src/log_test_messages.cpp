#include "usdcc/usd/log_test_messages.h"

#include "usdcc/core/log.h"

#include <pxr/base/tf/diagnostic.h>

#include <QLoggingCategory>

#include <cstdio>
#include <iostream>
#include <thread>

// The TF_* macros name Tf's helpers unqualified.
PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;

namespace {
Q_LOGGING_CATEGORY(lcLogTest, "usdcc.test.qt")
}  // namespace

void emitLogTestMessages() {
    USDCC_LOG_TRACE("usdcc.test", "usdcc trace message");
    USDCC_LOG_DEBUG("usdcc.test", "usdcc debug message");
    USDCC_LOG_INFO("usdcc.test", "usdcc info message");
    USDCC_LOG_WARNING("usdcc.test", "usdcc warning message");
    USDCC_LOG_ERROR("usdcc.test", "usdcc error message");

    LogRecord multiLine;
    multiLine.level = LogLevel::Warning;
    multiLine.source = "usdcc";
    multiLine.category = "usdcc.test";
    multiLine.message = "A message over\nthree\nlines";
    multiLine.details = "A details block, as a Python traceback will have:\n  File \"script.py\", line 3, in <module>";
    multiLine.file = __FILE__;
    multiLine.line = __LINE__;
    LogHub::instance().post(std::move(multiLine));

    qCDebug(lcLogTest) << "Qt debug message";
    qCInfo(lcLogTest) << "Qt info message";
    qCWarning(lcLogTest) << "Qt warning message";
    qCCritical(lcLogTest) << "Qt critical message";
    qWarning("Qt warning without a category");

    TF_STATUS("OpenUSD status message");
    TF_WARN("OpenUSD warning message");
    TF_CODING_ERROR("OpenUSD coding error message");
    TF_RUNTIME_ERROR("OpenUSD runtime error message");

    // Captured by NativeStreamCapture (unless USDCC_NO_STREAM_CAPTURE is set).
    std::printf("Native stdout line (printf)\n");
    std::fprintf(stderr, "Native stderr line (fprintf)\n");
    std::cout << "Native stdout line (std::cout)" << std::endl;

    std::thread worker([] {
        usdcc::core::setThreadName("log-test-worker");
        std::printf("Native stdout line from a worker thread\n");
        USDCC_LOG_WARNING("usdcc.test", "usdcc warning from a worker thread");
        TF_WARN("OpenUSD warning from a worker thread");
        TF_RUNTIME_ERROR("OpenUSD runtime error from a worker thread");
    });
    worker.join();

    for (int i = 0; i < 500; ++i) {
        USDCC_LOG_WARNING("usdcc.test", "The same warning 500 times (one row, x500)");
    }
}

void emitLogBurst(int count) {
    std::thread worker([count] {
        usdcc::core::setThreadName("log-burst");
        for (int i = 1; i <= count; ++i) {
            USDCC_LOG_INFO("usdcc.test.burst", "Burst message {} of {}", i, count);
        }
    });
    worker.join();
}

}  // namespace usdcc::usd
