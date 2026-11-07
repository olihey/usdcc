#pragma once
#include <ovx/dlpack/dlpack.h>

#include <QSize>
#include <memory>
#include "gpu_frame.h"
namespace usdcc::rtx {
class GpuBridge {
public:
    virtual ~GpuBridge() = default;
    virtual uintptr_t stream() const = 0;
    virtual int deviceId() const = 0;
    // Returns nothing when the bounded ring is full. No CPU pixels are read.
    virtual std::shared_ptr<GpuFrame> copy(const DLTensor& tensor) = 0;
};
}  // namespace usdcc::rtx
