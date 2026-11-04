// Toast notifications (docs/NOTIFICATIONS.md) in a real MainWindow: stacking
// in each corner of the window's content area, sizes from the text, timed and
// sticky toasts, pausing, repeats, the queue beyond the visible limit,
// dismissing, following the window, the settings, and log records marked as
// toasts arriving from another thread with a Show Log button. Runs in
// usdcc_ui_tests (offscreen); settings stay in memory (test_settings.h).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/toast_log_sink.h"
#include "usdcc/ui/toast_manager.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QPushButton>
#include <QToolButton>

#include <functional>
#include <string>
#include <thread>

using usdcc::core::Settings;
using usdcc::core::ToastKind;
using usdcc::core::ToastRequest;
using usdcc::ui::MainWindow;
using usdcc::ui::ToastManager;
using usdcc::ui::ToastWidget;

namespace {

void wait(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

bool waitFor(const std::function<bool()>& done, int timeoutMs = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) wait(5);
    return done();
}

// A main window that is up, with its layout settled.
struct Window {
    TestSettings settings;
    MainWindow window;
    Window() {
        window.restoreLayout();
        window.resize(1200, 800);
        window.show();
        waitFor([this]() { return window.isLayoutSettled(); }, 3000);
    }
    ToastManager& toasts() { return *window.toasts(); }
};

ToastRequest request(ToastKind kind, const std::string& title, const std::string& body = {}, int durationMs = 0) {
    return {.kind = kind, .title = title, .body = body, .durationMs = durationMs};
}

}  // namespace

USDCC_TEST(toast_stacking_and_corners) {
    Window w;
    ToastManager& toasts = w.toasts();
    const int first = toasts.show(request(ToastKind::Info, "One", "short"));
    const int second = toasts.show(request(ToastKind::Success, "Two", "line one\nline two\nline three\nline four"));
    wait(300);
    ToastWidget* one = toasts.toast(first);
    ToastWidget* two = toasts.toast(second);
    CHECK(one && two && first != second, "two toasts are up");
    if (!one || !two) return;
    CHECK(toasts.visibleToasts() == QList<ToastWidget*>({two, one}), "newest first");
    CHECK(one->isVisible() && two->isVisible() && one->isWindow(), "as windows of their own");
    CHECK(two->height() > one->height(), "a longer body makes a taller toast (%d vs %d)", two->height(),
          one->height());
    CHECK(one->width() >= usdcc::ui::scaled::pixels(ToastWidget::minimumWidth) &&
              two->width() <= usdcc::ui::scaled::pixels(ToastWidget::maximumWidth),
          "widths within the limits (%d, %d)", one->width(), two->width());
    CHECK(one->property("kind").toString() == QStringLiteral("info") &&
              usdcc::ui::theme::toastColor(ToastKind::Error) == QColor(0xff, 0x5f, 0x56) &&
              usdcc::ui::theme::toastColor(ToastKind::Info) == usdcc::ui::theme::accent(),
          "kinds and their colours");

    const QRect area = toasts.area();
    const QRect windowRect(w.window.mapToGlobal(QPoint(0, 0)), w.window.size());
    CHECK(windowRect.contains(area) && area.top() > windowRect.top(), "the area is inside the window, below the title bar");
    const int margin = usdcc::ui::scaled::pixels(12);
    const auto at = [](ToastWidget* toast) { return toast->geometry(); };
    CHECK(at(two).right() == area.right() - margin && at(two).bottom() == area.bottom() - margin,
          "bottom right: the newest in the corner (%d,%d vs %d,%d)", at(two).right(), at(two).bottom(),
          area.right() - margin, area.bottom() - margin);
    CHECK(at(one).bottom() < at(two).top() && at(one).right() == at(two).right(), "the older one above it");

    auto* dismissAll = w.window.findChild<QToolButton*>(QStringLiteral("toastDismissAll"));
    CHECK(dismissAll && dismissAll->isVisible() && dismissAll->geometry().bottom() < at(one).top(),
          "Dismiss All follows the stack");

    auto& settings = Settings::instance();
    settings.setValue(ToastManager::positionKey, std::string("Top left"));
    wait(300);
    CHECK(toasts.corner() == ToastManager::Corner::TopLeft, "the corner follows the setting");
    CHECK(at(two).left() == area.left() + margin && at(two).top() == area.top() + margin,
          "top left: the newest in the corner");
    CHECK(at(one).top() > at(two).bottom(), "the older one below it");
    settings.setValue(ToastManager::positionKey, std::string("Top right"));
    wait(300);
    CHECK(at(two).right() == area.right() - margin && at(two).top() == area.top() + margin, "top right");
    settings.setValue(ToastManager::positionKey, std::string("Bottom left"));
    wait(300);
    CHECK(at(two).left() == area.left() + margin && at(two).bottom() == area.bottom() - margin, "bottom left");

    // Moving the window moves the stack with it, at once.
    const QPoint before = two->pos();
    w.window.move(w.window.pos() + QPoint(40, 25));
    wait(50);
    CHECK(two->pos() == before + QPoint(40, 25), "the stack follows the window (%d,%d)", two->pos().x() - before.x(),
          two->pos().y() - before.y());

    toasts.dismissAll();
    CHECK(waitFor([&toasts, first]() { return toasts.visibleToasts().isEmpty() && !toasts.toast(first); }),
          "Dismiss All closes them");
    CHECK(dismissAll && !dismissAll->isVisible(), "and hides itself");
}

