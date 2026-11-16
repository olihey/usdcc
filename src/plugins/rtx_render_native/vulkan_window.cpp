#define VOLK_IMPLEMENTATION
#include "vulkan_window.h"

#include <ovrtx/ovrtx_types.h>
#include <volk.h>
#include <windows.h>

#include <QEvent>
#include <QImage>
#include <QTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "cuda_driver.h"
#include "shaders/present_comp_spv.h"

namespace usdcc::rtx {
namespace {
// IEEE half floats: the LUT goes up as half (linear filtering is guaranteed for it), and a
// saved frame comes back as half.
uint16_t toHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = int((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) return uint16_t(sign);                    // too small: zero
    if (exponent >= 31) return uint16_t(sign | 0x7c00u);         // too big: infinity
    mantissa += 0x1000u;                                         // round to nearest
    if (mantissa & 0x800000u) return uint16_t(sign | ((exponent + 1) << 10));
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}
float fromHalf(uint16_t half) {
    const uint32_t sign = uint32_t(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    const uint32_t mantissa = half & 0x3ffu;
    float value;
    if (exponent == 0) value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = std::ldexp(float(mantissa | 0x400u), int(exponent) - 25);
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bits |= sign;
    std::memcpy(&value, &bits, 4);
    return value;
}
void vkCheck(VkResult result, const char* operation) {
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
        throw std::runtime_error("A compatible NVIDIA Vulkan driver is unavailable. Check the driver installation and restart usdcc.");
    if (result != VK_SUCCESS)
        throw std::runtime_error(QStringLiteral("%1: Vulkan %2").arg(operation).arg(result).toStdString());
}
// One process-owned retirement worker; no GUI waits or detached per-frame jobs.
class Retirement {
public:
    static Retirement& instance() {
        static auto* value = new Retirement;
        return *value;
    }
    void add(std::function<void()> job) {
        ++pending;
        {
            std::lock_guard lock(mutex);
            jobs.push_back(std::move(job));
        }
        wake.notify_one();
    }
    bool idle() const { return pending.load(std::memory_order_acquire) == 0; }

private:
    Retirement()
        : worker([this] {
              for (;;) {
                  std::function<void()> job;
                  {
                      std::unique_lock lock(mutex);
                      wake.wait(lock, [this] { return !jobs.empty(); });
                      job = std::move(jobs.front());
                      jobs.pop_front();
                  }
                  try {
                      job();
                  } catch (...) {
                  }
                  --pending;
              }
          }) {}
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::thread worker;
    std::atomic<size_t> pending{0};
};
struct Device {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT debug{};
    std::shared_ptr<std::atomic<int>> errors = std::make_shared<std::atomic<int>>(0);
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    Cuda cuda;
    CUdevice cudaDevice = -1;
    CUcontext context{};
    std::mutex queueMutex;
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) const {
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        throw std::runtime_error("Vulkan has no suitable memory type");
    }
    ~Device() {
        if (device) {
            std::lock_guard lock(queueMutex);
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
        }
        if (context) cuda.cuDevicePrimaryCtxRelease(cudaDevice);
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (debug) vkDestroyDebugUtilsMessengerEXT(instance, debug, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
struct Frame final : GpuFrame {
    std::shared_ptr<Device> owner;
    QSize dimensions;
    // The pixels as ovrtx gave them: 4 channels (or 3), each 1 byte (LdrColor, display
    // colors), 2 (half) or 4 (float) (HdrColor, linear).
    int channels = 4;
    int componentBytes = 1;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    // The frame in display colors, RGBA8, which the presentation pass writes and blits.
    VkImage image{};
    VkDeviceMemory imageMemory{};
    VkImageView view{};
    VkSemaphore ready{};
    CUexternalMemory cudaMemory{};
    CUdeviceptr pointer{};
    CUexternalSemaphore cudaReady{};
    CUevent complete{};
    std::atomic<bool> firstPresentation{true};
    std::atomic<int> readers{0};
    mutable std::mutex access;
    explicit Frame(std::shared_ptr<Device> device) : owner(std::move(device)) {}
    size_t pixelBytes() const { return size_t(channels) * size_t(componentBytes); }
    QSize size() const override { return dimensions; }
    bool copied() const override {
        CudaContext current(owner->cuda, owner->context);
        CUresult status = owner->cuda.cuEventQuery(complete);
        if (status == CUDA_ERROR_NOT_READY) return false;
        cudaCheck(status, "CUDA copy completion");
        return true;
    }
    void save(const QString& file) const override {
        std::lock_guard lock(access);
        // Keep Vulkan from acquiring this buffer while the explicit readback
        // runs. The GUI only tries the lock; fence polling remains responsive.
        while (readers.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CudaContext current(owner->cuda, owner->context);
        const size_t rowBytes = size_t(dimensions.width()) * pixelBytes();
        std::vector<unsigned char> pixels(rowBytes * size_t(dimensions.height()));
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = pointer;
        copy.srcPitch = copy.WidthInBytes = copy.dstPitch = rowBytes;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = pixels.data();
        copy.Height = dimensions.height();
        cudaCheck(owner->cuda.cuMemcpy2D(&copy), "read displayed frame for saving");
        QImage image;
        if (componentBytes == 1) {
            // Display colors already (RTX's tonemap): as they are.
            image = QImage(pixels.data(), dimensions.width(), dimensions.height(), qsizetype(rowBytes),
                           channels == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888)
                        .copy();
        } else {
            // Linear: through the panel's view transform, as the window shows it.
            image = QImage(dimensions, QImage::Format_RGBA32FPx4);
            if (image.isNull()) throw std::runtime_error("Could not allocate image for saving");
            for (int y = 0; y < dimensions.height(); ++y) {
                auto* out = reinterpret_cast<float*>(image.scanLine(y));
                const unsigned char* row = pixels.data() + rowBytes * size_t(y);
                for (int x = 0; x < dimensions.width(); ++x) {
                    for (int c = 0; c < 4; ++c) {
                        float value = 1.0f;
                        if (c < channels) {
                            const unsigned char* at = row + (size_t(x) * channels + c) * componentBytes;
                            if (componentBytes == 2) {
                                uint16_t half;
                                std::memcpy(&half, at, 2);
                                value = fromHalf(half);
                            } else {
                                std::memcpy(&value, at, 4);
                            }
                        }
                        out[x * 4 + c] = value;
                    }
                }
                if (saveTransform) saveTransform->apply(out, size_t(dimensions.width()));
            }
            image = image.convertToFormat(QImage::Format_RGBA8888);
        }
        if (image.isNull() || !image.save(file)) throw std::runtime_error("Could not save " + file.toStdString());
    }
    ~Frame() {
        // Device-loss errors during destruction must not throw from a noexcept
        // destructor. The driver owns recovery of invalidated resources.
        const bool pushed = owner->cuda.cuCtxPushCurrent(owner->context) == CUDA_SUCCESS;
        if (complete) owner->cuda.cuEventDestroy(complete);
        if (cudaReady) owner->cuda.cuDestroyExternalSemaphore(cudaReady);
        if (pointer) owner->cuda.cuMemFree(pointer);
        if (cudaMemory) owner->cuda.cuDestroyExternalMemory(cudaMemory);
        if (ready) vkDestroySemaphore(owner->device, ready, nullptr);
        if (view) vkDestroyImageView(owner->device, view, nullptr);
        if (buffer) vkDestroyBuffer(owner->device, buffer, nullptr);
        if (memory) vkFreeMemory(owner->device, memory, nullptr);
        if (image) vkDestroyImage(owner->device, image, nullptr);
        if (imageMemory) vkFreeMemory(owner->device, imageMemory, nullptr);
        if (pushed) {
            CUcontext old{};
            owner->cuda.cuCtxPopCurrent(&old);
        }
    }
    void allocate(QSize size, int pixelChannels, int bytesPerComponent) {
        dimensions = size;
        channels = pixelChannels;
        componentBytes = bytesPerComponent;
        // Whole words, as the presentation pass reads them.
        const VkDeviceSize bytes = (VkDeviceSize(size.width()) * size.height() * pixelBytes() + 3) & ~VkDeviceSize(3);
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.pNext = &external;
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCheck(vkCreateBuffer(owner->device, &bufferInfo, nullptr, &buffer), "create shared buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(owner->device, buffer, &requirements);
        VkExportMemoryAllocateInfo exported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exported.handleTypes = external.handleTypes;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &exported;
        dedicated.buffer = buffer;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &dedicated;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = owner->memoryType(requirements.memoryTypeBits);
        vkCheck(vkAllocateMemory(owner->device, &allocate, nullptr, &memory), "allocate shared memory");
        vkCheck(vkBindBufferMemory(owner->device, buffer, memory, 0), "bind shared buffer");
        VkMemoryGetWin32HandleInfoKHR handleInfo{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        handleInfo.memory = memory;
        handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE handle{};
        vkCheck(vkGetMemoryWin32HandleKHR(owner->device, &handleInfo, &handle), "export shared memory");
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC import{};
        import.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
        import.handle.win32.handle = handle;
        import.size = requirements.size;
        import.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
        CUresult status = owner->cuda.cuImportExternalMemory(&cudaMemory, &import);
        CloseHandle(handle);
        cudaCheck(status, "import Vulkan memory into CUDA");
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC mapped{};
        mapped.size = bytes;
        cudaCheck(owner->cuda.cuExternalMemoryGetMappedBuffer(&pointer, cudaMemory, &mapped), "map shared CUDA buffer");
        VkExportSemaphoreCreateInfo exportedSemaphore{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportedSemaphore.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphore.pNext = &exportedSemaphore;
        vkCheck(vkCreateSemaphore(owner->device, &semaphore, nullptr, &ready), "create shared semaphore");
        VkSemaphoreGetWin32HandleInfoKHR semaphoreHandle{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        semaphoreHandle.semaphore = ready;
        semaphoreHandle.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        vkCheck(vkGetSemaphoreWin32HandleKHR(owner->device, &semaphoreHandle, &handle), "export shared semaphore");
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC importSemaphore{};
        importSemaphore.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32;
        importSemaphore.handle.win32.handle = handle;
        status = owner->cuda.cuImportExternalSemaphore(&cudaReady, &importSemaphore);
        CloseHandle(handle);
        cudaCheck(status, "import Vulkan semaphore into CUDA");
        cudaCheck(owner->cuda.cuEventCreate(&complete, CU_EVENT_DISABLE_TIMING), "create copy event");
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {uint32_t(size.width()), uint32_t(size.height()), 1};
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCheck(vkCreateImage(owner->device, &imageInfo, nullptr, &image), "create presentation image");
        vkGetImageMemoryRequirements(owner->device, image, &requirements);
        allocate.pNext = nullptr;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = owner->memoryType(requirements.memoryTypeBits);
        vkCheck(vkAllocateMemory(owner->device, &allocate, nullptr, &imageMemory), "allocate presentation image");
        vkCheck(vkBindImageMemory(owner->device, image, imageMemory, 0), "bind presentation image");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(owner->device, &viewInfo, nullptr, &view), "create presentation image view");
    }
};
struct Bridge final : GpuBridge {
    std::shared_ptr<Device> device;
    CUstream copyStream{};
    // Three display/clean pairs. Old displayed frames and presentation fences
    // may retain a pair while the next selected image is being produced.
    std::array<std::shared_ptr<Frame>, 6> slots;
    explicit Bridge(std::shared_ptr<Device> value) : device(std::move(value)) {
        CudaContext current(device->cuda, device->context);
        cudaCheck(device->cuda.cuStreamCreate(&copyStream, CU_STREAM_NON_BLOCKING), "create CUDA copy stream");
    }
    ~Bridge() {
        auto keep = device;
        auto stream = copyStream;
        auto frames = std::move(slots);
        Retirement::instance().add([keep, stream, frames = std::move(frames)]() mutable {
            CudaContext current(keep->cuda, keep->context);
            keep->cuda.cuStreamSynchronize(stream);
            keep->cuda.cuStreamDestroy(stream);
            frames = {};
        });
    }
    uintptr_t stream() const override { return reinterpret_cast<uintptr_t>(copyStream); }
    int deviceId() const override { return device->cudaDevice; }
    std::shared_ptr<GpuFrame> copy(const DLTensor& tensor) override {
        // LdrColor: RGBA8, display colors. HdrColor: half or float, linear; RGB or RGBA.
        const bool bytes = tensor.dtype.code == kDLUInt && tensor.dtype.bits == 8;
        const bool floats = tensor.dtype.code == kDLFloat && (tensor.dtype.bits == 16 || tensor.dtype.bits == 32);
        const int64_t channels = tensor.ndim == 3 && tensor.shape ? tensor.shape[2] : 0;
        if (!tensor.data || tensor.device.device_type != kDLCUDA || tensor.device.device_id != device->cudaDevice ||
            tensor.ndim != 3 || !tensor.shape || (channels != 4 && channels != 3) || (!bytes && !floats) ||
            (bytes && channels != 4) || tensor.dtype.lanes != 1 || tensor.shape[0] < 1 || tensor.shape[1] < 1 ||
            tensor.shape[0] > 8192 || tensor.shape[1] > 8192 ||
            (tensor.strides && (tensor.strides[2] != 1 || tensor.strides[1] != channels ||
                                tensor.strides[0] < tensor.shape[1] * channels)))
            throw std::runtime_error("RTX output must be RGBA8, or half or float RGB(A), CUDA data on the Vulkan-matched device");
        const int componentBytes = tensor.dtype.bits / 8;
        const size_t pixelBytes = size_t(channels) * size_t(componentBytes);
        CudaContext current(device->cuda, device->context);
        const QSize size(int(tensor.shape[1]), int(tensor.shape[0]));
        for (auto& slot : slots)
            if (slot && slot.use_count() == 1) slot->cleanFrame.reset();
        for (auto& slot : slots) {
            if (slot && slot.use_count() != 1) continue;
            if (!slot || slot->size() != size || slot->channels != channels || slot->componentBytes != componentBytes) {
                auto candidate = std::shared_ptr<Frame>(
                    new Frame(device), [](Frame* value) { Retirement::instance().add([value] { delete value; }); });
                candidate->allocate(size, int(channels), componentBytes);
                slot = std::move(candidate);
            } else if (slot->firstPresentation.load(std::memory_order_acquire)) {
                // Consume an unpublished frame's signal before slot reuse.
                CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait{};
                cudaCheck(device->cuda.cuWaitExternalSemaphoresAsync(&slot->cudaReady, &wait, 1, copyStream),
                          "retire discarded ready semaphore");
            }
            slot->firstPresentation.store(true, std::memory_order_release);
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice = reinterpret_cast<CUdeviceptr>(tensor.data) + tensor.byte_offset;
            copy.srcPitch = tensor.strides ? size_t(tensor.strides[0]) * size_t(componentBytes) : size_t(size.width()) * pixelBytes;
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstDevice = slot->pointer;
            copy.dstPitch = copy.WidthInBytes = size_t(size.width()) * pixelBytes;
            copy.Height = size.height();
            try {
                cudaCheck(device->cuda.cuMemcpy2DAsync(&copy, copyStream), "copy RTX output on GPU");
                CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal{};
                cudaCheck(device->cuda.cuSignalExternalSemaphoresAsync(&slot->cudaReady, &signal, 1, copyStream),
                          "signal presentation semaphore");
                cudaCheck(device->cuda.cuEventRecord(slot->complete, copyStream), "record GPU copy");
            } catch (...) {
                device->cuda.cuStreamSynchronize(copyStream);
                slot.reset();
                throw;
            }
            return slot;
        }
        return {};
    }
};
// The presentation pass (shaders/present.comp) and the LUT it samples, made with the first
// frame shown and retired after the GPU is done with them, as a swapchain is.
struct PresentPipeline {
    std::shared_ptr<Device> owner;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    VkSampler sampler{};
    int lutSize = 0;
    VkImage lut{};
    VkDeviceMemory lutMemory{};
    VkImageView lutView{};
    VkBuffer staging{};
    VkDeviceMemory stagingMemory{};
    void* stagingData = nullptr;
    bool lutReady = false;  // uploaded and in SHADER_READ_ONLY_OPTIMAL
    explicit PresentPipeline(std::shared_ptr<Device> value) : owner(std::move(value)) {
        VkDevice device = owner->device;
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 3;
        setInfo.pBindings = bindings;
        vkCheck(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout), "create presentation set layout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        vkCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "create presentation pipeline layout");
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = sizeof(kPresentCompSpirv);
        shaderInfo.pCode = kPresentCompSpirv;
        vkCheck(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader), "create presentation shader");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = layout;
        vkCheck(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
                "create presentation pipeline");
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = sizes;
        vkCheck(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "create presentation descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &setLayout;
        vkCheck(vkAllocateDescriptorSets(device, &allocate, &set), "allocate presentation descriptors");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCheck(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "create LUT sampler");
    }
    void destroyLut() {
        VkDevice device = owner->device;
        if (stagingData) vkUnmapMemory(device, stagingMemory);
        stagingData = nullptr;
        if (staging) vkDestroyBuffer(device, staging, nullptr);
        if (stagingMemory) vkFreeMemory(device, stagingMemory, nullptr);
        if (lutView) vkDestroyImageView(device, lutView, nullptr);
        if (lut) vkDestroyImage(device, lut, nullptr);
        if (lutMemory) vkFreeMemory(device, lutMemory, nullptr);
        staging = VK_NULL_HANDLE;
        stagingMemory = lutMemory = VK_NULL_HANDLE;
        lutView = VK_NULL_HANDLE;
        lut = VK_NULL_HANDLE;
        lutSize = 0;
        lutReady = false;
    }
    // A LUT image of `size`³ and its staging buffer, unless there is one of that size.
    void ensureLut(int size) {
        if (size == lutSize) return;
        destroyLut();
        VkDevice device = owner->device;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent = {uint32_t(size), uint32_t(size), uint32_t(size)};
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vkCheck(vkCreateImage(device, &imageInfo, nullptr, &lut), "create LUT image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, lut, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = owner->memoryType(requirements.memoryTypeBits);
        vkCheck(vkAllocateMemory(device, &allocate, nullptr, &lutMemory), "allocate LUT image");
        vkCheck(vkBindImageMemory(device, lut, lutMemory, 0), "bind LUT image");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = lut;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(device, &viewInfo, nullptr, &lutView), "create LUT view");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = VkDeviceSize(size) * size * size * 8;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCheck(vkCreateBuffer(device, &bufferInfo, nullptr, &staging), "create LUT staging buffer");
        vkGetBufferMemoryRequirements(device, staging, &requirements);
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = owner->memoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(device, &allocate, nullptr, &stagingMemory), "allocate LUT staging buffer");
        vkCheck(vkBindBufferMemory(device, staging, stagingMemory, 0), "bind LUT staging buffer");
        vkCheck(vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &stagingData), "map LUT staging buffer");
        lutSize = size;
    }
    ~PresentPipeline() {
        VkDevice device = owner->device;
        destroyLut();
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
};
struct Swapchain {
    std::shared_ptr<Device> owner;
    VkSwapchainKHR handle{};
    VkExtent2D extent{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkSemaphore acquired{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> finished;
    bool inFlight = false;
    std::shared_ptr<Frame> lease;
    void retireLease() {
        if (lease) {
            --lease->readers;
            lease.reset();
        }
    }
    explicit Swapchain(std::shared_ptr<Device> value) : owner(std::move(value)) {}
    ~Swapchain() {
        retireLease();
        for (auto semaphore : finished)
            if (semaphore) vkDestroySemaphore(owner->device, semaphore, nullptr);
        if (acquired) vkDestroySemaphore(owner->device, acquired, nullptr);
        if (fence) vkDestroyFence(owner->device, fence, nullptr);
        if (pool) vkDestroyCommandPool(owner->device, pool, nullptr);
        if (handle) vkDestroySwapchainKHR(owner->device, handle, nullptr);
    }
};
}  // namespace
struct VulkanWindow::Impl {
    std::shared_ptr<Device> device;
    std::shared_ptr<Bridge> bridge;
    std::shared_ptr<Swapchain> swap;
    std::shared_ptr<PresentPipeline> pipeline;
    std::shared_ptr<Frame> frame;
    // The LUT, as half RGBA, kept for a pipeline made again after releaseResources();
    // lutChanged until the next presentation has uploaded it.
    int lutSize = 0;
    std::vector<uint16_t> lut;
    bool lutChanged = false;
    void retirePipeline() {
        if (!pipeline) return;
        auto old = std::exchange(pipeline, {});
        Retirement::instance().add([old]() mutable {
            {
                std::lock_guard lock(old->owner->queueMutex);
                vkDeviceWaitIdle(old->owner->device);
            }
            old.reset();
        });
    }
    std::shared_ptr<std::atomic<bool>> drained;
    bool recreate = false;
    QString error, description;
    std::shared_ptr<std::atomic<int>> errors;
    void retireSwapchain() {
        if (!swap) return;
        auto old = std::exchange(swap, {});
        drained = std::make_shared<std::atomic<bool>>(false);
        auto done = drained;
        Retirement::instance().add([old, done]() mutable {
            {
                std::lock_guard lock(old->owner->queueMutex);
                vkDeviceWaitIdle(old->owner->device);
            }
            old.reset();
            done->store(true, std::memory_order_release);
        });
    }
    void createSwapchain(QWindow& window) {
        auto value = std::make_shared<Swapchain>(device);
        VkSurfaceCapabilitiesKHR caps{};
        vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical, device->surface, &caps),
                "surface capabilities");
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("Surface does not support transfer presentation");
        uint32_t count = 0;
        vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical, device->surface, &count, nullptr),
                "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical, device->surface, &count, formats.data()),
                "surface formats");
        auto found = std::find_if(formats.begin(), formats.end(), [](auto& f) {
            return f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                   (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM);
        });
        if (found == formats.end()) throw std::runtime_error("No RGBA8 swapchain format");
        VkFormatProperties source{}, target{};
        vkGetPhysicalDeviceFormatProperties(device->physical, VK_FORMAT_R8G8B8A8_UNORM, &source);
        vkGetPhysicalDeviceFormatProperties(device->physical, found->format, &target);
        if (!(source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(source.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
            !(target.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            throw std::runtime_error("GPU does not support RGBA presentation blits");
        value->extent = caps.currentExtent;
        if (value->extent.width == UINT32_MAX) {
            value->extent.width = std::clamp(uint32_t(window.width() * window.devicePixelRatio()),
                                             caps.minImageExtent.width, caps.maxImageExtent.width);
            value->extent.height = std::clamp(uint32_t(window.height() * window.devicePixelRatio()),
                                              caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!value->extent.width || !value->extent.height) return;
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = device->surface;
        info.minImageCount = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        info.imageFormat = found->format;
        info.imageColorSpace = found->colorSpace;
        info.imageExtent = value->extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        vkCheck(vkCreateSwapchainKHR(device->device, &info, nullptr, &value->handle), "create swapchain");
        vkCheck(vkGetSwapchainImagesKHR(device->device, value->handle, &count, nullptr), "swapchain images");
        value->images.resize(count);
        vkCheck(vkGetSwapchainImagesKHR(device->device, value->handle, &count, value->images.data()),
                "swapchain images");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(device->device, &sem, nullptr, &value->acquired), "create acquire semaphore");
        value->finished.resize(count);
        for (auto& semaphore : value->finished)
            vkCheck(vkCreateSemaphore(device->device, &sem, nullptr, &semaphore), "create present semaphore");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = device->family;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCheck(vkCreateCommandPool(device->device, &pool, nullptr, &value->pool), "create command pool");
        VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmd.commandPool = value->pool;
        cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(device->device, &cmd, &value->command), "allocate presentation commands");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCheck(vkCreateFence(device->device, &fence, nullptr, &value->fence), "create presentation fence");
        swap = std::move(value);
    }
};
VulkanWindow::VulkanWindow() : m(std::make_unique<Impl>()) { setSurfaceType(QSurface::RasterSurface); }
VulkanWindow::~VulkanWindow() {
    m->retireSwapchain();
    m->retirePipeline();
}
void VulkanWindow::releaseResources() {
    m->frame.reset();
    m->retireSwapchain();
    m->retirePipeline();
    if (m->device && m->device->surface) {
        auto device = m->device;
        auto surface = std::exchange(device->surface, VK_NULL_HANDLE);
        // FIFO retirement puts this after the swapchain's GPU users. The CUDA
        // bridge may outlive the panel without retaining its native surface.
        Retirement::instance().add([device, surface] { vkDestroySurfaceKHR(device->instance, surface, nullptr); });
    }
    m->bridge.reset();
    m->device.reset();
}
bool VulkanWindow::retirementIdle() { return Retirement::instance().idle(); }
int VulkanWindow::validationErrors() const { return m->errors ? m->errors->load() : 0; }
QString VulkanWindow::error() const { return m->error; }
std::shared_ptr<GpuBridge> VulkanWindow::bridge() const { return m->bridge; }
QString VulkanWindow::initialize() {
    if (m->bridge) return m->description;
    if (m->drained && !Retirement::instance().idle()) {
        m->error = "GPU presentation resources are still retiring; retry shortly.";
        return {};
    }
    try {
        auto device = std::shared_ptr<Device>(
            new Device, [](Device* value) { Retirement::instance().add([value] { delete value; }); });
        vkCheck(volkInitialize(), "load Vulkan");
        std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        const bool validation = qEnvironmentVariableIntValue("USDCC_RTX_VULKAN_VALIDATION") != 0;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        if (validation) {
            uint32_t count = 0;
            vkCheck(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate validation layers");
            std::vector<VkLayerProperties> layers(count);
            vkCheck(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate validation layers");
            if (std::none_of(layers.begin(), layers.end(),
                             [&](auto& value) { return !strcmp(value.layerName, layer); }))
                throw std::runtime_error("Requested Vulkan validation layer is not installed");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "usdcc RTX";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance.pApplicationInfo = &app;
        instance.enabledExtensionCount = uint32_t(extensions.size());
        instance.ppEnabledExtensionNames = extensions.data();
        instance.enabledLayerCount = validation ? 1 : 0;
        instance.ppEnabledLayerNames = validation ? &layer : nullptr;
        vkCheck(vkCreateInstance(&instance, nullptr, &device->instance), "create Vulkan instance");
        volkLoadInstance(device->instance);
        if (validation) {
            VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pUserData = device->errors.get();
            debug.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) -> VkBool32 {
                if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++*static_cast<std::atomic<int>*>(user);
                std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
                return VK_FALSE;
            };
            vkCheck(vkCreateDebugUtilsMessengerEXT(device->instance, &debug, nullptr, &device->debug),
                    "create validation callback");
        }
        VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface.hinstance = GetModuleHandleW(nullptr);
        surface.hwnd = reinterpret_cast<HWND>(winId());
        vkCheck(vkCreateWin32SurfaceKHR(device->instance, &surface, nullptr, &device->surface), "create Win32 surface");
        cudaCheck(device->cuda.cuInit(0), "initialize CUDA driver");
        int cudaCount = 0;
        cudaCheck(device->cuda.cuDeviceGetCount(&cudaCount), "enumerate CUDA devices");
        uint32_t count = 0;
        vkCheck(vkEnumeratePhysicalDevices(device->instance, &count, nullptr), "enumerate Vulkan devices");
        std::vector<VkPhysicalDevice> devices(count);
        vkCheck(vkEnumeratePhysicalDevices(device->instance, &count, devices.data()), "enumerate Vulkan devices");
        const char* required[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                  VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
        for (auto candidate : devices) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &id;
            vkGetPhysicalDeviceProperties2(candidate, &properties);
            if (properties.properties.vendorID != 0x10de || properties.properties.apiVersion < VK_API_VERSION_1_2)
                continue;
            uint32_t n = 0;
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> available(n);
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &n, available.data());
            if (!std::all_of(std::begin(required), std::end(required), [&](auto name) {
                    return std::any_of(available.begin(), available.end(),
                                       [&](auto& ext) { return !strcmp(name, ext.extensionName); });
                }))
                continue;
            CUdevice cudaDevice = -1;
            for (int i = 0; i < cudaCount; ++i) {
                CUuuid uuid{};
                cudaCheck(device->cuda.cuDeviceGetUuid(&uuid, i), "CUDA UUID");
                if (!memcmp(uuid.bytes, id.deviceUUID, VK_UUID_SIZE)) cudaDevice = i;
            }
            if (cudaDevice < 0) continue;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> families(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
            for (uint32_t i = 0; i < n; ++i) {
                VkBool32 present = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, device->surface, &present);
                if (!present || !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                device->physical = candidate;
                device->family = i;
                device->cudaDevice = cudaDevice;
                m->description = QStringLiteral("%1; Vulkan/CUDA UUID matched; GPU-only presentation")
                                     .arg(properties.properties.deviceName);
                break;
            }
            if (device->physical) break;
        }
        if (!device->physical)
            throw std::runtime_error("No NVIDIA Vulkan/CUDA UUID-matched device supporting Win32 interop");
        float priority = 1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = device->family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = 3;
        info.ppEnabledExtensionNames = required;
        vkCheck(vkCreateDevice(device->physical, &info, nullptr, &device->device), "create Vulkan device");
        vkGetDeviceQueue(device->device, device->family, 0, &device->queue);
        cudaCheck(device->cuda.cuDevicePrimaryCtxRetain(&device->context, device->cudaDevice),
                  "retain CUDA primary context");
        m->errors = device->errors;
        m->device = std::move(device);
        m->bridge = std::make_shared<Bridge>(m->device);
        return m->description;
    } catch (const std::exception& e) {
        m->error = QString::fromUtf8(e.what());
        m->bridge.reset();
        m->device.reset();
        return {};
    }
}
void VulkanWindow::setFrame(std::shared_ptr<GpuFrame> frame) {
    m->frame = std::dynamic_pointer_cast<Frame>(std::move(frame));
    requestUpdate();
}
void VulkanWindow::clear() { m->frame.reset(); }
void VulkanWindow::setLut(const usdcc::usd::DisplayLut& lut) {
    if (lut.size < 2 || lut.rgba.size() != size_t(lut.size) * lut.size * lut.size * 4) return;
    m->lutSize = lut.size;
    m->lut.resize(lut.rgba.size());
    std::transform(lut.rgba.begin(), lut.rgba.end(), m->lut.begin(), toHalf);
    m->lutChanged = true;
    requestUpdate();
}
std::shared_ptr<GpuFrame> VulkanWindow::currentFrame() const { return m->frame; }
bool VulkanWindow::event(QEvent* event) {
    if (event->type() == QEvent::UpdateRequest) {
        present();
        return true;
    }
    return QWindow::event(event);
}
void VulkanWindow::exposeEvent(QExposeEvent*) {
    if (isExposed()) requestUpdate();
}
void VulkanWindow::resizeEvent(QResizeEvent*) {
    m->recreate = true;
    requestUpdate();
}
void VulkanWindow::present() {
    if (!m->bridge || !isExposed() || !m->frame || width() < 1 || height() < 1) return;
    auto retry = [this] { QTimer::singleShot(16, this, [this] { requestUpdate(); }); };
    try {
        if (m->recreate) {
            m->retireSwapchain();
            m->recreate = false;
        }
        if (m->drained && !m->drained->load(std::memory_order_acquire)) {
            retry();
            return;
        }
        m->drained.reset();
        std::unique_lock queueLock(m->device->queueMutex, std::try_to_lock);
        if (!queueLock.owns_lock()) {
            retry();
            return;
        }
        if (!m->swap) m->createSwapchain(*this);
        if (!m->swap) return;
        auto& swap = *m->swap;
        if (swap.inFlight) {
            VkResult status = vkGetFenceStatus(m->device->device, swap.fence);
            if (status == VK_NOT_READY) {
                retry();
                return;
            }
            vkCheck(status, "poll presentation fence");
            swap.inFlight = false;
            swap.retireLease();
        }
        std::unique_lock frameLock(m->frame->access, std::try_to_lock);
        if (!frameLock.owns_lock()) {
            retry();
            return;
        }
        uint32_t index = 0;
        VkResult acquired =
            vkAcquireNextImageKHR(m->device->device, swap.handle, 0, swap.acquired, VK_NULL_HANDLE, &index);
        if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT) {
            retry();
            return;
        }
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            m->recreate = true;
            retry();
            return;
        }
        if (acquired != VK_SUBOPTIMAL_KHR) vkCheck(acquired, "acquire image");
        auto& frame = *m->frame;
        if (!m->pipeline) m->pipeline = std::make_shared<PresentPipeline>(m->device);
        auto& pass = *m->pipeline;
        // The LUT: up again when it changed or the pipeline is new; a black placeholder
        // until the panel sends one.
        const bool uploadLut = m->lutChanged || !pass.lutReady;
        if (uploadLut) {
            const int size = m->lut.empty() ? 2 : m->lutSize;
            pass.ensureLut(size);
            const size_t halves = size_t(size) * size * size * 4;
            if (m->lut.empty()) std::memset(pass.stagingData, 0, halves * 2);
            else std::memcpy(pass.stagingData, m->lut.data(), halves * 2);
            m->lutChanged = false;
        }
        vkCheck(vkResetCommandBuffer(swap.command, 0), "reset presentation commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkCheck(vkBeginCommandBuffer(swap.command, &begin), "begin presentation commands");
        if (uploadLut) {
            VkImageMemoryBarrier lutBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            lutBarrier.srcQueueFamilyIndex = lutBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            lutBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            lutBarrier.image = pass.lut;
            lutBarrier.oldLayout = pass.lutReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            lutBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            lutBarrier.srcAccessMask = pass.lutReady ? VK_ACCESS_SHADER_READ_BIT : 0;
            lutBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(swap.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lutBarrier);
            VkBufferImageCopy lutCopy{};
            lutCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            lutCopy.imageExtent = {uint32_t(pass.lutSize), uint32_t(pass.lutSize), uint32_t(pass.lutSize)};
            vkCmdCopyBufferToImage(swap.command, pass.staging, pass.lut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &lutCopy);
            lutBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            lutBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            lutBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            lutBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(swap.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &lutBarrier);
            pass.lutReady = true;
        }
        // The shared buffer, from CUDA, for the pass to read.
        VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        buffer.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        buffer.dstQueueFamilyIndex = m->device->family;
        buffer.buffer = frame.buffer;
        buffer.size = VK_WHOLE_SIZE;
        VkImageMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        barriers[0].image = frame.image;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(swap.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &buffer, 1, barriers);
        VkDescriptorBufferInfo pixels{frame.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo lutInfo{pass.sampler, pass.lutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo displayInfo{VK_NULL_HANDLE, frame.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = pass.set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &pixels;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &lutInfo;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo = &displayInfo;
        // Safe: the previous presentation's fence has signalled before this one records.
        vkUpdateDescriptorSets(m->device->device, 3, writes, 0, nullptr);
        struct {
            int32_t width, height, channels, componentBytes;
            float lutSize, minStops, maxStops;
            int32_t pad;
        } params{frame.size().width(), frame.size().height(), frame.channels, frame.componentBytes,
                 float(pass.lutSize), usdcc::usd::kLutMinStops, usdcc::usd::kLutMaxStops, 0};
        vkCmdBindPipeline(swap.command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
        vkCmdBindDescriptorSets(swap.command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layout, 0, 1, &pass.set, 0, nullptr);
        vkCmdPushConstants(swap.command, pass.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(swap.command, uint32_t((frame.size().width() + 15) / 16), uint32_t((frame.size().height() + 15) / 16), 1);
        buffer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        buffer.dstAccessMask = 0;
        buffer.srcQueueFamilyIndex = m->device->family;
        buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        vkCmdPipelineBarrier(swap.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 1, &buffer, 0, nullptr);
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image = swap.images[index];
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(swap.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, barriers);
        VkClearColorValue black{};
        black.float32[3] = 1;
        vkCmdClearColorImage(swap.command, swap.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &barriers[1].subresourceRange);
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(swap.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barriers[1]);
        const QSize target = frame.size().scaled(int(swap.extent.width), int(swap.extent.height), Qt::KeepAspectRatio);
        int x = (int(swap.extent.width) - target.width()) / 2, y = (int(swap.extent.height) - target.height()) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {frame.size().width(), frame.size().height(), 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + target.width(), y + target.height(), 1};
        vkCmdBlitImage(swap.command, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap.images[index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[1].dstAccessMask = 0;
        vkCmdPipelineBarrier(swap.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barriers[1]);
        vkCheck(vkEndCommandBuffer(swap.command), "end presentation commands");
        VkSemaphore waits[] = {swap.acquired, frame.ready};
        VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = frame.firstPresentation ? 2 : 1;
        submit.pWaitSemaphores = waits;
        submit.pWaitDstStageMask = stages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &swap.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &swap.finished[index];
        vkCheck(vkResetFences(m->device->device, 1, &swap.fence), "reset presentation fence");
        vkCheck(vkQueueSubmit(m->device->queue, 1, &submit, swap.fence), "submit presentation");
        frame.firstPresentation = false;
        swap.inFlight = true;
        swap.lease = m->frame;
        ++frame.readers;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &swap.finished[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swap.handle;
        present.pImageIndices = &index;
        VkResult shown = vkQueuePresentKHR(m->device->queue, &present);
        if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR) {
            m->recreate = true;
            retry();
        } else
            vkCheck(shown, "present RTX image");
        if (presented) presented(frame.size(), {});
        // Poll retirement but don't redraw the same frame continuously.
        QTimer::singleShot(16, this, [this] { pollPresentation(); });
    } catch (const std::exception& e) {
        m->error = QString::fromUtf8(e.what());
        if (presented) presented({}, m->error);
    }
}
void VulkanWindow::pollPresentation() {
    if (!m->swap || !m->swap->inFlight) return;
    const VkResult status = vkGetFenceStatus(m->device->device, m->swap->fence);
    if (status == VK_NOT_READY) {
        QTimer::singleShot(16, this, [this] { pollPresentation(); });
        return;
    }
    if (status == VK_SUCCESS) {
        m->swap->inFlight = false;
        m->swap->retireLease();
    } else {
        m->error = QStringLiteral("Presentation fence failed: Vulkan %1").arg(status);
        m->retireSwapchain();
        if (presented) presented({}, m->error);
    }
}
}  // namespace usdcc::rtx
