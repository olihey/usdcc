#pragma once

// What a toast notification shows (docs/NOTIFICATIONS.md): a small card in a
// corner of the main window, stacked with others, that disappears after a
// while or stays until dismissed. Qt-free, so non-UI code can ask for one by
// marking a log record (LogRecord::toast, USDCC_LOG_TOAST); the UI
// (usdcc::ui::ToastManager) shows it.

#include <optional>
#include <string>
#include <string_view>

namespace usdcc::core {

enum class ToastKind { Info, Success, Warning, Error };

struct ToastRequest {
    // For a log record, unset means: from its level (Error/Fatal: Error,
    // Warning: Warning, else Info). Shown directly, unset means Info.
    std::optional<ToastKind> kind;
    // For a log record, empty means the level's name ("Warning").
    std::string title;
    // Multiline. For a log record, empty means its message (and details).
    std::string body;
    // Milliseconds; kDefaultDuration = the kind's default (a setting),
    // kSticky = until dismissed.
    int durationMs = -1;

    static constexpr int kDefaultDuration = -1;
    static constexpr int kSticky = 0;

    static ToastRequest sticky() {
        ToastRequest request;
        request.durationMs = kSticky;
        return request;
    }
};

// "info", "success", "warning", "error".
inline std::string_view toastKindName(ToastKind kind) {
    switch (kind) {
        case ToastKind::Info: return "info";
        case ToastKind::Success: return "success";
        case ToastKind::Warning: return "warning";
        case ToastKind::Error: return "error";
    }
    return "info";
}

// The names above; false if `text` is none of them.
inline bool parseToastKind(std::string_view text, ToastKind* kind) {
    for (ToastKind candidate : {ToastKind::Info, ToastKind::Success, ToastKind::Warning, ToastKind::Error}) {
        if (toastKindName(candidate) == text) {
            *kind = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace usdcc::core
