#pragma once

#include "usdcc/core/log.h"
#include "usdcc/core/toast.h"

#include <functional>
#include <optional>

namespace usdcc::ui {

class ToastManager;

// Shows the log records marked as toasts (LogRecord::toast, USDCC_LOG_TOAST,
// usdcc.core.log(..., toast=True)) with a ToastManager (docs/NOTIFICATIONS.md).
// Records arrive on any thread, with the hub's lock held: they are only
// queued here (ToastManager::post()) and shown on the UI thread. A repeated
// record counts on the toast that is up ("×2").
//
// The toast's kind defaults to the level's, its title to the level's name and
// its body to the message (and details). Warnings and errors get a "Show Log"
// button when `showLog` is given.
//
// Remove the sink from its hub before `toasts` is destroyed.
class ToastLogSink : public core::LogSink {
public:
    explicit ToastLogSink(ToastManager* toasts, std::function<void()> showLog = {});

    void onRecord(const core::LogRecord& record) override;
    void onRepeat(const core::LogRecord& record) override;

    // What a record shows as (nothing for one not marked as a toast).
    static std::optional<core::ToastRequest> requestFor(const core::LogRecord& record);

private:
    ToastManager* m_toasts;
    std::function<void()> m_showLog;
};

}  // namespace usdcc::ui
