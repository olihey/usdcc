#pragma once

// The application's one log (docs/LOGGING.md). Every source — usdcc's own
// code through the USDCC_LOG_* macros below, and later Qt, OpenUSD, Python
// and native stdout/stderr through their bridges — turns its message into a
// LogRecord and posts it to LogHub::instance(). Sinks (the log file, the Log
// panel) subscribe to the hub; no source knows about any sink.
//
//   USDCC_LOG_INFO("usdcc.stage", "Opened {} ({} prims)", path, primCount);
//
// Qt-free, and the hub lives in usdcc_core's shared library so that every
// module in the process posts to the same instance (§2.2).

#include "usdcc/core/export.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace usdcc::core {

enum class LogLevel : int { Trace, Debug, Info, Warning, Error, Fatal };

// "trace", "debug", "info", "warning", "error", "fatal".
USDCC_CORE_API std::string_view logLevelName(LogLevel level);
// Accepts the names above in any case, plus "warn". False if `text` is none.
USDCC_CORE_API bool parseLogLevel(std::string_view text, LogLevel* level);

struct LogRecord {
    std::uint64_t sequence = 0;  // assigned by the hub, strictly increasing
    std::chrono::system_clock::time_point time;      // first occurrence; the hub fills it if unset
    std::chrono::system_clock::time_point lastTime;  // latest occurrence, when collapsed
    LogLevel level = LogLevel::Info;
    std::string source;    // "usdcc", "qt", "usd", "python", "stdout", "stderr", "plugin:<name>"
    std::string category;  // "usdcc.viewport", a Tf diagnostic code, a Python logger name, ...
    std::string message;
    std::string details;   // optional: a traceback, a long diagnostic's remainder
    std::string file;      // optional source location
    int line = 0;
    std::string function;
    std::uint64_t threadId = 0;  // the hub fills these two with the posting thread's if unset
    std::string threadName;
    std::uint32_t repeatCount = 1;  // > 1 once identical records were collapsed into this one
};

// Receives records from a LogHub. Both calls are made with the hub's lock
// held, on whichever thread posted, so they must be quick and must not call
// back into the hub (a sink's own logging is deferred, see LogHub::post()).
class LogSink {
public:
    virtual ~LogSink() = default;
    // A new record.
    virtual void onRecord(const LogRecord& record) = 0;
    // An identical record was collapsed into `record`: its repeatCount and
    // lastTime changed (docs/LOGGING.md §2.3).
    virtual void onRepeat(const LogRecord& record) { (void)record; }
    // Get everything received so far to its destination before returning
    // (LogHub::flush(); also after every Fatal record).
    virtual void flush() {}
};

class LogHub {
public:
    // The process-wide hub every USDCC_LOG_* call and bridge posts to.
    USDCC_CORE_API static LogHub& instance();

    // Separate hubs are for tests; the app only uses instance().
    USDCC_CORE_API LogHub();
    USDCC_CORE_API ~LogHub();
    LogHub(const LogHub&) = delete;
    LogHub& operator=(const LogHub&) = delete;

    // Thread-safe. Drops the record if its level isn't enabled (isEnabled()).
    // A Fatal record is followed by flush(): the process is about to end.
    // Otherwise stamps sequence, time and thread, then either collapses it into
    // the previous record from the same source (same level, category, message,
    // details, file and line, within the repeat window) or appends it to the
    // ring buffer, and tells every sink. A post made from inside a sink is
    // queued and delivered once the outer post is done; posts made while
    // delivering those are dropped, so a sink that logs can't loop.
    USDCC_CORE_API void post(LogRecord record);
    // post() for a record with just these fields.
    USDCC_CORE_API void log(LogLevel level, std::string_view source, std::string_view category,
                            std::string message);

