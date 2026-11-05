#include "usdcc/ui/toast_log_sink.h"

#include "usdcc/ui/toast_manager.h"

#include <QCoreApplication>

#include <utility>

namespace usdcc::ui {

namespace {

using core::LogLevel;
using core::ToastKind;

ToastKind kindFor(LogLevel level) {
    switch (level) {
        case LogLevel::Error:
        case LogLevel::Fatal: return ToastKind::Error;
        case LogLevel::Warning: return ToastKind::Warning;
        default: return ToastKind::Info;
    }
}

std::string titleFor(LogLevel level) {
    std::string name(core::logLevelName(level));
    if (!name.empty()) name[0] = char(name[0] - 'a' + 'A');
    return name;
}

}  // namespace

ToastLogSink::ToastLogSink(ToastManager* toasts, std::function<void()> showLog)
    : m_toasts(toasts), m_showLog(std::move(showLog)) {}

std::optional<core::ToastRequest> ToastLogSink::requestFor(const core::LogRecord& record) {
    if (!record.toast) return std::nullopt;
    core::ToastRequest request = *record.toast;
    if (!request.kind) request.kind = kindFor(record.level);
    if (request.title.empty()) request.title = titleFor(record.level);
    if (request.body.empty()) {
        request.body = record.message;
        if (!record.details.empty()) request.body += "\n\n" + record.details;
    }
    return request;
}

void ToastLogSink::onRecord(const core::LogRecord& record) {
    const std::optional<core::ToastRequest> request = requestFor(record);
    if (!request || !m_toasts) return;
    QList<ToastAction> actions;
    if (m_showLog && (*request->kind == ToastKind::Warning || *request->kind == ToastKind::Error)) {
        actions.append({QCoreApplication::translate("usdcc::ui::ToastLogSink", "Show Log"), m_showLog});
    }
    // Any thread, hub lock held: post() only queues it for the UI thread.
    m_toasts->post(*request, actions);
}

void ToastLogSink::onRepeat(const core::LogRecord& record) {
    // Shown again: the toast that is up counts it.
    onRecord(record);
}

}  // namespace usdcc::ui