USDCC_TEST(toast_timing) {
    Window w;
    ToastManager& toasts = w.toasts();
    const int timed = toasts.show(request(ToastKind::Info, "Timed", {}, 250));
    const int sticky = toasts.show(request(ToastKind::Error, "Sticky", {}, ToastRequest::kSticky));
    CHECK(toasts.toast(timed) && !toasts.toast(timed)->isSticky() && toasts.toast(sticky)->isSticky(),
          "a timed and a sticky toast");
    CHECK(waitFor([&]() { return !toasts.toast(timed); }), "the timed one goes by itself");
    wait(300);
    CHECK(toasts.toast(sticky) != nullptr, "the sticky one stays");

    // Hovering stops the countdown; leaving starts it again from where it was.
    const int hovered = toasts.show(request(ToastKind::Warning, "Hovered", {}, 300));
    wait(50);
    ToastWidget* toast = toasts.toast(hovered);
    if (!toast) return;
    QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
    QApplication::sendEvent(toast, &enter);
    CHECK(toast->isPaused(), "the pointer over it pauses it");
    const int left = toast->remainingMs();
    wait(450);
    CHECK(toasts.toast(hovered) && toast->remainingMs() == left, "nothing runs out while paused (%d ms left)", left);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(toast, &leave);
    CHECK(!toast->isPaused(), "leaving resumes it");
    CHECK(waitFor([&]() { return !toasts.toast(hovered); }), "and then it goes");

    // The default durations come from the settings.
    auto& settings = Settings::instance();
    CHECK(ToastManager::defaultDurationMs(ToastKind::Info) == 5000 &&
              ToastManager::defaultDurationMs(ToastKind::Warning) == 8000 &&
              ToastManager::defaultDurationMs(ToastKind::Error) == ToastRequest::kSticky,
          "5 s, 8 s for warnings, errors sticky");
    settings.setValue(ToastManager::durationKey, 2.5);
    settings.setValue(ToastManager::errorsStickyKey, false);
    CHECK(ToastManager::defaultDurationMs(ToastKind::Success) == 2500 &&
              ToastManager::defaultDurationMs(ToastKind::Error) == 8000,
          "which can be changed");
    const int byDefault = toasts.show({.kind = ToastKind::Success, .title = "Default"});
    CHECK(toasts.toast(byDefault) && toasts.toast(byDefault)->durationMs() == 2500, "a toast takes its kind's");
    toasts.dismissAll();
}

