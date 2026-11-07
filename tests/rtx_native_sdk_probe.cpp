// Explicit opt-in GPU diagnostic. Python and the USD app are not involved.
#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "cuda_driver.h"
#include "rtx_engine.h"
#include "vulkan_window.h"

// Deterministic GPU-only presenter test. CPU upload/readback exists only in this
// diagnostic; it neither initializes RTX nor counts as an RTX image proof.
int interop(QGuiApplication& app, const QString& screenshot) {
    using namespace usdcc::rtx;
    VulkanWindow window;
    window.resize(640, 360);
    window.show();
    if (window.initialize().isEmpty()) {
        std::fprintf(stderr, "%s\n", window.error().toUtf8().constData());
        return 1;
    }
    auto bridge = window.bridge();
    const int deviceId = bridge->deviceId();
    Cuda cuda;
    CUcontext context{};
    cudaCheck(cuda.cuDevicePrimaryCtxRetain(&context, bridge->deviceId()), "retain test CUDA context");
    CudaContext current(cuda, context);
    QImage reference(64, 32, QImage::Format_RGBA8888);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) reference.setPixelColor(x, y, QColor(x * 4, y * 8, 128, 255));
    CUdeviceptr pixels{};
    cudaCheck(cuda.cuMemAlloc(&pixels, reference.sizeInBytes()), "allocate test GPU pixels");
    cudaCheck(cuda.cuMemcpyHtoD(pixels, reference.constBits(), reference.sizeInBytes()), "upload diagnostic pixels");
    int64_t shape[] = {32, 64, 4}, strides[] = {256, 4, 1};
    DLTensor tensor{};
    tensor.data = reinterpret_cast<void*>(pixels);
    tensor.device = {kDLCUDA, bridge->deviceId()};
    tensor.ndim = 3;
    tensor.dtype = {kDLUInt, 8, 1};
    tensor.shape = shape;
    tensor.strides = strides;
    // All three GPU leases fill the ring. Release them without presentation to
    // exercise consuming abandoned CUDA-ready signals on the next reuse.
    std::array<std::shared_ptr<GpuFrame>, 3> held;
    for (auto& image : held) image = bridge->copy(tensor);
    if (bridge->copy(tensor)) throw std::runtime_error("GPU ring exceeded three frames");
    for (auto& image : held)
        while (!image->copied()) QThread::msleep(1);
    held = {};
    QTemporaryDir folder;
    std::shared_ptr<GpuFrame> frame;
    int published = 0, tick = 0;
    bool passed = false;
    QTimer retirement;
    QObject::connect(&retirement, &QTimer::timeout, &app, [&] {
        if (VulkanWindow::retirementIdle()) {
            passed = window.validationErrors() == 0;
            retirement.stop();
            app.quit();
        }
    });
    window.presented = [&](QSize size, const QString& error) {
        if (!error.isEmpty()) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            app.exit(1);
            return;
        }
        if (size != QSize(64, 32)) {
            app.exit(1);
            return;
        }
        ++published;
    };
    QTimer producer;
    QObject::connect(&producer, &QTimer::timeout, &app, [&] {
        try {
            if (frame && !frame->copied()) return;
            if (frame) {
                auto path = folder.filePath("roundtrip.png");
                frame->save(path);
                if (QImage(path).convertToFormat(QImage::Format_RGBA8888) != reference)
                    throw std::runtime_error("GPU roundtrip pixels differ");
                window.setFrame(std::exchange(frame, {}));
            }
            if (++tick == 6) window.resize(720, 400);
            if (tick == 10) window.hide();
            if (tick == 12) window.show();
            if (tick >= 25 && published >= 10) {
                if (!screenshot.isEmpty() && !window.screen()->grabWindow(window.winId()).save(screenshot))
                    throw std::runtime_error("Could not capture the diagnostic window");
                producer.stop();
                frame.reset();
                window.clear();
                bridge.reset();
                window.releaseResources();
                retirement.start(10);
                return;
            }
            frame = bridge->copy(tensor);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            producer.stop();
            app.exit(1);
        }
    });
    producer.start(30);
    QTimer::singleShot(15000, &app, [&] {
        if (!passed) {
            std::fprintf(stderr, "Interop diagnostic timed out\n");
            app.exit(1);
        }
    });
    app.exec();
    cuda.cuMemFree(pixels);
    cuda.cuDevicePrimaryCtxRelease(deviceId);
    std::printf("Interop diagnostic: %s; %d presentations, resize/hide/show, exact RGBA roundtrip\n",
                passed ? "PASS" : "FAIL", published);
    return passed ? 0 : 1;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if ((argc == 2 || argc == 3) && QString::fromLocal8Bit(argv[1]) == "--interop") {
        try {
            return interop(app, argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString{});
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr, "Usage: rtx_native_sdk_probe ovrtx-root ovstage-root scene.usda /RenderProduct [image.png]\n");
        return 2;
    }
    usdcc::rtx::VulkanWindow window;
    window.resize(960, 540);
    window.show();
    auto device = window.initialize();
    if (device.isEmpty()) {
        std::fprintf(stderr, "%s\n", window.error().toUtf8().constData());
        return 1;
    }
    std::printf("%s\n", device.toUtf8().constData());
    auto engine = std::make_unique<usdcc::rtx::RtxEngine>(
        QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]), window.bridge());
    auto scene = std::make_shared<usdcc::rtx::Snapshot>();
    scene->file = QString::fromLocal8Bit(argv[3]);
    scene->product = QString::fromLocal8Bit(argv[4]);
    scene->size = QSize(960, 540);
    engine->replace(scene, 1);
    engine->setActive(true);
    QString last;
    bool draining = false, imageReceived = false;
    int result = 0;
    const auto finish = [&](int code) {
        result = code;
        draining = true;
        engine.reset();
        window.clear();
        window.releaseResources();
        usdcc::rtx::RtxEngine::shutdown();
    };
    window.presented = [&](QSize size, const QString& error) {
        if (draining || imageReceived) return;
        if (!error.isEmpty()) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            finish(1);
            return;
        }
        imageReceived = true;
        std::printf("GPU image %dx%d\n", size.width(), size.height());
        std::fflush(stdout);
        if (argc == 6)
            engine->save(QString::fromLocal8Bit(argv[5]), window.currentFrame());
        else
            finish(0);
    };
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (draining) {
            if (usdcc::rtx::RtxEngine::shutdownFinished() && usdcc::rtx::VulkanWindow::retirementIdle()) {
                if (window.validationErrors()) result = 1;
                std::printf("SDK/GPU drain complete; Vulkan validation errors: %d\n", window.validationErrors());
                app.exit(result);
            }
            return;
        }
        auto update = engine->poll();
        if (last != update.status) {
            last = update.status;
            std::printf("%s\n", last.toUtf8().constData());
            std::fflush(stdout);
        }
        if (!update.error.isEmpty()) {
            std::fprintf(stderr, "%s\n", update.error.toUtf8().constData());
            finish(1);
            return;
        }
        if (!update.saveError.isEmpty()) {
            std::fprintf(stderr, "%s\n", update.saveError.toUtf8().constData());
            finish(1);
            return;
        }
        if (!update.saved.isEmpty()) {
            std::printf("Saved RTX image: %s\n", update.saved.toUtf8().constData());
            finish(0);
            return;
        }
        if (update.frame) window.setFrame(std::move(update.frame));
    });
    poll.start(30);
    // A new executable/driver can have a cold shader cache even when Python's
    // cache is warm. Attachment alone took ~100 seconds on the validation GPU.
    // Keep a bounded diagnostic, but do not classify initial compilation as a
    // hang after only 60 seconds. Production rendering has no such time limit.
    bool timeoutValid = false;
    const int configuredTimeout = qEnvironmentVariableIntValue("USDCC_RTX_PROBE_TIMEOUT_SECONDS", &timeoutValid);
    const int timeoutSeconds = timeoutValid && configuredTimeout > 0 && configuredTimeout <= 3600
                                   ? configuredTimeout
                                   : 600;
    QTimer::singleShot(timeoutSeconds * 1000, &app, [] {
        std::fprintf(stderr, "SDK probe timed out; inspect RTX compilation progress before diagnosing a hang\n");
        std::fflush(stderr);
        std::_Exit(1);
    });
    return app.exec();
}
