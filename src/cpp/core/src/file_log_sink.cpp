#include "usdcc/core/file_log_sink.h"

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace usdcc::core {

namespace {
constexpr std::chrono::seconds kFlushInterval{1};

std::tm localTime(std::time_t time) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    return tm;
}

// "2026-11-02 12:04:31.204"
std::string formatTime(std::chrono::system_clock::time_point time) {
    const std::tm tm = localTime(std::chrono::system_clock::to_time_t(time));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count() % 1000;
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

char levelLetter(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return 'T';
        case LogLevel::Debug: return 'D';
        case LogLevel::Info: return 'I';
        case LogLevel::Warning: return 'W';
        case LogLevel::Error: return 'E';
        case LogLevel::Fatal: return 'F';
    }
    return 'I';
}

// Each line of `text` after the first, and every line of a details block,
// starts with this.
constexpr std::string_view kIndent = "    ";

void appendIndented(std::string& out, std::string_view text) {
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        out += kIndent;
        out += text.substr(0, newline);
        out += '\n';
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
    }
}

std::string fileName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// "<time> <L> [<thread>] <source>/<category>: " — the start of every line.
std::string linePrefix(const LogRecord& record, std::chrono::system_clock::time_point time) {
    std::string prefix = formatTime(time);
    prefix += ' ';
    prefix += levelLetter(record.level);
    prefix += ' ';
    if (!record.threadName.empty()) {
        prefix += std::format("[{}] ", record.threadName);
    } else {
        prefix += std::format("[{}] ", record.threadId);
    }
    prefix += record.source;
    if (!record.category.empty()) {
        prefix += '/';
        prefix += record.category;
    }
    prefix += ": ";
    return prefix;
}

int processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}
}  // namespace

struct FileLogSink::Impl {
    std::filesystem::path path;
    std::ofstream out;

    std::mutex mutex;  // everything below
    std::condition_variable wake;      // for the writer: lines queued, flush requested or stopping
    std::condition_variable flushed;   // for flush(): flushedUpTo advanced
    std::vector<std::string> queue;
    bool urgent = false;               // an Error/Fatal is queued: flush right after writing it
    bool stopping = false;
    std::uint64_t queuedUpTo = 0;      // lines ever queued
    std::uint64_t flushRequestedUpTo = 0;
    std::uint64_t flushedUpTo = 0;     // lines written and flushed

    // Per source: the record whose repeats haven't all been reported yet.
    struct Run {
        LogRecord record;            // copy of the collapsed record, repeatCount as last seen
        std::uint32_t reported = 1;  // occurrences already in the file
    };
    std::unordered_map<std::string, Run> runs;

    std::thread writer;

    void enqueue(std::string line, bool flushSoon) {
        queue.push_back(std::move(line));
        ++queuedUpTo;
        urgent = urgent || flushSoon;
        wake.notify_one();
    }

    // Writes "(repeated N more times)" for a run with unreported repeats.
    void reportRepeats(Run& run) {
        if (run.record.repeatCount <= run.reported) return;
        const std::uint32_t more = run.record.repeatCount - run.reported;
        enqueue(linePrefix(run.record, run.record.lastTime) +
                    std::format("(previous message repeated {} more time{})\n", more, more == 1 ? "" : "s"),
                false);
        run.reported = run.record.repeatCount;
    }

    void writeLoop() {
        auto lastFlush = std::chrono::steady_clock::now();
        bool dirty = false;
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait_for(lock, kFlushInterval,
                          [this] { return stopping || !queue.empty() || flushRequestedUpTo > flushedUpTo; });
            std::vector<std::string> lines = std::move(queue);
            queue.clear();
            const std::uint64_t upTo = queuedUpTo;
            const bool flushNow = urgent || stopping || flushRequestedUpTo > flushedUpTo;
            urgent = false;
            const bool stop = stopping;
            lock.unlock();

            for (const std::string& line : lines) out << line;
            dirty = dirty || !lines.empty();
            const auto now = std::chrono::steady_clock::now();
            const bool flushedToDisk = dirty && (flushNow || now - lastFlush >= kFlushInterval);
            if (flushedToDisk) {
                out.flush();
                lastFlush = now;
                dirty = false;
            }

            lock.lock();
            if (flushedToDisk || !dirty) {
                flushedUpTo = upTo;
                flushed.notify_all();
            }
            if (stop && queue.empty()) return;
        }
    }
};

