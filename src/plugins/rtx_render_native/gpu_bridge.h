#pragma once
#include <ovx/dlpack/dlpack.h>

#include <QSize>
#include <memory>
namespace usdcc::rtx {
class GpuFrame {
public:
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
    // Returns nothing when the three-slot ring is full. No CPU pixels are read.
    virtual std::shared_ptr<GpuFrame> copy(const DLTensor& tensor) = 0;
};
}  // namespace usdcc::rtx
