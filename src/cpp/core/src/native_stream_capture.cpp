#include "usdcc/core/native_stream_capture.h"

#include "usdcc/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace usdcc::core {

namespace {
// A line longer than this without a newline is posted in pieces.
constexpr std::size_t kMaxLineLength = 64 * 1024;
// How long the destructor lets a reader finish on its own before cancelling
// its read (e.g. a child process still holds the pipe's write end).
constexpr std::chrono::milliseconds kReaderExitTimeout{1000};

#if defined(_WIN32)
int sysDup(int fd) { return _dup(fd); }
int sysDup2(int from, int to) { return _dup2(from, to); }
int sysClose(int fd) { return _close(fd); }
int sysRead(int fd, char* buffer, unsigned size) { return _read(fd, buffer, size); }
int sysWrite(int fd, const char* data, unsigned size) { return _write(fd, data, size); }
int sysFileno(FILE* file) { return _fileno(file); }
// Not inherited: a child process holding the write end would keep the pipe
// open after capture ends.
bool makePipe(int fds[2]) { return _pipe(fds, 64 * 1024, _O_BINARY | _O_NOINHERIT) == 0; }
bool disabledByEnvironment() {
    std::size_t length = 0;
    getenv_s(&length, nullptr, 0, "USDCC_NO_STREAM_CAPTURE");
    return length > 1;  // set and not empty
}
#else
int sysDup(int fd) { return dup(fd); }
int sysDup2(int from, int to) { return dup2(from, to) < 0 ? -1 : 0; }
int sysClose(int fd) { return close(fd); }
int sysRead(int fd, char* buffer, unsigned size) { return static_cast<int>(read(fd, buffer, size)); }
int sysWrite(int fd, const char* data, unsigned size) { return static_cast<int>(write(fd, data, size)); }
int sysFileno(FILE* file) { return fileno(file); }
bool makePipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}
bool disabledByEnvironment() {
    const char* value = std::getenv("USDCC_NO_STREAM_CAPTURE");
    return value && *value;
}
#endif

// The stderr writeToOriginalStderr() writes to while capturing, else -1.
std::mutex g_originalMutex;
int g_originalStderr = -1;
// Only one capture at a time.
std::atomic<bool> g_captureActive{false};

void writeAll(int fd, std::string_view text) {
    while (!text.empty()) {
        const int written = sysWrite(fd, text.data(), static_cast<unsigned>(text.size()));
        if (written <= 0) return;
        text.remove_prefix(static_cast<std::size_t>(written));
    }
}
}  // namespace

void writeToOriginalStderr(std::string_view text) {
    std::lock_guard lock(g_originalMutex);
    if (g_originalStderr >= 0) {
        writeAll(g_originalStderr, text);
    } else {
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fflush(stderr);
    }
}

struct NativeStreamCapture::Impl {
    struct Stream {
        FILE* file;
        const char* name;  // also the records' source and category
        LogLevel level;
#if defined(_WIN32)
        DWORD stdHandle;
#endif
        int fd = -1;       // 1 or 2, normally
        int savedFd = -1;  // the original stream, to restore
        int readFd = -1;
        std::thread reader;
        std::atomic<bool> done{false};
        std::atomic<bool> stop{false};  // POSIX readers poll for this
    };

#if defined(_WIN32)
    Stream out{stdout, "stdout", LogLevel::Info, STD_OUTPUT_HANDLE};
    Stream err{stderr, "stderr", LogLevel::Warning, STD_ERROR_HANDLE};
#else
    Stream out{stdout, "stdout", LogLevel::Info};
    Stream err{stderr, "stderr", LogLevel::Warning};
#endif
    bool active = false;

    static void post(const Stream& stream, std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        LogRecord record;
        record.level = stream.level;
        record.source = stream.name;
        record.category = stream.name;
        record.message = std::move(line);
        LogHub::instance().post(std::move(record));
    }

    // Posts every complete line in `pending` (all of it when `final`).
    static void postLines(const Stream& stream, std::string& pending, bool final) {
        std::size_t start = 0;
        for (std::size_t newline = pending.find('\n'); newline != std::string::npos;
             newline = pending.find('\n', start)) {
            post(stream, pending.substr(start, newline - start));
            start = newline + 1;
        }
        pending.erase(0, start);
        if (final || pending.size() > kMaxLineLength) {
            post(stream, std::move(pending));
            pending.clear();
        }
    }

