// RTX for final renders (rtx_final.h) with a fake engine: what the scene file gets (ovrtx's
// product attributes, the HdrColor var, the private camera, the user's RenderSettings), the
// settings and budget it renders with, the frame read back, a setup problem, and a camera at a
// frame. No CUDA, vendor headers, runtime or GPU.
#include "rtx_final.h"
#include "rtx_panel.h"
#include "test_harness.h"

#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_spec.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/tokens.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::rtx;
using namespace usdcc::render;

namespace {
// A frame on the CPU: R = x / width, G = y / height.
struct CpuFrame : GpuFrame {
    QSize dimensions;
    explicit CpuFrame(QSize size) : dimensions(size) {}
    QSize size() const override { return dimensions; }
    bool copied() const override { return true; }
    void save(const QString&) const override {}
    bool readLinear(std::vector<float>& rgba) const override {
        rgba.assign(size_t(dimensions.width()) * size_t(dimensions.height()) * 4, 1.0f);
        for (int y = 0; y < dimensions.height(); ++y)
            for (int x = 0; x < dimensions.width(); ++x) {
                float* p = rgba.data() + (size_t(y) * size_t(dimensions.width()) + size_t(x)) * 4;
                p[0] = float(x) / float(dimensions.width());
                p[1] = float(y) / float(dimensions.height());
                p[2] = 0.25f;
            }
        return true;
    }
};

struct Seen {
    std::shared_ptr<Snapshot> snapshot;
    int budget = 0;
    bool active = false, visible = false, closed = false;
    int polls = 0;
    bool fail = false;
};

// Done on its third poll, with a frame of the snapshot's size.
class FakeEngine final : public Engine {
public:
    explicit FakeEngine(std::shared_ptr<Seen> seen) : m(std::move(seen)) {}
    uint64_t replace(std::shared_ptr<Snapshot> snapshot, int budget) override {
        m->snapshot = std::move(snapshot);
        m->budget = budget;
        return 1;
    }
    void update(Camera, QSize, const std::map<QString, std::array<double, 16>>&) override {}
    void setSelection(QStringList) override {}
    void setActive(bool on) override { m->active = on; }
    void setVisible(bool on) override { m->visible = on; }
    void save(QString, std::shared_ptr<GpuFrame>) override {}
    EngineUpdate poll() override {
        EngineUpdate update;
        ++m->polls;
        if (m->fail) {
            update.error = "fake failure";
            return update;
        }
        update.status = "Rendering";
        if (m->polls >= 3 && m->snapshot) {
            update.frame = std::make_shared<CpuFrame>(m->snapshot->size);
            update.done = true;
        }
        return update;
    }
    uint64_t generation() const override { return 1; }
    void close() override { m->closed = true; }
    bool isClosed() const override { return m->closed; }

private:
    std::shared_ptr<Seen> m;
};

bool waitUntil(const std::function<bool()>& done, int ms) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

bool ended(const RenderJobPtr& job) {
    return job->status == RenderJob::Status::Done || job->status == RenderJob::Status::Failed ||
           job->status == RenderJob::Status::Cancelled;
}
}  // namespace

