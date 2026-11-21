#pragma once
#include <memory>

namespace usdcc::rtx {
class GpuBridge;
// A CUDA-only bridge on CUDA device `device`, which copies each frame to the CPU: what final
// renders give their engine (rtx_final.h). Throws when CUDA isn't there.
std::shared_ptr<GpuBridge> createHeadlessBridge(int device = 0);
}  // namespace usdcc::rtx
