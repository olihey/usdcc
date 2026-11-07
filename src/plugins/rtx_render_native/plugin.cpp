#include "usdcc/plugin/plugin.h"

#include <QCoreApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QProgressDialog>
#include <QTimer>
#include <limits>
#include <utility>

#include "rtx_engine.h"
#include "vulkan_window.h"

#include "rtx_panel.h"
namespace usdcc::rtx {
namespace {
// Intercept Close before the host's unsaved-layer questions. Polling a pending
// SDK/driver operation keeps the app responsive; Cancel keeps the app open.
class ShutdownGate final : public QObject {
public:
    explicit ShutdownGate(usdcc::ui::MainWindow* window) : QObject(window), window(window) {
        timer.setInterval(50);
        connect(&timer, &QTimer::timeout, this, [this] {
            if (drained()) {
                timer.stop();
                progress->hide();
                allowNext = true;
                QTimer::singleShot(0, this, [this] { this->window->close(); });
            } else
                progress->setLabelText(
                    QStringLiteral(
                        "Finishing pending RTX work — %1 s. The scene remains available while the native SDK drains.")
                        .arg(elapsed.elapsed() / 1000));
        });
        window->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object != window || event->type() != QEvent::Close) return false;
        if (allowNext) {
            allowNext = false;
            return false;
        }
        RtxEngine::shutdown();
        if (drained()) return false;
        static_cast<QCloseEvent*>(event)->ignore();
        elapsed.start();
        // Construct only after Close: an idle QProgressDialog can otherwise
        // show itself during normal rendering through its automatic timer.
        if (!progress) {
            progress = new QProgressDialog("Finishing pending RTX work…", "Cancel close", 0, 0, window);
            progress->setMinimumDuration(std::numeric_limits<int>::max());
            progress->setWindowModality(Qt::NonModal);
            progress->setAutoClose(false);
            progress->setAutoReset(false);
            connect(progress, &QProgressDialog::canceled, this, [this] {
                timer.stop();
                progress->hide();
            });
        }
        progress->show();
        timer.start();
        return true;
    }

private:
    bool drained() {
        if (!RtxEngine::shutdownFinished()) return false;
        quiesceRtxPanels();
        return VulkanWindow::retirementIdle();
    }
    usdcc::ui::MainWindow* window;
    QProgressDialog* progress{};
    QTimer timer;
    QElapsedTimer elapsed;
    bool allowNext = false;
};
}  // namespace
}  // namespace usdcc::rtx
USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    usdcc::rtx::defineRtxSettings();
    if (host.mainWindow) new usdcc::rtx::ShutdownGate(host.mainWindow);
    host.addViewPanel("org.usdcc.rtx-render.view", "RTX Render",
                      [&host](const auto& stage, const auto& title, auto* parent) -> usdcc::ui::ViewPanel* {
                          return usdcc::rtx::createRtxPanel(host, stage, title, parent,
                              [](QString rtx, QString ovstage, auto bridge) -> std::unique_ptr<usdcc::rtx::Engine> {
                                  return std::make_unique<usdcc::rtx::RtxEngine>(std::move(rtx), std::move(ovstage), std::move(bridge));
                              }, usdcc::rtx::createVulkanPresentation);
                      });
}
USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host&) { usdcc::rtx::RtxEngine::shutdown(); }
