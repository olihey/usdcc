#include "usdcc/core/log.h"

#include "usdcc/core/native_stream_capture.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace usdcc::core {

namespace {
constexpr std::size_t kDefaultCapacity = 10000;
constexpr std::chrono::milliseconds kDefaultRepeatWindow{2000};
// Posts made from inside a sink, per outer post; more are dropped.
constexpr std::size_t kMaxDeferredPosts = 64;

std::uint64_t currentThreadId() {
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}

thread_local std::string t_threadName;

// Per thread: inside LogHub::post() (posts made now are deferred), and
// delivering those deferred posts (posts made now are dropped).
thread_local bool t_posting = false;
thread_local bool t_deliveringDeferred = false;
thread_local std::vector<std::pair<LogHub*, LogRecord>> t_deferred;

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// `key` is `category` itself or a dot-separated prefix of it.
bool coversCategory(std::string_view key, std::string_view category) {
    return category.size() >= key.size() && category.compare(0, key.size(), key) == 0 &&
           (category.size() == key.size() || category[key.size()] == '.');
}

// Calls `call` for one sink. A sink that throws must not take the hub down
// with it (or leave this thread marked as posting): the exception is reported
// on the original stderr, the only place left that can't be the failing sink
// (stderr itself may be captured into the hub).
template <typename Call>
void callSink(const char* what, Call&& call) {
    try {
        call();
    } catch (const std::exception& e) {
        writeToOriginalStderr(std::string("usdcc log: a sink's ") + what + " threw: " + e.what() + "\n");
    } catch (...) {
        writeToOriginalStderr(std::string("usdcc log: a sink's ") + what + " threw\n");
    }
}

bool sameMessage(const LogRecord& a, const LogRecord& b) {
    return a.level == b.level && a.line == b.line && a.message == b.message && a.category == b.category &&
           a.file == b.file && a.details == b.details;
}
}  // namespace

std::string_view logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
        case LogLevel::Fatal: return "fatal";
    }
    return "info";
}

bool parseLogLevel(std::string_view text, LogLevel* level) {
    std::string lower(trimmed(text));
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (LogLevel candidate : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warning,
                               LogLevel::Error, LogLevel::Fatal}) {
        if (lower == logLevelName(candidate)) {
            *level = candidate;
            return true;
        }
    }
    if (lower == "warn") {
        *level = LogLevel::Warning;
        return true;
    }
    return false;
}

struct LogHub::Impl {
    mutable std::mutex mutex;  // records, sinks and everything below it
    std::deque<LogRecord> records;
    std::size_t capacity = kDefaultCapacity;
    std::chrono::milliseconds repeatWindow = kDefaultRepeatWindow;
    std::vector<std::shared_ptr<LogSink>> sinks;
    std::uint64_t nextSequence = 1;
    // Sequence number of each source's latest record, for collapsing.
    std::unordered_map<std::string, std::uint64_t> lastBySource;

    mutable std::mutex levelMutex;  // the levels; separate so isEnabled() never waits on a sink
    LogLevel defaultLevel = LogLevel::Info;
    std::vector<std::pair<std::string, LogLevel>> levels;
    // The lowest level any key or the default enables: anything below it is
    // rejected without taking levelMutex (the common case for Debug/Trace).
    std::atomic<int> lowestEnabled{static_cast<int>(LogLevel::Info)};

    void updateLowestEnabled() {
        LogLevel lowest = defaultLevel;
        for (const auto& entry : levels) lowest = std::min(lowest, entry.second);
        lowestEnabled.store(static_cast<int>(lowest), std::memory_order_relaxed);
    }

    LogRecord* find(std::uint64_t sequence) {
        if (records.empty() || sequence < records.front().sequence) return nullptr;
        const std::uint64_t index = sequence - records.front().sequence;
        return index < records.size() ? &records[static_cast<std::size_t>(index)] : nullptr;
    }

    void deliver(LogRecord record) {
        std::lock_guard lock(mutex);
        if (auto it = lastBySource.find(record.source); it != lastBySource.end()) {
            LogRecord* previous = find(it->second);
            if (previous && sameMessage(*previous, record) && record.time >= previous->lastTime &&
                record.time - previous->lastTime <= repeatWindow) {
                ++previous->repeatCount;
                previous->lastTime = record.time;
                for (const auto& sink : sinks) callSink("onRepeat", [&] { sink->onRepeat(*previous); });
                return;
            }
        }

        record.sequence = nextSequence++;
        lastBySource[record.source] = record.sequence;
        records.push_back(std::move(record));
        while (records.size() > capacity) records.pop_front();
        const LogRecord& added = records.back();
        for (const auto& sink : sinks) callSink("onRecord", [&] { sink->onRecord(added); });
    }

    void flushSinks() {
        std::lock_guard lock(mutex);
        for (const auto& sink : sinks) callSink("flush", [&] { sink->flush(); });
    }
};

LogHub& LogHub::instance() {
    static LogHub hub;
    return hub;
}

LogHub::LogHub() : m_impl(std::make_unique<Impl>()) {}

LogHub::~LogHub() = default;

