#include "usdcc/core/console_log_sink.h"

#include "usdcc/core/file_log_sink.h"
#include "usdcc/core/native_stream_capture.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace usdcc::core {

ConsoleLogSink::ConsoleLogSink() = default;

void ConsoleLogSink::onRecord(const LogRecord& record) {
    const std::string text = FileLogSink::formatRecord(record);
    writeToOriginalStderr(text);
#if defined(_WIN32)
    OutputDebugStringA(text.c_str());
#endif
}

void ConsoleLogSink::flush() {}  // writeToOriginalStderr() doesn't buffer

}  // namespace usdcc::core