    static void readLoop(Stream& stream) {
        setThreadName(std::string(stream.name) + " capture");
        std::string pending;
        char buffer[4096];
        for (;;) {
#if !defined(_WIN32)
            pollfd poller{stream.readFd, POLLIN, 0};
            const int ready = poll(&poller, 1, 100);
            if (ready == 0) {
                if (stream.stop) break;
                continue;
            }
#endif
            const int count = sysRead(stream.readFd, buffer, sizeof(buffer));
            if (count <= 0) break;  // every write end closed, or the read was cancelled
            pending.append(buffer, static_cast<std::size_t>(count));
            postLines(stream, pending, false);
        }
        postLines(stream, pending, true);
        sysClose(stream.readFd);
        stream.done = true;
    }

    bool install(Stream& stream) {
        std::fflush(stream.file);
#if defined(_WIN32)
        // A GUI process started without a console has no stdout/stderr at all;
        // give the stream a descriptor (on NUL) to redirect first.
        if (_fileno(stream.file) < 0) {
            FILE* reopened = nullptr;
            freopen_s(&reopened, "NUL", "w", stream.file);
        }
#endif
        stream.fd = sysFileno(stream.file);
        if (stream.fd < 0) return false;

        int fds[2];
        if (!makePipe(fds)) return false;
        stream.savedFd = sysDup(stream.fd);
        if (stream.savedFd < 0 || sysDup2(fds[1], stream.fd) != 0) {
            if (stream.savedFd >= 0) sysClose(stream.savedFd);
            sysClose(fds[0]);
            sysClose(fds[1]);
            return false;
        }
        sysClose(fds[1]);  // the stream's descriptor holds the write end now
#if defined(_WIN32)
        // DLLs with a CRT of their own write through the standard handles.
        SetStdHandle(stream.stdHandle, reinterpret_cast<HANDLE>(_get_osfhandle(stream.fd)));
#endif
        std::setvbuf(stream.file, nullptr, _IONBF, 0);
        stream.readFd = fds[0];
        stream.reader = std::thread([&stream] { readLoop(stream); });
        return true;
    }

    // Puts the original stream back; the reader sees end-of-file.
    void restore(Stream& stream) {
        std::fflush(stream.file);
        sysDup2(stream.savedFd, stream.fd);
#if defined(_WIN32)
        SetStdHandle(stream.stdHandle, reinterpret_cast<HANDLE>(_get_osfhandle(stream.fd)));
#endif
    }

    void finish(Stream& stream) {
        const auto deadline = std::chrono::steady_clock::now() + kReaderExitTimeout;
        while (!stream.done && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!stream.done) {
#if defined(_WIN32)
            CancelSynchronousIo(stream.reader.native_handle());
#else
            stream.stop = true;
#endif
        }
        stream.reader.join();
        sysClose(stream.savedFd);
    }
};

NativeStreamCapture::NativeStreamCapture() : m_impl(std::make_unique<Impl>()) {
    if (disabledByEnvironment()) return;
    bool expected = false;
    if (!g_captureActive.compare_exchange_strong(expected, true)) return;

#if defined(_WIN32)
    // Without a console stdin has no descriptor either; give it 0 so that
    // stdout and stderr, if they need one too, get 1 and 2.
    if (_fileno(stdin) < 0) {
        FILE* reopened = nullptr;
        freopen_s(&reopened, "NUL", "r", stdin);
    }
#endif
    if (!m_impl->install(m_impl->out)) {
        g_captureActive = false;
        return;
    }
    if (!m_impl->install(m_impl->err)) {
        m_impl->restore(m_impl->out);
        m_impl->finish(m_impl->out);
        g_captureActive = false;
        return;
    }
    {
        std::lock_guard lock(g_originalMutex);
        g_originalStderr = m_impl->err.savedFd;
    }
    m_impl->active = true;
}

NativeStreamCapture::~NativeStreamCapture() {
    if (!m_impl->active) return;
    m_impl->restore(m_impl->out);
    m_impl->restore(m_impl->err);
    {
        // From here on stderr is the original again.
        std::lock_guard lock(g_originalMutex);
        g_originalStderr = -1;
    }
    m_impl->finish(m_impl->out);
    m_impl->finish(m_impl->err);
    g_captureActive = false;
}

bool NativeStreamCapture::isActive() const { return m_impl->active; }

}  // namespace usdcc::core
