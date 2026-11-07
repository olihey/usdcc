#include "rtx_panel.h"
#include "vulkan_window.h"
#include <QCoreApplication>
#include <QTimer>
namespace usdcc::rtx {
namespace {
class VulkanPresentation final : public Presentation {
public:
    VulkanPresentation() : view(new VulkanWindow), container(QWidget::createWindowContainer(view)) {
        view->setObjectName("nativeRtxWindow");
        view->presented = [this](QSize size, const QString& error) { if (presented) presented(size, error); };
    }
    ~VulkanPresentation() override { close(); }
    QWidget* widget() override { return container; }
    QWindow* window() override { return view; }
    QString initialize() override { return view->initialize(); }
    QString error() const override { return view->error(); }
    std::shared_ptr<GpuBridge> bridge() const override { return view->bridge(); }
    void setFrame(std::shared_ptr<GpuFrame> frame) override { view->setFrame(std::move(frame)); }
    std::shared_ptr<GpuFrame> currentFrame() const override { return view->currentFrame(); }
    void clear() override { view->clear(); }
    void releaseResources() override { view->releaseResources(); }
    void close() override {
        if (!view) return;
        view->presented = {};
        view->releaseResources();
        if (!VulkanWindow::retirementIdle()) {
            view->hide();
            view->setParent(static_cast<QWindow*>(nullptr));
            auto* keep = view;
            auto* timer = new QTimer(QCoreApplication::instance());
            QObject::connect(timer, &QTimer::timeout, timer, [keep, timer] {
                if (VulkanWindow::retirementIdle()) { timer->stop(); delete keep; timer->deleteLater(); }
            });
            timer->start(20);
        }
        view = nullptr; // otherwise the container owns the QWindow
    }
private:
    VulkanWindow* view;
    QWidget* container;
};
}
std::unique_ptr<Presentation> createVulkanPresentation() { return std::make_unique<VulkanPresentation>(); }
}