std::shared_ptr<FileLogSink> FileLogSink::openInDirectory(const std::filesystem::path& directory, int keepFiles) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);

    // The names sort by start time, so the oldest come first.
    std::vector<std::filesystem::path> existing;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(error) && name.starts_with("usdcc-") && name.ends_with(".log")) {
            existing.push_back(entry.path());
        }
    }
    std::sort(existing.begin(), existing.end());
    const std::size_t keepOld = static_cast<std::size_t>(std::max(keepFiles - 1, 0));
    for (std::size_t i = 0; i + keepOld < existing.size(); ++i) {
        std::filesystem::remove(existing[i], error);
    }

    const std::tm tm = localTime(std::time(nullptr));
    const std::string name = std::format("usdcc-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.log", tm.tm_year + 1900,
                                         tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, processId());
    auto sink = std::make_shared<FileLogSink>(directory / name);
    return sink->isOpen() ? sink : nullptr;
}

FileLogSink::FileLogSink(const std::filesystem::path& file) : m_impl(std::make_unique<Impl>()) {
    m_impl->path = file;
    // Binary: '\n' stays '\n' on every platform.
    m_impl->out.open(file, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!m_impl->out) return;
    m_impl->out << std::format("{} usdcc log started (pid {})\n", formatTime(std::chrono::system_clock::now()),
                               processId());
    m_impl->writer = std::thread([impl = m_impl.get()] { impl->writeLoop(); });
}

FileLogSink::~FileLogSink() {
    if (!m_impl->writer.joinable()) return;
    {
        std::lock_guard lock(m_impl->mutex);
        for (auto& [source, run] : m_impl->runs) m_impl->reportRepeats(run);
        m_impl->stopping = true;
        m_impl->wake.notify_one();
    }
    m_impl->writer.join();
}

bool FileLogSink::isOpen() const { return m_impl->writer.joinable(); }

const std::filesystem::path& FileLogSink::path() const { return m_impl->path; }

void FileLogSink::onRecord(const LogRecord& record) {
    if (!isOpen()) return;
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->runs.find(record.source);
    if (it != m_impl->runs.end()) {
        // A new record from this source ends its previous run.
        m_impl->reportRepeats(it->second);
        it->second = {record, record.repeatCount};
    } else {
        m_impl->runs.emplace(record.source, Impl::Run{record, record.repeatCount});
    }
    m_impl->enqueue(formatRecord(record), record.level >= LogLevel::Error);
}

void FileLogSink::onRepeat(const LogRecord& record) {
    if (!isOpen()) return;
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->runs.find(record.source);
    if (it != m_impl->runs.end() && it->second.record.sequence == record.sequence) {
        it->second.record.repeatCount = record.repeatCount;
        it->second.record.lastTime = record.lastTime;
    }
}

void FileLogSink::flush() {
    if (!isOpen()) return;
    std::unique_lock lock(m_impl->mutex);
    for (auto& [source, run] : m_impl->runs) m_impl->reportRepeats(run);
    const std::uint64_t target = m_impl->queuedUpTo;
    m_impl->flushRequestedUpTo = std::max(m_impl->flushRequestedUpTo, target);
    m_impl->wake.notify_one();
    m_impl->flushed.wait(lock, [this, target] { return m_impl->flushedUpTo >= target; });
}

std::string FileLogSink::formatRecord(const LogRecord& record) {
    std::string text = linePrefix(record, record.time);
    const std::string_view message = record.message;
    const std::size_t newline = message.find('\n');
    text += message.substr(0, newline);
    if (!record.file.empty()) {
        text += std::format(" ({}:{})", fileName(record.file), record.line);
    }
    text += '\n';
    if (newline != std::string_view::npos) appendIndented(text, message.substr(newline + 1));
    if (!record.details.empty()) appendIndented(text, record.details);
    return text;
}

}  // namespace usdcc::core
