// A busy view panel (docs/PANEL_BUSY.md): the overlay's delay, its card, input, cancelling,
// progress from a worker thread, several handles, closing, a handle outliving its panel, and
// native windows stepping aside. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/busy.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/stage_manager.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWindow>

#include <atomic>
#include <memory>
#include <thread>

using usdcc::ui::Busy;
using usdcc::ui::BusyOptions;
using usdcc::ui::ViewPanel;

namespace {
void settle(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

template <typename Condition>
bool waitUntil(Condition condition, int ms = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > ms) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

// A panel with a toolbar, a label and, when asked, a native window as the viewport has.
class BusyTestPanel : public ViewPanel {
public:
    BusyTestPanel(usdcc::usd::StageManager* stages, bool native) : ViewPanel(stages, nullptr, QStringLiteral("Busy")) {
        auto* container = new QWidget(this);
        auto* layout = new QVBoxLayout(container);
        auto* toolbar = new QWidget(container);
        auto* toolbarLayout = new QHBoxLayout(toolbar);
        addPanelControls(toolbarLayout);
        layout->addWidget(toolbar);
        content = new QLabel(QStringLiteral("content"), container);
        layout->addWidget(content, 1);
        if (native) {
            window = new QWindow;
            nativeContainer = QWidget::createWindowContainer(window, container);
            layout->addWidget(nativeContainer, 1);
        }
        setContentWidget(container);
        refreshStageCombo();
    }
    ViewPanel* duplicate(QWidget*) const override { return nullptr; }
    QImage busySnapshot(QWidget* container) override {
        ++snapshots;
        QImage image(container->size().expandedTo(QSize(1, 1)), QImage::Format_RGB32);
        image.fill(Qt::red);
        return image;
    }

    QLabel* content = nullptr;
    QWindow* window = nullptr;
    QWidget* nativeContainer = nullptr;
    int snapshots = 0;

protected:
    void populateOptionsMenu(QMenu*) override {}
};

QWidget* overlayOf(const ViewPanel& panel) { return panel.findChild<QWidget*>(QStringLiteral("busyOverlay")); }
bool overlayUp(const ViewPanel& panel) {
    const QWidget* overlay = overlayOf(panel);
    return overlay && overlay->isVisible();
}
QString cardText(const ViewPanel& panel) {
    const auto* label = panel.findChild<QLabel*>(QStringLiteral("busyText"));
    return label ? label->text() : QString();
}
}  // namespace

USDCC_TEST(busy_panel_overlay) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    usdcc::usd::StageManager stages;
    BusyTestPanel panel(&stages, false);
    panel.resize(500, 400);
    panel.show();
    QList<bool> changes;
    QObject::connect(&panel, &ViewPanel::busyChanged, &panel, [&changes](bool busy) { changes.append(busy); });

    // Quick work never shows.
    {
        auto busy = panel.beginBusy(QStringLiteral("Quick"));
        CHECK(panel.isBusy() && !overlayUp(panel), "busy at once, the overlay not yet");
        settle(100);
        CHECK(!overlayUp(panel), "not before the delay");
    }
    settle(100);
    CHECK(!panel.isBusy() && !overlayUp(panel) && changes == QList<bool>({true, false}),
          "quick work: busy and back, never shown (%d changes)", int(changes.size()));

    // Work that takes a while: after the delay, the card with the text, the stripe, no Cancel.
    changes.clear();
    auto busy = panel.beginBusy(QStringLiteral("Baking lights"));
    CHECK(waitUntil([&] { return overlayUp(panel); }, 2000), "shows after the delay");
    CHECK(cardText(panel) == "Baking lights", "with the text: %s", qPrintable(cardText(panel)));
    auto* bar = panel.findChild<QProgressBar*>(QStringLiteral("busyProgress"));
    auto* cancel = panel.findChild<QPushButton*>(QStringLiteral("busyCancel"));
    CHECK(bar && bar->maximum() == 0, "an indeterminate bar until there's progress");
    CHECK(cancel && !cancel->isVisible(), "no Cancel without onCancel");
    CHECK(overlayOf(panel)->geometry() == panel.rect(), "over the whole panel, its toolbar too");
    const QPoint middle = panel.content->mapTo(&panel, panel.content->rect().center());
    QWidget* hit = panel.childAt(middle);
    CHECK(hit && (hit == overlayOf(panel) || overlayOf(panel)->isAncestorOf(hit)), "it takes the clicks over the content");

