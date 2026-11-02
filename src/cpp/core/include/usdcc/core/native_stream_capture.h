#pragma once

#include "usdcc/core/export.h"

#include <memory>
#include <string_view>

namespace usdcc::core {

// Captures what is written to the process's stdout and stderr at the C level
// (docs/LOGGING.md §3.5): TF_DEBUG output, printf from render delegates and
// third-party libraries, std::cout, and later Python C extensions. File
// descriptors 1 and 2 (and on Windows the STD_OUTPUT/ERROR handles, for
// DLLs that write through those) become pipes; one reader thread per pipe
// splits what arrives into lines and posts each to LogHub::instance(), with
// source and category "stdout" (Info) or "stderr" (Warning). Empty lines are
// dropped; a last line without a newline is posted when capture ends.
//
// stdout and stderr are made unbuffered while captured, so a line shows up
// when it is written rather than when a buffer fills.
//
// While capturing, anything of ours meant for a terminal or the debugger
// (ConsoleLogSink, the hub's own error notes) must go through
// writeToOriginalStderr(): writing to stderr would come straight back as a
// record, and could block on a full pipe from the reader thread itself.
//
// One instance at a time; create it at the top of main(), before anything
// else can write. Destroying it restores the original streams and joins the
// readers. Turned off with the environment variable USDCC_NO_STREAM_CAPTURE.
class NativeStreamCapture {
public:
    USDCC_CORE_API NativeStreamCapture();
    USDCC_CORE_API ~NativeStreamCapture();
    NativeStreamCapture(const NativeStreamCapture&) = delete;
    NativeStreamCapture& operator=(const NativeStreamCapture&) = delete;

    // False when capture couldn't be set up (or another instance is active).
    USDCC_CORE_API bool isActive() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Writes to the stderr the process had before capture started (unchanged
// stderr when nothing is captured). Thread-safe.
USDCC_CORE_API void writeToOriginalStderr(std::string_view text);

}  // namespace usdcc::core
