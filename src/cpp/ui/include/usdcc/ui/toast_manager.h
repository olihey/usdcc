#pragma once

#include "usdcc/core/toast.h"
#include "usdcc/ui/toast.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>

#include <deque>

class QToolButton;
class QWidget;

namespace usdcc::ui {

// The toast notifications of one window (docs/NOTIFICATIONS.md): shows them
// stacked in a corner of `window` (the Notifications/position setting), the
// newest nearest the corner, and keeps them there while the window moves,
// resizes or changes its toolbars.
//
// - A toast with the same kind, title and body as one that is up isn't
//   stacked again: that one counts it ("×2") and starts its time again.
// - At most Notifications/maxVisible are up; more wait in a queue and appear
//   as others go. Toasts shown before the window is visible wait for it too.
// - From two up, a "Dismiss all" button follows the stack.
//
// MainWindow owns one (MainWindow::toasts()); usable on any QWidget window.
class ToastManager : public QObject {
    Q_OBJECT
public:
    enum class Corner { BottomRight, BottomLeft, TopRight, TopLeft };

    // The settings, in Settings::instance() (the constructor defines them).
    static constexpr auto positionKey = "Notifications/position";
    static constexpr auto durationKey = "Notifications/duration";
    static constexpr auto warningDurationKey = "Notifications/warningDuration";
    static constexpr auto errorsStickyKey = "Notifications/errorsSticky";
    static constexpr auto maxVisibleKey = "Notifications/maxVisible";
    static void defineSettings();

    explicit ToastManager(QWidget* window);
    ~ToastManager() override;

    // Shows a toast (or queues it) and returns its id. An unset kind is Info;
    // durationMs -1 is the kind's default from the settings, 0 sticky.
    int show(const core::ToastRequest& request, const QList<ToastAction>& actions = {});
    // show() from any thread, later on the UI thread.
    void post(const core::ToastRequest& request, const QList<ToastAction>& actions = {});
    // Closes a toast that is up or drops a queued one; unknown ids are ignored.
    void dismiss(int id);
    // Every toast, up or queued.
    void dismissAll();

    // The toasts that are up (and not closing), newest first.
    QList<ToastWidget*> visibleToasts() const;
    ToastWidget* toast(int id) const;
    int queuedCount() const { return int(m_queue.size()); }
    Corner corner() const;
    // The milliseconds a toast of `kind` stays by default (0: sticky).
    static int defaultDurationMs(core::ToastKind kind);
    // The part of the window the stack keeps to, in global coordinates:
    // inside the title bar, toolbars and status bar.
    QRect area() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Pending {
        int id;
        core::ToastKind kind;
        QString title;
        QString body;
        int durationMs;
        QList<ToastAction> actions;
    };
    void showPending();
    void open(const Pending& pending);
    void relayout(bool animate);
    void scheduleRelayout();
    void onDismissed(int id);

    QPointer<QWidget> m_window;
    QList<ToastWidget*> m_toasts;  // newest first, including ones fading out
    std::deque<Pending> m_queue;
    QToolButton* m_dismissAll = nullptr;
    int m_nextId = 1;
    bool m_relayoutScheduled = false;
};

}  // namespace usdcc::ui