    // Progress, text and detail from a worker thread.
    std::thread([busy]() {
        busy->setProgress(0.5);
        busy->setText(QStringLiteral("Baking 5 of 10"));
        busy->setDetail(QStringLiteral("1.2 GB written"));
    }).join();
    CHECK(waitUntil([&] { return bar->maximum() == 1000 && bar->value() == 500 && cardText(panel) == "Baking 5 of 10"; }),
          "a worker's progress and text show: %d", bar->value());
    auto* detail = panel.findChild<QLabel*>(QStringLiteral("busyDetail"));
    CHECK(detail && detail->text() == "1.2 GB written" && detail->isVisible(), "and its detail");

    // Two at once: the newest one's card; when it ends, the other's again.
    auto second = panel.beginBusy(QStringLiteral("Second"));
    CHECK(waitUntil([&] { return cardText(panel) == "Second"; }), "the newest shows");
    second.reset();
    CHECK(waitUntil([&] { return cardText(panel) == "Baking 5 of 10"; }), "then the first again");
    CHECK(panel.isBusy() && changes == QList<bool>({true}), "busy throughout, said once");

    // Finished from a worker thread: it goes at the next tick.
    std::thread([busy]() { busy->finish(); }).join();
    CHECK(waitUntil([&] { return !overlayUp(panel) && !panel.isBusy(); }), "finished from a thread, it goes");
    CHECK(changes == QList<bool>({true, false}), "and says so once");
}

USDCC_TEST(busy_panel_cancel) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    usdcc::usd::StageManager stages;
    auto panel = std::make_unique<BusyTestPanel>(&stages, false);
    panel->resize(500, 400);
    panel->show();

    // Cancel: called once, the handle says so, the overlay goes.
    int cancels = 0;
    BusyOptions options;
    options.onCancel = [&cancels]() { ++cancels; };
    options.showAfterMs = 0;
    auto busy = panel->beginBusy(QStringLiteral("Converting"), options);
    CHECK(waitUntil([&] { return overlayUp(*panel); }), "shown");
    auto* cancel = panel->findChild<QPushButton*>(QStringLiteral("busyCancel"));
    CHECK(cancel && cancel->isVisible(), "with Cancel");
    if (cancel) cancel->click();
    CHECK(cancels == 1 && busy->isCancelled(), "Cancel calls onCancel, and the handle knows");
    CHECK(waitUntil([&] { return !overlayUp(*panel) && !panel->isBusy(); }), "the overlay goes at once");

    // Esc, when the keys go to the overlay.
    busy = panel->beginBusy(QStringLiteral("Again"), options);
    CHECK(waitUntil([&] { return overlayUp(*panel); }), "shown again");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(overlayOf(*panel), &escape);
    CHECK(cancels == 2 && busy->isCancelled(), "Esc cancels");

    // paintNow: up at once, no delay.
    BusyOptions now;
    now.paintNow = true;
    auto immediate = panel->beginBusy(QStringLiteral("Flattening"), now);
    CHECK(overlayUp(*panel) && cardText(*panel) == "Flattening", "paintNow shows before beginBusy() returns");
    immediate.reset();
    CHECK(waitUntil([&] { return !overlayUp(*panel); }), "and goes when it's dropped");

    // Closing the panel cancels its work; a handle outliving it does nothing.
    auto left = panel->beginBusy(QStringLiteral("Left running"), options);
    panel.reset();
    CHECK(cancels == 3 && left->isCancelled(), "closing the panel cancels");
    left->setProgress(0.7);
    left->setText(QStringLiteral("nobody sees this"));
    left->finish();
    CHECK(true, "a handle outliving its panel is harmless");
}

USDCC_TEST(busy_panel_native_windows) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    usdcc::usd::StageManager stages;
    BusyTestPanel panel(&stages, true);
    panel.resize(500, 400);
    panel.show();
    settle(50);
    CHECK(panel.nativeContainer && panel.nativeContainer->isVisible(), "a native window in the panel");

    BusyOptions options;
    options.showAfterMs = 0;
    auto busy = panel.beginBusy(QStringLiteral("Loading"), options);
    CHECK(waitUntil([&] { return overlayUp(panel); }), "shown");
    CHECK(!panel.nativeContainer->isVisible() && panel.snapshots == 1, "the native window steps aside, a still in its place");
    busy.reset();
    CHECK(waitUntil([&] { return !overlayUp(panel); }), "gone");
    CHECK(panel.nativeContainer->isVisible(), "and the native window is back");
}