USDCC_TEST(rtx_native_final) {
    defineFinalRenderSettings();
    auto seen = std::make_shared<Seen>();
    std::optional<QString> problem;
    auto renderer = createRtxFinalRenderer([seen]() { return std::make_unique<FakeEngine>(seen); },
                                           [&problem]() { return problem; });
    FinalRenderers::instance().add(renderer);
    CHECK(renderer->id() == kRtxFinalRendererId && renderer->namespaces() == std::vector<std::string>{"omni"},
          "RTX, in the omni namespace");
    CHECK(!renderer->capabilities().tiles, "whole frames only");
    const auto schema = renderer->settingsSchema();
    CHECK(schema.size() == 4 && schema[0].attribute == kRtxModeAttribute && schema[1].attribute == kRtxSamplesAttribute,
          "its settings: mode, samples, light matching");

    UsdStageRefPtr stage = UsdStage::CreateInMemory("rtx_final.usda");
    UsdGeomCube::Define(stage, SdfPath("/Box"));
    UsdGeomCamera camera = UsdGeomCamera::Define(stage, SdfPath("/Cam"));
    UsdGeomXformCommonAPI(camera).SetTranslate(GfVec3d(0, 0, 5), UsdTimeCode(1));
    UsdGeomXformCommonAPI(camera).SetTranslate(GfVec3d(0, 0, 9), UsdTimeCode(5));
    camera.CreateHorizontalApertureAttr().Set(36.0f);
    camera.CreateVerticalApertureAttr().Set(24.0f);
    camera.CreateFocalLengthAttr().Set(50.0f);
    const SdfPath settings = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(settings);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(40, 20));
    setJobOption(prim, jobattr::renderer, VtValue(std::string(kRtxFinalRendererId)));
    prim.CreateAttribute(TfToken(kRtxModeAttribute), SdfValueTypeNames->Token, true).Set(TfToken("RealTimePathTracing"));
    prim.CreateAttribute(TfToken(kRtxSamplesAttribute), SdfValueTypeNames->Int, true).Set(7);
    prim.CreateAttribute(TfToken(kRtxMatchLightsAttribute), SdfValueTypeNames->Bool, true).Set(false);
    QTemporaryDir dir;
    setJobOption(prim, jobattr::output, VtValue((dir.path() + "/<stage>").toStdString()));

    // The camera at a frame: its translation then.
    const usdcc::usd::RenderCamera atFive = finalCamera(stage, SdfPath("/Cam"), 5, 2.0);
    CHECK(std::abs(atFive.cameraToWorld.ExtractTranslation()[2] - 9.0) < 1e-9, "the camera where it is at frame 5");
    CHECK(std::abs(atFive.aspect - 2.0) < 1e-9 && atFive.prim == SdfPath("/Cam"), "at the image's aspect");

    JobRequest request;
    request.stage = stage;
    request.currentTime = 5;
    std::string error;
    RenderJobPtr job = makeRenderJob(request, &error);
    CHECK(job != nullptr, "a job: %s", error.c_str());
    if (!job) return;
    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    QImage frame;
    QObject::connect(&scheduler, &RenderScheduler::frameRendered, [&frame](const QString&, int, const QImage& image) { frame = image; });
    scheduler.submit(job);
    CHECK(waitUntil([&job]() { return ended(job); }, 10000) && job->status == RenderJob::Status::Done, "rendered: %s",
          job->error.c_str());
    CHECK(seen->active && seen->visible && seen->closed, "the engine was started, and closed after");
    CHECK(seen->budget == 7, "with the prim's budget: %d", seen->budget);
    CHECK(seen->snapshot && seen->snapshot->mode == Mode::RealTime && !seen->snapshot->light.on, "its mode and no light matching");
    CHECK(seen->snapshot && seen->snapshot->size == QSize(40, 20) && seen->snapshot->colorVar == "HdrColor", "its size and HdrColor");
    bool exact = frame.size() == QSize(40, 20);
    for (int y = 0; y < 20 && exact; ++y) {
        const float* row = reinterpret_cast<const float*>(frame.constScanLine(y));
        for (int x = 0; x < 40 && exact; ++x) exact = std::abs(row[x * 4] - x / 40.0f) < 1e-6f && row[x * 4 + 2] == 0.25f;
    }
    CHECK(exact, "the frame read back is the frame");

    // What ovrtx was given. The file goes with the task, so it's read from a scene made the same way.
    if (seen->snapshot) {
        const QString product = seen->snapshot->product;
        const QString cameraPath = seen->snapshot->camera;
        CHECK(cameraPath.startsWith("/__usdccRtxFinal"), "a private camera: %s", qPrintable(cameraPath));
        CHECK(!product.isEmpty(), "the product: %s", qPrintable(product));
    }

    // A setup problem: no job.
    problem = QStringLiteral("Native RTX runtime missing");
    CHECK(!makeRenderJob(request, &error) && error.find("runtime missing") != std::string::npos, "a missing runtime says so: %s",
          error.c_str());
    problem.reset();

    // A failure from the engine fails the task.
    seen->fail = true;
    RenderJobPtr failing = makeRenderJob(request, &error);
    if (failing) {
        scheduler.submit(failing);
        CHECK(waitUntil([&failing]() { return ended(failing); }, 10000) && failing->status == RenderJob::Status::Failed &&
                  failing->error.find("fake failure") != std::string::npos,
              "an engine error fails it: %s", failing->error.c_str());
    }
    FinalRenderers::instance().remove(kRtxFinalRendererId);
}