    // Whether a record at `level` from `source`/`category` is kept. The most
    // specific configured level wins: the longest key that is the category or
    // a dot-separated prefix of it ("usdcc.viewport" covers
    // "usdcc.viewport.picking"), then a key equal to the source, then the
    // default. Fatal is always enabled.
    USDCC_CORE_API bool isEnabled(LogLevel level, std::string_view source, std::string_view category) const;
    USDCC_CORE_API void setDefaultLevel(LogLevel level);  // Info unless changed
    USDCC_CORE_API LogLevel defaultLevel() const;
    // `key` is a source or a category prefix, as described at isEnabled().
    USDCC_CORE_API void setLevel(std::string_view key, LogLevel level);
    USDCC_CORE_API void clearLevels();  // keys only; the default stays
    // Applies a spec such as "warning,usdcc.viewport=debug,usd=error": a bare
    // level sets the default, key=level sets that key. Returns false if any
    // part couldn't be parsed; the others are still applied.
    USDCC_CORE_API bool configureLevels(std::string_view spec);

    // Adds a sink. With `replayBacklog`, it first receives every record still
    // in the ring buffer, so a sink created late (the Log panel) misses nothing.
    USDCC_CORE_API void addSink(std::shared_ptr<LogSink> sink, bool replayBacklog = true);
    // Once this returns, `sink` is never called again.
    USDCC_CORE_API void removeSink(const LogSink* sink);

    // Calls every sink's flush(), e.g. before the process ends abnormally.
    USDCC_CORE_API void flush();

    // The records in the ring buffer, oldest first.
    USDCC_CORE_API std::vector<LogRecord> snapshot() const;
    USDCC_CORE_API void setCapacity(std::size_t capacity);  // 10 000 unless changed
    USDCC_CORE_API std::size_t capacity() const;
    // How far apart two identical records may be and still collapse (2 s).
    USDCC_CORE_API void setRepeatWindow(std::chrono::milliseconds window);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// The name records posted from the calling thread carry ("main",
// "stage-loader", ...). Empty unless set.
USDCC_CORE_API void setThreadName(std::string name);
USDCC_CORE_API const std::string& threadName();

namespace detail {
template <typename... Args>
void logFormatted(LogLevel level, std::string_view category, const std::source_location& where,
                  std::format_string<Args...> format, Args&&... args) {
    LogRecord record;
    record.level = level;
    record.source = "usdcc";
    record.category = category;
    record.message = std::format(format, std::forward<Args>(args)...);
    record.file = where.file_name();
    record.line = static_cast<int>(where.line());
    record.function = where.function_name();
    LogHub::instance().post(std::move(record));
}
}  // namespace detail

}  // namespace usdcc::core

// usdcc's own logging: a category, then a std::format string and its
// arguments. The arguments aren't evaluated when the level is disabled.
#define USDCC_LOG(level, category, ...)                                                                    \
    do {                                                                                                   \
        if (::usdcc::core::LogHub::instance().isEnabled(level, "usdcc", category)) {                       \
            ::usdcc::core::detail::logFormatted(level, category, std::source_location::current(), __VA_ARGS__); \
        }                                                                                                  \
    } while (0)

#define USDCC_LOG_TRACE(category, ...) USDCC_LOG(::usdcc::core::LogLevel::Trace, category, __VA_ARGS__)
#define USDCC_LOG_DEBUG(category, ...) USDCC_LOG(::usdcc::core::LogLevel::Debug, category, __VA_ARGS__)
#define USDCC_LOG_INFO(category, ...) USDCC_LOG(::usdcc::core::LogLevel::Info, category, __VA_ARGS__)
#define USDCC_LOG_WARNING(category, ...) USDCC_LOG(::usdcc::core::LogLevel::Warning, category, __VA_ARGS__)
#define USDCC_LOG_ERROR(category, ...) USDCC_LOG(::usdcc::core::LogLevel::Error, category, __VA_ARGS__)
#define USDCC_LOG_FATAL(category, ...) USDCC_LOG(::usdcc::core::LogLevel::Fatal, category, __VA_ARGS__)
