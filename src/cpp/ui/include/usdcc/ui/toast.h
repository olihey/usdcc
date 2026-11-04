#pragma once

#include "usdcc/core/toast.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <functional>

class QLabel;
class QPropertyAnimation;
class QScrollArea;
class QToolButton;

namespace usdcc::ui {

// A button on a toast ("Show Log"). Clicking it runs `run` and dismisses the toast.
struct ToastAction {
    QString text;
    std::function<void()> run;
};

// One toast notification (docs/NOTIFICATIONS.md): a card with a stripe and an
// icon in its kind's colour, a bold title, a word-wrapped body, a close button,
// optional action buttons, an "×N" badge for repeats and, while it is timed,
// a bar counting down the time left. Its text decides its size, between
// minimumWidth and maximumWidth, with a body taller than `maximumHeight`
// scrolling.
//
// A separate frameless tool window owned by the main window rather than a
// child widget: the viewport is a native OpenGL window, which would draw over
// any plain child overlay. A tool window stays above its owner only (not
// above other applications) and is hidden with it when it's minimized.
// ToastManager places it; it never takes the focus.
class ToastWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int minimumWidth = 260;  // at 100% UI scale
    static constexpr int maximumWidth = 420;

    ToastWidget(int id, core::ToastKind kind, const QString& title, const QString& body, int durationMs,
                const QList<ToastAction>& actions, int maximumHeight, QWidget* owner);
    ~ToastWidget() override;

    int id() const { return m_id; }
    core::ToastKind kind() const { return m_kind; }
    QString title() const { return m_title; }
    QString body() const { return m_body; }
    // Milliseconds it stays; 0 = until dismissed.
    int durationMs() const { return m_durationMs; }
    bool isSticky() const { return m_durationMs == 0; }
    // Time left before it closes by itself (its duration while sticky).
    int remainingMs() const;
    // The countdown stops while the pointer is over it.
    bool isPaused() const { return m_paused; }
    // How often it was shown again while it was up (1 at first).
    int repeatCount() const { return m_repeatCount; }
    // Whether it is fading out, on its way to dismissed().
    bool isClosing() const { return m_closing; }

    // Shows it (fading in) at `position` and starts its countdown.
    void showAt(const QPoint& position);
    // Moves it, animated or at once.
    void moveTo(const QPoint& position, bool animate);
    // The same toast again: counts it and restarts the countdown.
    void repeat();
    // Fades it out; then dismissed() is emitted. Calling it again does nothing.
    void dismiss();

    void setPaused(bool paused);

Q_SIGNALS:
    // Faded out (or closed at once); ToastManager deletes it.
    void dismissed(int id);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void layOut(int maximumHeight);
    void restartCountdown();
    void tick();

    int m_id;
    core::ToastKind m_kind;
    QString m_title;
    QString m_body;
    int m_durationMs;
    int m_remainingMs;
    int m_repeatCount = 1;
    bool m_paused = false;
    bool m_closing = false;
    QElapsedTimer m_clock;  // since the countdown last (re)started
    QTimer m_ticker;        // repaints the countdown bar, closes when it runs out
    QLabel* m_count = nullptr;
    QPointer<QPropertyAnimation> m_moveAnimation;
};

}  // namespace usdcc::ui
