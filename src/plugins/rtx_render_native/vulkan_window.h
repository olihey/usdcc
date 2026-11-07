#pragma once
#include <QWindow>
#include <functional>

#include "gpu_bridge.h"
namespace usdcc::rtx {
class VulkanWindow final : public QWindow {
public:
    VulkanWindow();
    ~VulkanWindow() override;
    QString initialize();
    QString error() const;
    std::shared_ptr<GpuBridge> bridge() const;
    void setFrame(std::shared_ptr<GpuFrame> frame);
    std::shared_ptr<GpuFrame> currentFrame() const;
    void clear();
    void releaseResources();
    static bool retirementIdle();
    int validationErrors() const;
    std::function<void(QSize, const QString&)> presented;

protected:
    bool event(QEvent*) override;
    void exposeEvent(QExposeEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void present();
    void pollPresentation();
    struct Impl;
    std::unique_ptr<Impl> m;
};
}  // namespace usdcc::rtx