USDCC_TEST(rtx_native_final_scene) {
    // The scene's RTX parts, from a renderer whose engine keeps the file.
    auto seen = std::make_shared<Seen>();
    std::string sceneText;
    auto renderer = createRtxFinalRenderer(
        [seen, &sceneText]() {
            // The snapshot arrives in replace(): the file is still there then.
            struct Keeping final : Engine {
                std::shared_ptr<Seen> m;
                std::string* text;
                uint64_t replace(std::shared_ptr<Snapshot> s, int budget) override {
                    auto layer = SdfLayer::FindOrOpen(s->file.toStdString());
                    if (layer) layer->ExportToString(text);
                    m->snapshot = std::move(s);
                    m->budget = budget;
                    return 1;
                }
                void update(Camera, QSize, const std::map<QString, std::array<double, 16>>&) override {}
                void setSelection(QStringList) override {}
                void setActive(bool) override {}
                void setVisible(bool) override {}
                void save(QString, std::shared_ptr<GpuFrame>) override {}
                EngineUpdate poll() override {
                    EngineUpdate u;
                    u.frame = std::make_shared<CpuFrame>(m->snapshot->size);
                    u.done = true;
                    return u;
                }
                uint64_t generation() const override { return 1; }
                void close() override {}
                bool isClosed() const override { return false; }
            };
            auto engine = std::make_unique<Keeping>();
            engine->m = seen;
            engine->text = &sceneText;
            return engine;
        },
        []() { return std::optional<QString>(); });
    FinalRenderers::instance().add(renderer);
    UsdStageRefPtr stage = UsdStage::CreateInMemory("rtx_final_scene.usda");
    UsdGeomCamera::Define(stage, SdfPath("/Cam"));
    const SdfPath settings = createRenderSettings(stage);
    setJobOption(stage->GetPrimAtPath(settings), jobattr::renderer, VtValue(std::string(kRtxFinalRendererId)));
    UsdRenderSettings(stage->GetPrimAtPath(settings)).GetResolutionAttr().Set(GfVec2i(16, 8));
    QTemporaryDir dir;
    setJobOption(stage->GetPrimAtPath(settings), jobattr::output, VtValue((dir.path() + "/<stage>").toStdString()));
    JobRequest request;
    request.stage = stage;
    RenderJobPtr job = makeRenderJob(request, nullptr);
    if (!job) {
        CHECK(false, "a job");
        return;
    }
    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    scheduler.submit(job);
    waitUntil([&job]() { return ended(job); }, 10000);
    CHECK(job->status == RenderJob::Status::Done, "rendered: %s", job->error.c_str());
    CHECK(sceneText.find("token omni:rtx:rendermode = \"PathTracing\"") != std::string::npos, "the product's mode, Path Tracing by default");
    CHECK(sceneText.find("def RenderVar \"HdrColor\"") != std::string::npos, "an HdrColor var");
    CHECK(sceneText.find("renderSettingsPrimPath = \"/Render/Settings\"") != std::string::npos, "the stage's own RenderSettings");
    CHECK(sceneText.find("/__usdccRtxFinal/Camera") != std::string::npos, "the private camera");
    CHECK(seen->budget == 128, "Settings' sample budget without one on the prim: %d", seen->budget);
    FinalRenderers::instance().remove(kRtxFinalRendererId);
}
