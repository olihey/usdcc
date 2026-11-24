#pragma once

// The overlay DockPanel::beginBusy() shows (docs/PANEL_BUSY.md §3, §4). Private to usdcc_ui.

#include "usdcc/ui/busy.h"

#include <QImage>
#include <QPointer>
#include <QRect>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QVariantAnimation;

namespace usdcc::ui {

class BusyOverlay;
class DockPanel;

// What a Busy handle and the overlay share. The setters write under the mutex from any
// thread; the overlay reads on the UI thread, on its timer.
struct Busy::State {
    BusyOptions options;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    mutable std::mutex mutex;
    QString text;
    QString detail;
    double progress = -1.0;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    // UI thread only.
    QPointer<BusyOverlay> overlay;
    bool cancelCalled = false;
};

class BusyOverlay : public QWidget {
    Q_OBJECT
public:
    explicit BusyOverlay(DockPanel* panel);
    ~BusyOverlay() override;

    void add(const std::shared_ptr<Busy::State>& state);
    // Any piece of work still running (shown or not yet).
    bool hasWork() const { return m_busy; }
    // Cancels every piece of work: the panel is closing.
    void cancelAll();
    // A setter called on the UI thread for paintNow work: repaint now, not on the timer.
    void repaintSoon();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void tick();
    void appear(bool now);
    void disappear();
    void refreshCard();
    void placeCard();
    void cancel(const std::shared_ptr<Busy::State>& state);
    std::shared_ptr<Busy::State> shown() const;  // the newest still running
    bool small() const;

    DockPanel* m_panel;
    std::vector<std::shared_ptr<Busy::State>> m_states;
    QTimer* m_timer;
    QVariantAnimation* m_fade;
    double m_opacity = 0.0;
    bool m_busy = false;  // busyChanged(true) said, busyChanged(false) not yet
    bool m_up = false;
    int m_spin = 0;
    std::chrono::steady_clock::time_point m_lastRepaint;
    // Native windows stepped aside while it's up (§4), each with the picture shown instead.
    struct Stepped {
        QPointer<QWidget> container;
        QRect rect;  // in the overlay
        QImage picture;
    };
    std::vector<Stepped> m_stepped;

    QFrame* m_card;
    QLabel* m_text;
    QProgressBar* m_bar;
    QLabel* m_detail;
    QLabel* m_time;
    QPushButton* m_cancel;
};

}  // namespace usdcc::ui
