#pragma once

#include "usdcc/core/export.h"
#include "usdcc/core/log.h"

#include <filesystem>
#include <memory>
#include <string>

namespace usdcc::core {

// Writes every record to a plain-text log file (docs/LOGGING.md §5): one line
// per record, with continuation lines and details indented beneath it.
// onRecord() only queues the line; a writer thread of its own writes it,
// flushing at least once a second and at once after an Error or Fatal, so the
// last messages before a crash are on disk. A run of collapsed repeats is
// written once, followed by a "(repeated N more times)" line when the run
// ends, on flush() and on destruction.
class FileLogSink : public LogSink {
public:
    // Creates `directory` if needed and opens a new file in it named
    // usdcc-<yyyyMMdd-HHmmss>-<pid>.log, first deleting the oldest such files
    // so that at most `keepFiles` remain, the new one included. Returns null
    // if the file can't be opened.
    USDCC_CORE_API static std::shared_ptr<FileLogSink> openInDirectory(const std::filesystem::path& directory,
                                                                       int keepFiles = 10);

    // Opens (truncating) `file`; check isOpen().
    USDCC_CORE_API explicit FileLogSink(const std::filesystem::path& file);
    // Writes what is still queued, then closes the file.
    USDCC_CORE_API ~FileLogSink() override;
    FileLogSink(const FileLogSink&) = delete;
    FileLogSink& operator=(const FileLogSink&) = delete;

    USDCC_CORE_API bool isOpen() const;
    USDCC_CORE_API const std::filesystem::path& path() const;

    USDCC_CORE_API void onRecord(const LogRecord& record) override;
    USDCC_CORE_API void onRepeat(const LogRecord& record) override;

    // Blocks until everything queued so far is written and flushed to disk.
    USDCC_CORE_API void flush() override;

    // The text written for `record`, ending in a newline (exposed for tests):
    //   2026-11-02 12:04:31.204 W [main] usd/TF_DIAGNOSTIC_WARNING_TYPE: message (file.cpp:42)
    USDCC_CORE_API static std::string formatRecord(const LogRecord& record);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace usdcc::core
