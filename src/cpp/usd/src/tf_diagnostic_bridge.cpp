#include "usdcc/usd/tf_diagnostic_bridge.h"

#include <pxr/base/tf/callContext.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>

#include <utility>

namespace usdcc::usd {

using usdcc::core::LogHub;
using usdcc::core::LogLevel;
using usdcc::core::LogRecord;

namespace {
constexpr const char* kSource = "usd";
}  // namespace

TfDiagnosticBridge::TfDiagnosticBridge() { PXR_NS::TfDiagnosticMgr::GetInstance().AddDelegate(this); }

TfDiagnosticBridge::~TfDiagnosticBridge() { PXR_NS::TfDiagnosticMgr::GetInstance().RemoveDelegate(this); }

LogRecord TfDiagnosticBridge::toRecord(const PXR_NS::TfDiagnosticBase& diagnostic, LogLevel level) {
    LogRecord record;
    record.level = diagnostic.GetQuiet() ? LogLevel::Debug : level;
    record.source = kSource;
    record.category = diagnostic.GetDiagnosticCodeAsString();
    record.message = diagnostic.GetCommentary();
    record.file = diagnostic.GetSourceFileName();
    record.line = static_cast<int>(diagnostic.GetSourceLineNumber());
    record.function = diagnostic.GetSourceFunction();
    return record;
}

void TfDiagnosticBridge::IssueStatus(const PXR_NS::TfStatus& status) {
    LogHub::instance().post(toRecord(status, LogLevel::Info));
}

void TfDiagnosticBridge::IssueWarning(const PXR_NS::TfWarning& warning) {
    LogHub::instance().post(toRecord(warning, LogLevel::Warning));
}

void TfDiagnosticBridge::IssueError(const PXR_NS::TfError& error) {
    LogHub::instance().post(toRecord(error, LogLevel::Error));
}

void TfDiagnosticBridge::IssueFatalError(const PXR_NS::TfCallContext& context, const std::string& message) {
    LogRecord record;
    record.level = LogLevel::Fatal;
    record.source = kSource;
    record.category = "TF_FATAL_ERROR";
    record.message = message;
    if (context) {
        record.file = context.GetFile();
        record.line = static_cast<int>(context.GetLine());
        record.function = context.GetFunction();
    }
    // A Fatal record makes the hub flush every sink before returning.
    LogHub::instance().post(std::move(record));
}

}  // namespace usdcc::usd
