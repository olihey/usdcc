#pragma once

#include "usdcc/core/export.h"
#include "usdcc/core/log.h"

namespace usdcc::core {

// Writes every record to stderr, and on Windows also to the debugger's output
// window (OutputDebugString), in the log file's format. Once the diagnostic
// bridge is installed OpenUSD no longer prints its messages itself, so this is
// how a developer still sees them in a terminal or in Visual Studio's Output
// pane. Attached in Debug builds and when USDCC_LOG_STDERR is set (main.cpp).
// Writes synchronously; collapsed repeats aren't printed again.
//
// TODO(LOGGING.md phase 4): once fd 2 is captured into the hub, this must
// write to the original stderr, or every line would come back as a record.
class ConsoleLogSink : public LogSink {
public:
    USDCC_CORE_API ConsoleLogSink();
    USDCC_CORE_API void onRecord(const LogRecord& record) override;
    USDCC_CORE_API void flush() override;
};

}  // namespace usdcc::core
