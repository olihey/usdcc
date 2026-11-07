#pragma once
#include <ovx/dlpack/dlpack.h>

#include <QSize>
#include <memory>
namespace usdcc::rtx {
class GpuFrame {
public:
    // Highlighted display frames retain their clean companion for Save Image.
    // Both remain on the GPU until explicit readback is requested.
    std::shared_ptr<GpuFrame> cleanFrame;
    virtual ~GpuFrame() = default;
    virtual QSize size() const = 0;
    virtual bool copied() const = 0;
    // Explicit readback only; holds the exact displayed frame while saving.
    virtual void save(const QString& file) const = 0;
};
class GpuBridge {
public:
    virtual ~GpuBridge() = default;
    virtual uintptr_t stream() const = 0;
    virtual int deviceId() const = 0;
    // Returns nothing when the bounded ring is full. No CPU pixels are read.
    virtual std::shared_ptr<GpuFrame> copy(const DLTensor& tensor) = 0;
};
}  // namespace usdcc::rtx
