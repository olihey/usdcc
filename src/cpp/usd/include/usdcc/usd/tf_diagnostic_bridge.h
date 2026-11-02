#pragma once

#include "usdcc/core/log.h"

#include <pxr/base/tf/diagnosticMgr.h>

namespace usdcc::usd {

// Routes OpenUSD's diagnostics into the usdcc log (docs/LOGGING.md §3.3):
// every TF_STATUS, TF_WARN, TF_CODING_ERROR, TF_RUNTIME_ERROR, TF_ERROR and
// TF_FATAL_ERROR, from C++ and from pxr Python, becomes a record with source
// "usd", the diagnostic code as its category (TF_DIAGNOSTIC_WARNING_TYPE,
// ...) and the call site's file, line and function.
//
// Registers itself with TfDiagnosticMgr for as long as it exists; create one
// at the top of main(). While registered, Tf no longer prints these to stderr
// (ConsoleLogSink does that instead). Errors inside a TfErrorMark reach it
// only if they are still there when the outermost mark goes away, not when
// the code clears them (it handled them). "Quiet" diagnostics, which Tf would
// not have printed, come in at Debug level.
//
// The Issue* methods run on whichever thread posted and only build a record.
// Tf aborts by itself after IssueFatalError(); the hub flushes the log first.
class TfDiagnosticBridge : public PXR_NS::TfDiagnosticMgr::Delegate {
public:
    TfDiagnosticBridge();
    ~TfDiagnosticBridge() override;
    TfDiagnosticBridge(const TfDiagnosticBridge&) = delete;
    TfDiagnosticBridge& operator=(const TfDiagnosticBridge&) = delete;

    void IssueStatus(const PXR_NS::TfStatus& status) override;
    void IssueWarning(const PXR_NS::TfWarning& warning) override;
    void IssueError(const PXR_NS::TfError& error) override;
    void IssueFatalError(const PXR_NS::TfCallContext& context, const std::string& message) override;

    // The record for `diagnostic` at `level` (lowered to Debug if it is quiet).
    static usdcc::core::LogRecord toRecord(const PXR_NS::TfDiagnosticBase& diagnostic, usdcc::core::LogLevel level);
};

}  // namespace usdcc::usd