void LogHub::post(LogRecord record) {
    if (!isEnabled(record.level, record.source, record.category)) return;

    if (record.time == std::chrono::system_clock::time_point()) record.time = std::chrono::system_clock::now();
    record.lastTime = record.time;
    record.repeatCount = 1;
    if (record.threadId == 0) record.threadId = currentThreadId();
    if (record.threadName.empty()) record.threadName = t_threadName;

    if (t_posting) {
        // From inside a sink: the hub's lock is held on this thread.
        if (!t_deliveringDeferred && t_deferred.size() < kMaxDeferredPosts) {
            t_deferred.emplace_back(this, std::move(record));
        }
        return;
    }

    t_posting = true;
    const bool fatal = record.level == LogLevel::Fatal;
    m_impl->deliver(std::move(record));
    if (!t_deferred.empty()) {
        t_deliveringDeferred = true;
        auto deferred = std::move(t_deferred);
        t_deferred.clear();
        for (auto& [hub, nested] : deferred) hub->m_impl->deliver(std::move(nested));
        t_deliveringDeferred = false;
    }
    if (fatal) m_impl->flushSinks();
    t_posting = false;
}

void LogHub::log(LogLevel level, std::string_view source, std::string_view category, std::string message) {
    LogRecord record;
    record.level = level;
    record.source = source;
    record.category = category;
    record.message = std::move(message);
    post(std::move(record));
}

bool LogHub::isEnabled(LogLevel level, std::string_view source, std::string_view category) const {
    if (level == LogLevel::Fatal) return true;
    if (static_cast<int>(level) < m_impl->lowestEnabled.load(std::memory_order_relaxed)) return false;

    std::lock_guard lock(m_impl->levelMutex);
    const std::pair<std::string, LogLevel>* best = nullptr;
    for (const auto& entry : m_impl->levels) {
        if (coversCategory(entry.first, category) && (!best || entry.first.size() > best->first.size())) {
            best = &entry;
        }
    }
    if (!best) {
        for (const auto& entry : m_impl->levels) {
            if (entry.first == source) best = &entry;
        }
    }
    return level >= (best ? best->second : m_impl->defaultLevel);
}

void LogHub::setDefaultLevel(LogLevel level) {
    std::lock_guard lock(m_impl->levelMutex);
    m_impl->defaultLevel = level;
    m_impl->updateLowestEnabled();
}

LogLevel LogHub::defaultLevel() const {
    std::lock_guard lock(m_impl->levelMutex);
    return m_impl->defaultLevel;
}

void LogHub::setLevel(std::string_view key, LogLevel level) {
    std::lock_guard lock(m_impl->levelMutex);
    auto it = std::find_if(m_impl->levels.begin(), m_impl->levels.end(),
                           [key](const auto& entry) { return entry.first == key; });
    if (it != m_impl->levels.end()) {
        it->second = level;
    } else {
        m_impl->levels.emplace_back(std::string(key), level);
    }
    m_impl->updateLowestEnabled();
}

void LogHub::clearLevels() {
    std::lock_guard lock(m_impl->levelMutex);
    m_impl->levels.clear();
    m_impl->updateLowestEnabled();
}

bool LogHub::configureLevels(std::string_view spec) {
    bool ok = true;
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view part = trimmed(spec.substr(0, comma));
        spec = comma == std::string_view::npos ? std::string_view() : spec.substr(comma + 1);
        if (part.empty()) continue;

        LogLevel level;
        const std::size_t equals = part.find('=');
        if (equals == std::string_view::npos) {
            if (parseLogLevel(part, &level)) {
                setDefaultLevel(level);
            } else {
                ok = false;
            }
            continue;
        }
        const std::string_view key = trimmed(part.substr(0, equals));
        if (!key.empty() && parseLogLevel(part.substr(equals + 1), &level)) {
            setLevel(key, level);
        } else {
            ok = false;
        }
    }
    return ok;
}

void LogHub::addSink(std::shared_ptr<LogSink> sink, bool replayBacklog) {
    if (!sink) return;
    std::lock_guard lock(m_impl->mutex);
    if (replayBacklog) {
        for (const LogRecord& record : m_impl->records) callSink("onRecord", [&] { sink->onRecord(record); });
    }
    m_impl->sinks.push_back(std::move(sink));
}

void LogHub::removeSink(const LogSink* sink) {
    std::lock_guard lock(m_impl->mutex);
    auto& sinks = m_impl->sinks;
    sinks.erase(std::remove_if(sinks.begin(), sinks.end(), [sink](const auto& s) { return s.get() == sink; }),
                sinks.end());
}

void LogHub::flush() { m_impl->flushSinks(); }

std::vector<LogRecord> LogHub::snapshot() const {
    std::lock_guard lock(m_impl->mutex);
    return {m_impl->records.begin(), m_impl->records.end()};
}

void LogHub::setCapacity(std::size_t capacity) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->capacity = std::max<std::size_t>(capacity, 1);
    while (m_impl->records.size() > m_impl->capacity) m_impl->records.pop_front();
}

std::size_t LogHub::capacity() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->capacity;
}

void LogHub::setRepeatWindow(std::chrono::milliseconds window) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->repeatWindow = window;
}

void setThreadName(std::string name) { t_threadName = std::move(name); }

const std::string& threadName() { return t_threadName; }

}  // namespace usdcc::core