USDCC_TEST(toast_repeats_and_queue) {
    Window w;
    ToastManager& toasts = w.toasts();
    const int first = toasts.show(request(ToastKind::Info, "Again", "and again"));
    const int again = toasts.show(request(ToastKind::Info, "Again", "and again"));
    CHECK(first == again && toasts.visibleToasts().size() == 1, "the same toast isn't stacked twice");
    CHECK(toasts.toast(first) && toasts.toast(first)->repeatCount() == 2, "it's counted instead");
    toasts.dismissAll();
    waitFor([&]() { return toasts.visibleToasts().isEmpty(); });

    Settings::instance().setValue(ToastManager::maxVisibleKey, 2);
    const int a = toasts.show(request(ToastKind::Info, "A"));
    toasts.show(request(ToastKind::Info, "B"));
    const int c = toasts.show(request(ToastKind::Info, "C"));
    CHECK(toasts.visibleToasts().size() == 2 && toasts.queuedCount() == 1 && !toasts.toast(c),
          "beyond the limit, toasts wait (%lld up, %d queued)", static_cast<long long>(toasts.visibleToasts().size()),
          toasts.queuedCount());
    toasts.dismiss(a);
    CHECK(waitFor([&]() { return toasts.toast(c) != nullptr; }), "and appear as others go");
    CHECK(toasts.queuedCount() == 0 && toasts.visibleToasts().size() == 2, "the queue is empty again");

    const int queued = toasts.show(request(ToastKind::Info, "D"));
    toasts.dismiss(queued);
    CHECK(toasts.queuedCount() == 0, "a queued toast can be dismissed before it shows");
    toasts.dismissAll();
}

USDCC_TEST(toast_from_the_log) {
    Window w;
    ToastManager& toasts = w.toasts();
    w.window.logPanel()->hide();

    // From another thread, as any code may log.
    std::thread worker([]() {
        USDCC_LOG_TOAST(usdcc::core::LogLevel::Warning, "usdcc.test", "Disk {} is almost full", "C:");
    });
    worker.join();
    CHECK(waitFor([&]() { return toasts.visibleToasts().size() == 1; }), "a toast record from a worker shows");
    if (toasts.visibleToasts().size() != 1) return;
    ToastWidget* toast = toasts.visibleToasts().front();
    CHECK(toast->kind() == ToastKind::Warning && toast->title() == QStringLiteral("Warning") &&
              toast->body() == QStringLiteral("Disk C: is almost full"),
          "kind and title from the level, the message as its body (%s / %s)", qPrintable(toast->title()),
          qPrintable(toast->body()));

    USDCC_LOG_WARNING("usdcc.test", "a plain warning");
    wait(100);
    CHECK(toasts.visibleToasts().size() == 1, "an unmarked record shows no toast");

    QPushButton* showLog = nullptr;
    for (QPushButton* button : toast->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Show Log")) showLog = button;
    }
    CHECK(showLog != nullptr, "a warning toast has Show Log");
    if (showLog) showLog->click();
    CHECK(w.window.logPanel()->isVisible(), "which opens the Log panel");
    CHECK(waitFor([&]() { return toasts.visibleToasts().isEmpty(); }), "and dismisses the toast");

    usdcc::core::LogRecord record;
    record.level = usdcc::core::LogLevel::Error;
    record.message = "Export failed";
    record.details = "Traceback...";
    record.toast = ToastRequest{.kind = ToastKind::Success, .title = "Custom"};
    const auto custom = usdcc::ui::ToastLogSink::requestFor(record);
    CHECK(custom && custom->kind == ToastKind::Success && custom->title == "Custom" &&
              custom->body == "Export failed\n\nTraceback...",
          "a request's own kind and title win; the details follow the message");
    record.toast = ToastRequest{};
    const auto derived = usdcc::ui::ToastLogSink::requestFor(record);
    CHECK(derived && derived->kind == ToastKind::Error && derived->title == "Error", "an error's kind and title");
    record.toast.reset();
    CHECK(!usdcc::ui::ToastLogSink::requestFor(record), "no request for an unmarked record");
}
