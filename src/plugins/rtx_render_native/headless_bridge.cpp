// The GPU bridge final renders use (rtx_final.h): CUDA only, no Vulkan window. Each frame
// ovrtx hands over is copied from the GPU to the CPU, where the final render reads it as
// linear floats. Panels keep using the Vulkan presentation's bridge (vulkan_window.cpp), which
// never reads pixels back except to save.
#include "headless_bridge.h"

#include "cuda_driver.h"
#include "gpu_bridge.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/linear_exr.h"

#include <QImage>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace usdcc::rtx {
namespace {

float halfToFloat(uint16_t half) {
    const uint32_t sign = uint32_t(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1f;
    uint32_t mantissa = half & 0x3ff;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {  // subnormal
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ff;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000 | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

struct HostFrame final : GpuFrame {
    QSize dimensions;
    int channels = 4;
    int componentBytes = 4;
    std::vector<unsigned char> pixels;

    QSize size() const override { return dimensions; }
    bool copied() const override { return true; }  // copied synchronously

    bool readLinear(std::vector<float>& rgba) const override {
        if (componentBytes == 1) return false;  // RTX's own tonemap: display colors
        const size_t count = size_t(dimensions.width()) * size_t(dimensions.height());
        rgba.assign(count * 4, 1.0f);
        for (size_t p = 0; p < count; ++p) {
            for (int c = 0; c < channels && c < 4; ++c) {
                const unsigned char* at = pixels.data() + (p * channels + c) * componentBytes;
                if (componentBytes == 2) {
                    uint16_t half;
                    std::memcpy(&half, at, 2);
                    rgba[p * 4 + c] = halfToFloat(half);
                } else {
                    std::memcpy(&rgba[p * 4 + c], at, 4);
                }
            }
        }
        return true;
    }

    void save(const QString& file) const override {
        std::vector<float> rgba;
        if (!readLinear(rgba)) throw std::runtime_error("No linear image to save");
        if (file.endsWith(QStringLiteral(".exr"), Qt::CaseInsensitive)) {
            std::string error;
            if (!usdcc::usd::writeLinearExr(file.toStdString(), rgba.data(), dimensions.width(), dimensions.height(), &error))
                throw std::runtime_error("Could not save " + file.toStdString() + ": " + error);
            return;
        }
        QImage image(dimensions, QImage::Format_RGBA32FPx4);
        for (int y = 0; y < dimensions.height(); ++y) {
            auto* row = reinterpret_cast<float*>(image.scanLine(y));
            std::memcpy(row, rgba.data() + size_t(y) * size_t(dimensions.width()) * 4, size_t(dimensions.width()) * 16);
            if (saveTransform) saveTransform->apply(row, size_t(dimensions.width()));
        }
        if (!image.convertToFormat(QImage::Format_RGBA8888).save(file))
            throw std::runtime_error("Could not save " + file.toStdString());
    }
};

struct HeadlessBridge final : GpuBridge {
    Cuda cuda;
    CUcontext context{};
    CUstream copyStream{};
    int device = 0;

    explicit HeadlessBridge(int ordinal) : device(ordinal) {
        cudaCheck(cuda.cuInit(0), "cuInit");
        int count = 0;
        cudaCheck(cuda.cuDeviceGetCount(&count), "count CUDA devices");
        if (device < 0 || device >= count) throw std::runtime_error("No CUDA device " + std::to_string(device));
        cudaCheck(cuda.cuDevicePrimaryCtxRetain(&context, CUdevice(device)), "retain CUDA context");
        CudaContext current(cuda, context);
        cudaCheck(cuda.cuStreamCreate(&copyStream, CU_STREAM_NON_BLOCKING), "create CUDA stream");
    }
    ~HeadlessBridge() override {
        if (context) {
            if (cuda.cuCtxPushCurrent(context) == CUDA_SUCCESS) {
                if (copyStream) {
                    cuda.cuStreamSynchronize(copyStream);
                    cuda.cuStreamDestroy(copyStream);
                }
                CUcontext old{};
                cuda.cuCtxPopCurrent(&old);
            }
            cuda.cuDevicePrimaryCtxRelease(CUdevice(device));
        }
    }
    uintptr_t stream() const override { return reinterpret_cast<uintptr_t>(copyStream); }
    int deviceId() const override { return device; }

    std::shared_ptr<GpuFrame> copy(const DLTensor& tensor) override {
        const bool bytes = tensor.dtype.code == kDLUInt && tensor.dtype.bits == 8;
        const bool floats = tensor.dtype.code == kDLFloat && (tensor.dtype.bits == 16 || tensor.dtype.bits == 32);
        const int64_t channels = tensor.ndim == 3 && tensor.shape ? tensor.shape[2] : 0;
        if (!tensor.data || tensor.device.device_type != kDLCUDA || tensor.device.device_id != device || tensor.ndim != 3 ||
            (channels != 4 && channels != 3) || (!bytes && !floats) || tensor.dtype.lanes != 1 || tensor.shape[0] < 1 ||
            tensor.shape[1] < 1 || (tensor.strides && (tensor.strides[2] != 1 || tensor.strides[1] != channels)))
            throw std::runtime_error("RTX output must be RGBA8, or half or float RGB(A), CUDA data on the bridge's device");
        auto frame = std::make_shared<HostFrame>();
        frame->dimensions = QSize(int(tensor.shape[1]), int(tensor.shape[0]));
        frame->channels = int(channels);
        frame->componentBytes = tensor.dtype.bits / 8;
        const size_t rowBytes = size_t(frame->dimensions.width()) * size_t(channels) * size_t(frame->componentBytes);
        frame->pixels.resize(rowBytes * size_t(frame->dimensions.height()));
        CudaContext current(cuda, context);
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = reinterpret_cast<CUdeviceptr>(tensor.data) + tensor.byte_offset;
        copy.srcPitch = tensor.strides ? size_t(tensor.strides[0]) * size_t(frame->componentBytes) : rowBytes;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = frame->pixels.data();
        copy.dstPitch = copy.WidthInBytes = rowBytes;
        copy.Height = size_t(frame->dimensions.height());
        cudaCheck(cuda.cuMemcpy2DAsync(&copy, copyStream), "read RTX output back");
        cudaCheck(cuda.cuStreamSynchronize(copyStream), "wait for RTX output");
        return frame;
    }
};

}  // namespace

std::shared_ptr<GpuBridge> createHeadlessBridge(int device) { return std::make_shared<HeadlessBridge>(device); }

}  // namespace usdcc::rtx
