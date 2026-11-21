#pragma once
#include <QSize>
#include <QString>
#include <memory>
#include <vector>
namespace usdcc::usd {
class ImageTransform;
}
namespace usdcc::rtx {
class GpuFrame {
public:
    std::shared_ptr<GpuFrame> cleanFrame;
    // How save() turns a linear frame into the file's display colors: the panel's view
    // transform, set before the save is queued (docs/COLOR_MANAGEMENT.md §5). An 8-bit
    // frame (RTX's own tonemap) is saved as it is.
    std::shared_ptr<const usdcc::usd::ImageTransform> saveTransform;
    virtual ~GpuFrame() = default;
    virtual QSize size() const = 0;
    virtual bool copied() const = 0;
    virtual void save(const QString& file) const = 0;
    // The frame as linear float RGBA, top row first, for a frame whose pixels are on the CPU
    // (the headless bridge's, for final renders). False for one that isn't, or isn't linear.
    virtual bool readLinear(std::vector<float>& rgba) const {
        (void)rgba;
        return false;
    }
};
}
