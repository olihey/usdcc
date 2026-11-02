#pragma once

namespace usdcc::usd {

// Debug > Emit Test Log Messages (docs/LOGGING.md §7): one message of every
// level from every source the log has a bridge for — usdcc (USDCC_LOG_*),
// Qt (qDebug ... qCritical), OpenUSD (TF_STATUS ... TF_RUNTIME_ERROR), native
// stdout/stderr (printf, fprintf, std::cout), the same from a worker thread,
// a multi-line message with details, and 500 identical warnings that must
// collapse into one row. All under category
// "usdcc.test" (or the source's own). Debug and Trace only show up when
// enabled with USDCC_LOG. Never Fatal: that would end the process.
void emitLogTestMessages();

// `count` distinct Info messages, posted from a worker thread as fast as it
// can, to check that the Log panel keeps up.
void emitLogBurst(int count);

}  // namespace usdcc::usd
