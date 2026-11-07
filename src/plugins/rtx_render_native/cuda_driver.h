#pragma once
#include <cuda.h>
#include <windows.h>

#include <stdexcept>
#include <string>
namespace usdcc::rtx {
struct Cuda {
#define USDCC_CUDA_FN(name, symbol) decltype(&name) name = nullptr;
    USDCC_CUDA_FN(cuInit, "cuInit")
    USDCC_CUDA_FN(cuDeviceGetCount, "cuDeviceGetCount")
    USDCC_CUDA_FN(cuDeviceGetUuid, "cuDeviceGetUuid")
    USDCC_CUDA_FN(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain")
    USDCC_CUDA_FN(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2")
    USDCC_CUDA_FN(cuCtxPushCurrent, "cuCtxPushCurrent_v2")
    USDCC_CUDA_FN(cuCtxPopCurrent, "cuCtxPopCurrent_v2")
    USDCC_CUDA_FN(cuStreamCreate, "cuStreamCreate")
    USDCC_CUDA_FN(cuStreamDestroy, "cuStreamDestroy_v2")
    USDCC_CUDA_FN(cuStreamSynchronize, "cuStreamSynchronize")
    USDCC_CUDA_FN(cuEventCreate, "cuEventCreate")
    USDCC_CUDA_FN(cuEventRecord, "cuEventRecord")
    USDCC_CUDA_FN(cuEventQuery, "cuEventQuery")
    USDCC_CUDA_FN(cuEventDestroy, "cuEventDestroy_v2")
    USDCC_CUDA_FN(cuImportExternalMemory, "cuImportExternalMemory")
    USDCC_CUDA_FN(cuExternalMemoryGetMappedBuffer, "cuExternalMemoryGetMappedBuffer")
    USDCC_CUDA_FN(cuDestroyExternalMemory, "cuDestroyExternalMemory")
    USDCC_CUDA_FN(cuImportExternalSemaphore, "cuImportExternalSemaphore")
    USDCC_CUDA_FN(cuDestroyExternalSemaphore, "cuDestroyExternalSemaphore")
    USDCC_CUDA_FN(cuSignalExternalSemaphoresAsync, "cuSignalExternalSemaphoresAsync")
    USDCC_CUDA_FN(cuWaitExternalSemaphoresAsync, "cuWaitExternalSemaphoresAsync")
    USDCC_CUDA_FN(cuMemcpy2DAsync, "cuMemcpy2DAsync_v2")
    USDCC_CUDA_FN(cuMemcpy2D, "cuMemcpy2D_v2")
    USDCC_CUDA_FN(cuMemFree, "cuMemFree_v2")
    USDCC_CUDA_FN(cuMemAlloc, "cuMemAlloc_v2")
    USDCC_CUDA_FN(cuMemcpyHtoD, "cuMemcpyHtoD_v2")
#undef USDCC_CUDA_FN
    Cuda() {
        static auto module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("NVIDIA CUDA driver is unavailable");
#define USDCC_CUDA_FN(name, symbol)                                          \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(module, symbol)); \
    if (!name) throw std::runtime_error("CUDA driver is missing " symbol);
        USDCC_CUDA_FN(cuInit, "cuInit")
        USDCC_CUDA_FN(cuDeviceGetCount, "cuDeviceGetCount")
        USDCC_CUDA_FN(cuDeviceGetUuid, "cuDeviceGetUuid")
        USDCC_CUDA_FN(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain")
        USDCC_CUDA_FN(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2")
        USDCC_CUDA_FN(cuCtxPushCurrent, "cuCtxPushCurrent_v2")
        USDCC_CUDA_FN(cuCtxPopCurrent, "cuCtxPopCurrent_v2")
        USDCC_CUDA_FN(cuStreamCreate, "cuStreamCreate")
        USDCC_CUDA_FN(cuStreamDestroy, "cuStreamDestroy_v2")
        USDCC_CUDA_FN(cuStreamSynchronize, "cuStreamSynchronize")
        USDCC_CUDA_FN(cuEventCreate, "cuEventCreate")
        USDCC_CUDA_FN(cuEventRecord, "cuEventRecord")
        USDCC_CUDA_FN(cuEventQuery, "cuEventQuery")
        USDCC_CUDA_FN(cuEventDestroy, "cuEventDestroy_v2")
        USDCC_CUDA_FN(cuImportExternalMemory, "cuImportExternalMemory")
        USDCC_CUDA_FN(cuExternalMemoryGetMappedBuffer, "cuExternalMemoryGetMappedBuffer")
        USDCC_CUDA_FN(cuDestroyExternalMemory, "cuDestroyExternalMemory")
        USDCC_CUDA_FN(cuImportExternalSemaphore, "cuImportExternalSemaphore")
        USDCC_CUDA_FN(cuDestroyExternalSemaphore, "cuDestroyExternalSemaphore")
        USDCC_CUDA_FN(cuSignalExternalSemaphoresAsync, "cuSignalExternalSemaphoresAsync")
        USDCC_CUDA_FN(cuWaitExternalSemaphoresAsync, "cuWaitExternalSemaphoresAsync")
        USDCC_CUDA_FN(cuMemcpy2DAsync, "cuMemcpy2DAsync_v2")
        USDCC_CUDA_FN(cuMemcpy2D, "cuMemcpy2D_v2")
        USDCC_CUDA_FN(cuMemFree, "cuMemFree_v2")
        USDCC_CUDA_FN(cuMemAlloc, "cuMemAlloc_v2")
        USDCC_CUDA_FN(cuMemcpyHtoD, "cuMemcpyHtoD_v2")
#undef USDCC_CUDA_FN
    }
};
inline void cudaCheck(CUresult code, const char* operation) {
    if (code != CUDA_SUCCESS) throw std::runtime_error(std::string(operation) + ": CUDA error " + std::to_string(code));
}
struct CudaContext {
    Cuda& api;
    CudaContext(Cuda& api, CUcontext context) : api(api) {
        cudaCheck(api.cuCtxPushCurrent(context), "cuCtxPushCurrent");
    }
    ~CudaContext() {
        CUcontext old{};
        api.cuCtxPopCurrent(&old);
    }
};
}  // namespace usdcc::rtx
