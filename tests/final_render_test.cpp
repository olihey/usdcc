// Final-frame rendering (docs/FINAL_RENDER.md): a RenderSettings prim read and written, tiles,
// output names, the scheduler with a renderer made for the test (tiles stitched, retries,
// cancelling, files written), and a real render with Embree, which needs no GPU.
#include "test_harness.h"

#include "usdcc/usd/linear_exr.h"
#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/render/hydra_final_renderer.h"
#include "usdcc/usd/render/render_job.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/render/render_spec.h"
#include "usdcc/usd/render/scene_file_renderer.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::render;

namespace {

// Fills a task's region with where each pixel is in the whole image: R = x / width,
// G = y / height (top row 0), B = the frame, A = 1. Optionally fails tasks first.
class GradientRenderer : public FinalRenderer {
public:
    std::string id() const override { return "test.gradient"; }
    std::string name() const override { return "Gradient"; }
    Capabilities capabilities() const override { return {tiles, true}; }
    std::vector<std::string> namespaces() const override { return {"gradient"}; }
    std::vector<RenderSettingField> settingsSchema() const override {
        RenderSettingField field;
        field.attribute = "gradient:boost";
        field.rendererKey = TfToken("boost");
        field.label = "Boost";
        field.type = RenderSettingField::Type::Float;
        field.defaultValue = VtValue(1.0f);
        return {field};
    }

    struct Handle : RenderTaskHandle {
        bool* cancelled;
        explicit Handle(bool* c) : cancelled(c) {}
        void cancel() override { *cancelled = true; }
    };

    std::unique_ptr<RenderTaskHandle> start(const RenderTask& task, TaskCallbacks callbacks) override {
        ++started;
        lastSettings = task.settings;
        regions.insert({task.region.x, task.region.y, task.region.width, task.region.height});
        if (failFirst > 0) {
            --failFirst;
            callbacks.failed("failed on purpose");
            return nullptr;
        }
        if (failAlways) {
            callbacks.failed("always fails");
            return nullptr;
        }
        if (hang) return std::make_unique<Handle>(&cancelled);
        FloatImage color(task.region.width, task.region.height);
        for (int y = 0; y < task.region.height; ++y) {
            for (int x = 0; x < task.region.width; ++x) {
                float* p = color.row(y) + x * 4;
                p[0] = float(task.region.x + x) / float(task.resolution[0]);
                p[1] = float(task.region.y + y) / float(task.resolution[1]);
                p[2] = float(task.frame);
                p[3] = 1.0f;
            }
        }
        std::map<std::string, FloatImage> images;
        for (const RenderAov& aov : task.aovs) images[aov.name] = color;
        callbacks.progress(0.5, "Half way");
        callbacks.finished(std::move(images));
        return std::make_unique<Handle>(&cancelled);
    }

    bool tiles = true;
    int failFirst = 0;
    bool failAlways = false;
    bool hang = false;
    bool cancelled = false;
    int started = 0;
    std::map<TfToken, VtValue> lastSettings;
    struct Rect {
        int x, y, w, h;
        bool operator<(const Rect& o) const { return std::tie(x, y, w, h) < std::tie(o.x, o.y, o.w, o.h); }
    };
    std::set<Rect> regions;
};

// A stage with a camera looking down -Z at a sphere a little above the middle.
UsdStageRefPtr testStage() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory("shot.usda");
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(3);
    UsdGeomSphere sphere = UsdGeomSphere::Define(stage, SdfPath("/World/Ball"));
    sphere.CreateRadiusAttr().Set(1.0);
    UsdGeomXformCommonAPI(sphere).SetTranslate(GfVec3d(0, 1.2, 0));
    UsdGeomCamera camera = UsdGeomCamera::Define(stage, SdfPath("/World/Cam"));
    UsdGeomXformCommonAPI(camera).SetTranslate(GfVec3d(0, 0, 10));
    camera.CreateFocalLengthAttr().Set(35.0f);
    camera.CreateHorizontalApertureAttr().Set(36.0f);
    camera.CreateVerticalApertureAttr().Set(27.0f);
    return stage;
}

// Runs the event loop until `done` or `ms` pass.
bool waitFor(const std::function<bool()>& done, int ms) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

bool ended(const RenderJobPtr& job) {
    return job->status == RenderJob::Status::Done || job->status == RenderJob::Status::Failed ||
           job->status == RenderJob::Status::Cancelled;
}

struct Registered {
    std::shared_ptr<GradientRenderer> renderer = std::make_shared<GradientRenderer>();
    Registered() { FinalRenderers::instance().add(renderer); }
    ~Registered() { FinalRenderers::instance().remove(renderer->id()); }
};

}  // namespace

USDCC_TEST(final_render_tiles) {
    std::vector<PixelRect> cores;
    const std::vector<PixelRect> regions = tileRegions(10, 7, 2, 2, 2, &cores);
    CHECK(regions.size() == 4 && cores.size() == 4, "a 2 × 2 grid: %zu", regions.size());
    // The cores cover every pixel once.
    std::vector<int> covered(70, 0);
    for (const PixelRect& core : cores) {
        for (int y = core.y; y < core.y + core.height; ++y)
            for (int x = core.x; x < core.x + core.width; ++x) ++covered[y * 10 + x];
    }
    CHECK(std::all_of(covered.begin(), covered.end(), [](int n) { return n == 1; }), "the tiles cover the image once");
    CHECK(regions[0] == (PixelRect{0, 0, 7, 5}), "the first grows right and down only: %d %d %d %d", regions[0].x, regions[0].y,
          regions[0].width, regions[0].height);
    CHECK(regions[3] == (PixelRect{3, 1, 7, 6}), "the last grows left and up, within the image: %d %d %d %d", regions[3].x,
          regions[3].y, regions[3].width, regions[3].height);
    for (size_t i = 0; i < regions.size(); ++i) CHECK(regions[i].contains(cores[i]), "tile %zu holds its core", i);
    CHECK(tileRegions(4, 4, 1, 1, 8, nullptr).front() == (PixelRect{0, 0, 4, 4}), "one tile is the image");
}

USDCC_TEST(final_render_spec) {
    Registered registered;
    UsdStageRefPtr stage = testStage();
    ResolvedSpec spec = resolveRenderSpec(stage, defaultRenderSettings(stage), registered.renderer.get());
    CHECK(spec.settings.IsEmpty() && spec.products.size() == 1 && spec.products[0].aovs.size() == 1 &&
              spec.products[0].aovs[0].name == "color",
          "no RenderSettings: one product with the color AOV");
    CHECK(spec.camera.IsEmpty(), "and no camera");

    const SdfPath path = createRenderSettings(stage);
    CHECK(path == SdfPath("/Render/Settings"), "made at /Render/Settings: %s", path.GetText());
    std::string named;
    stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &named);
    CHECK(named == path.GetString(), "and named in renderSettingsPrimPath: %s", named.c_str());
    CHECK(defaultRenderSettings(stage) == path, "it's the stage's default");
    CHECK(createRenderSettings(stage) == SdfPath("/Render/Settings2"), "another gets a free name");
    CHECK(defaultRenderSettings(stage) == path, "the metadata keeps the first as the default");

    const UsdPrim prim = stage->GetPrimAtPath(path);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(320, 200));
    prim.CreateAttribute(TfToken("gradient:boost"), SdfValueTypeNames->Double, true).Set(2.5);
    prim.CreateAttribute(TfToken("other:thing"), SdfValueTypeNames->Int, true).Set(4);
    setJobOption(prim, jobattr::renderer, VtValue(std::string("test.gradient")));
    setJobOption(prim, jobattr::tiles, VtValue(GfVec2i(3, 2)));
    setJobOption(prim, jobattr::frameStart, VtValue(2.0));
    setJobOption(prim, jobattr::format, VtValue(std::string("png")));

    spec = resolveRenderSpec(stage, path, registered.renderer.get());
    CHECK(spec.camera == SdfPath("/World/Cam"), "the stage's camera: %s", spec.camera.GetText());
    CHECK(spec.resolution == GfVec2i(320, 200), "its resolution");
    CHECK(spec.namespaced.count("gradient:boost") == 1 && spec.namespaced.count("other:thing") == 0,
          "the renderer's namespace only");
    CHECK(spec.options.renderer == "test.gradient" && spec.options.tilesX == 3 && spec.options.tilesY == 2 &&
              spec.options.frameStart == 2.0 && !spec.options.frameEnd && spec.options.format == "png",
          "usdcc's options");
    CHECK(resolvedRendererId(spec) == "test.gradient", "the renderer it names");
    const auto values = rendererValues(spec, registered.renderer->settingsSchema());
    CHECK(values.count(TfToken("boost")) && values.at(TfToken("boost")) == VtValue(2.5f),
          "the setting by its key, as the renderer's type (a float)");

    // A product with its own resolution wins.
    SdfPathVector products;
    UsdRenderSettings(prim).GetProductsRel().GetForwardedTargets(&products);
    UsdRenderProduct(stage->GetPrimAtPath(products.front())).CreateResolutionAttr().Set(GfVec2i(64, 32));
    UsdRenderProduct(stage->GetPrimAtPath(products.front())).CreateProductNameAttr().Set(TfToken("beauty.####.exr"));
    spec = resolveRenderSpec(stage, path, registered.renderer.get());
    CHECK(spec.resolution == GfVec2i(64, 32), "the product's resolution");
    CHECK(spec.products.front().productName == "beauty.####.exr", "and its productName");

    // A field set and cleared.
    const RenderSettingField field = registered.renderer->settingsSchema().front();
    CHECK(setFieldValue(prim, field, VtValue(4.0)), "set from a double");
    CHECK(fieldValue(prim, field) == VtValue(4.0), "kept as the double it was authored as");
    UsdPrim(prim).RemoveProperty(TfToken(field.attribute));
    CHECK(setFieldValue(prim, field, VtValue(5.0)) && fieldValue(prim, field) == VtValue(5.0f), "a new one as the field's float");
    CHECK(setFieldValue(prim, field, VtValue()) && fieldValue(prim, field).IsEmpty(), "cleared");
}

USDCC_TEST(final_render_output_names) {
    RenderJob job;
    job.stageName = "shot";
    job.settingsName = "Settings";
    job.rendererName = "Gradient";
    job.frames = {1, 2};
    job.spec.products = {{SdfPath("/Render/Products/Main"), "Main", std::string(), {{"color", "color", TfToken()}, {"depth", "depth", TfToken("float")}}}};
    QTemporaryDir dir;
    const QString root = dir.path();
    job.spec.options.output = (root + "/out/<stage>_<settings>").toStdString();
    const auto name = [&job](const std::string& aov, double frame) {
        return QFileInfo(QString::fromStdString(outputPath(job, job.spec.products.front(), aov, frame))).fileName();
    };
    CHECK(name("", 1) == "shot_Settings.0001.exr", "a frame number before the extension: %s", qPrintable(name("", 1)));
    job.frames = {1};
    CHECK(name("", 1) == "shot_Settings.exr", "none for one frame: %s", qPrintable(name("", 1)));
    job.spec.options.format = "png";
    CHECK(name("color", 1) == "shot_Settings.png", "PNG: the first AOV plain: %s", qPrintable(name("color", 1)));
    CHECK(name("depth", 1) == "shot_Settings_depth.png", "another with its name: %s", qPrintable(name("depth", 1)));
    job.spec.options.output = (root + "/<renderer>/####_<aov>.png").toStdString();
    CHECK(QString::fromStdString(outputPath(job, job.spec.products.front(), "depth", 12)).endsWith("/Gradient/0012_depth.png"),
          "tokens filled in");
    CHECK(frameText(12.5) == "0012.500", "a fractional frame: %s", frameText(12.5).c_str());
    job.spec.options.output.clear();
    job.spec.options.format = "exr";
    job.spec.products.front().productName = "beauty.####.exr";
    CHECK(name("", 7) == "beauty.0007.exr", "the product's own name: %s", qPrintable(name("", 7)));

    // A folder for each job: a relative pattern goes in it, an absolute one doesn't, and
    // <job> says where it goes.
    job.folderName = "shot_Settings_20261122-101500";
    const auto path = [&job](double frame) {
        return QDir::fromNativeSeparators(QString::fromStdString(outputPath(job, job.spec.products.front(), "", frame)));
    };
    CHECK(path(7).endsWith("/shot_Settings_20261122-101500/beauty.0007.exr"), "in the job's folder: %s", qPrintable(path(7)));
    job.spec.options.output = (root + "/abs/<stage>").toStdString();
    CHECK(path(7) == root + "/abs/shot.exr", "an absolute output as it is: %s", qPrintable(path(7)));
    job.spec.options.output = (root + "/abs/<job>/<stage>").toStdString();
    CHECK(path(7) == root + "/abs/shot_Settings_20261122-101500/shot.exr", "<job> in an absolute one: %s", qPrintable(path(7)));
    job.spec.options.output = "renders/<job>_<stage>";
    CHECK(path(7).endsWith("/renders/shot_Settings_20261122-101500_shot.exr") && !path(7).contains("101500/renders"),
          "<job> in a relative one, no folder added: %s", qPrintable(path(7)));
    const std::vector<std::string> files = frameFiles(job, 0);
    CHECK(files.size() == 1 && files.front() == outputPath(job, job.spec.products.front(), "", 7), "a frame's files");

    // Each name once, and not one that's taken.
    // The clock may reach the next second between the two: then again, with a stem of its own.
    std::string first;
    std::string second;
    const auto numbered = [&] { return second.size() > first.size() && second.rfind(first + "-", 0) == 0; };
    for (int attempt = 0; attempt < 5 && (attempt == 0 || !numbered()); ++attempt) {
        const std::string stem = "shot" + std::to_string(attempt);
        first = newJobFolderName(stem, "Settings");
        second = newJobFolderName(stem, "Settings");
    }
    CHECK(numbered(), "a second job in the same second gets -2: %s, %s", first.c_str(), second.c_str());
    const std::string free = newJobFolderName("other", "Settings", [](const std::string& name) {
        return name.find('-', name.size() - 3) == std::string::npos;  // only the plain name is taken
    });
    CHECK(free.size() > 2 && free.substr(free.size() - 2) == "-2", "a name that's taken: %s", free.c_str());
}

USDCC_TEST(final_render_scheduler_tiles) {
    Registered registered;
    QTemporaryDir dir;
    UsdStageRefPtr stage = testStage();
    const SdfPath path = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(path);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(40, 30));
    setJobOption(prim, jobattr::renderer, VtValue(std::string("test.gradient")));
    setJobOption(prim, jobattr::tiles, VtValue(GfVec2i(3, 2)));
    setJobOption(prim, jobattr::output, VtValue((dir.path() + "/frames/<stage>").toStdString()));
    prim.CreateAttribute(TfToken("gradient:boost"), SdfValueTypeNames->Float, true).Set(3.0f);

    JobRequest request;
    request.stage = stage;
    request.frames = JobRequest::Frames::Range;
    std::string error;
    RenderJobPtr job = makeRenderJob(request, &error);
    CHECK(job != nullptr, "a job: %s", error.c_str());
    if (!job) return;
    CHECK(job->frames == (std::vector<double>{1, 2, 3}), "the stage's frames");
    CHECK(job->tasks.size() == 18, "3 frames × 6 tiles: %zu", job->tasks.size());
    CHECK(job->rendererName == "Gradient" && job->stageName == "shot", "its renderer and stage: %s", job->stageName.c_str());
    // The snapshot is its own: an edit now doesn't reach the job.
    stage->GetPrimAtPath(SdfPath("/World/Ball")).SetActive(false);
    CHECK(job->tasks.front().task.stage->GetPrimAtPath(SdfPath("/World/Ball")).IsActive(), "the job keeps the stage as it was");

    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    std::vector<QImage> frames;
    QObject::connect(&scheduler, &RenderScheduler::frameRendered, [&frames](const QString&, int, const QImage& image) { frames.push_back(image); });
    scheduler.submit(job);
    CHECK(waitFor([&job]() { return ended(job); }, 10000), "it ends");
    CHECK(job->status == RenderJob::Status::Done, "done: %s", job->error.c_str());
    CHECK(registered.renderer->lastSettings.count(TfToken("boost")) && registered.renderer->lastSettings.at(TfToken("boost")) == VtValue(3.0f),
          "the renderer got the prim's setting");
    CHECK(registered.renderer->regions.size() == 6, "six different regions: %zu", registered.renderer->regions.size());
    CHECK(frames.size() == 3, "three frames put together: %zu", frames.size());

    // Every pixel where the gradient says, so each tile went to its place.
    bool exact = !frames.empty();
    for (size_t f = 0; f < frames.size() && exact; ++f) {
        const QImage& image = frames[f];
        if (image.width() != 40 || image.height() != 30) {
            exact = false;
            break;
        }
        for (int y = 0; y < 30 && exact; ++y) {
            const float* row = reinterpret_cast<const float*>(image.constScanLine(y));
            for (int x = 0; x < 40; ++x) {
                if (std::abs(row[x * 4] - x / 40.0f) > 1e-5f || std::abs(row[x * 4 + 1] - y / 30.0f) > 1e-5f) {
                    std::printf("    pixel %d,%d of frame %zu: %f %f\n", x, y, f, row[x * 4], row[x * 4 + 1]);
                    exact = false;
                    break;
                }
            }
        }
    }
    CHECK(exact, "the tiles stitched where they belong");
    if (usdcc::usd::linearExrAvailable()) {
        CHECK(job->files.size() == 3, "a file a frame: %zu", job->files.size());
        CHECK(QFileInfo::exists(dir.path() + "/frames/shot.0002.exr"), "named with its frame");
    }
}

USDCC_TEST(final_render_scheduler_retry_and_fail) {
    Registered registered;
    QTemporaryDir dir;
    UsdStageRefPtr stage = testStage();
    const SdfPath path = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(path);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(8, 8));
    setJobOption(prim, jobattr::renderer, VtValue(std::string("test.gradient")));
    setJobOption(prim, jobattr::format, VtValue(std::string("png")));
    setJobOption(prim, jobattr::output, VtValue((dir.path() + "/<stage>").toStdString()));
    JobRequest request;
    request.stage = stage;
    request.currentTime = 2;

    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    registered.renderer->failFirst = 2;
    RenderJobPtr job = makeRenderJob(request, nullptr);
    scheduler.submit(job);
    CHECK(waitFor([&job]() { return ended(job); }, 5000) && job->status == RenderJob::Status::Done,
          "two failures and then it renders");
    CHECK(job->tasks.front().attempts == 3, "on its third try: %d", job->tasks.front().attempts);
    CHECK(job->frames == std::vector<double>{2.0}, "the current frame");
    CHECK(QFileInfo::exists(dir.path() + "/shot.png"), "a PNG");

    registered.renderer->failAlways = true;
    RenderJobPtr failing = makeRenderJob(request, nullptr);
    scheduler.submit(failing);
    CHECK(waitFor([&failing]() { return ended(failing); }, 5000) && failing->status == RenderJob::Status::Failed,
          "fails after %d tries", RenderScheduler::kMaxAttempts);
    CHECK(failing->error == "always fails", "with the renderer's reason: %s", failing->error.c_str());

    registered.renderer->failAlways = false;
    scheduler.retry(failing->id);
    CHECK(waitFor([&failing]() { return ended(failing); }, 5000) && failing->status == RenderJob::Status::Done, "Retry renders it");

    registered.renderer->hang = true;
    RenderJobPtr hanging = makeRenderJob(request, nullptr);
    scheduler.submit(hanging);
    waitFor([&hanging]() { return hanging->status == RenderJob::Status::Running; }, 2000);
    scheduler.cancel(hanging->id);
    CHECK(hanging->status == RenderJob::Status::Cancelled && registered.renderer->cancelled, "Cancel stops the renderer");
    CHECK(!scheduler.busy(), "and nothing's left running");
    scheduler.clearEnded();
    CHECK(scheduler.jobs().empty(), "Clear Ended takes them off");
}

USDCC_TEST(final_render_job_errors) {
    UsdStageRefPtr stage = testStage();
    std::string error;
    JobRequest request;
    request.stage = stage;
    request.rendererOverride = "no.such.renderer";
    CHECK(!makeRenderJob(request, &error) && error.find("no.such.renderer") != std::string::npos, "an unknown renderer: %s",
          error.c_str());
    Registered registered;
    request.rendererOverride = "test.gradient";
    CHECK(!makeRenderJob(request, &error) && error.find("camera") != std::string::npos, "no camera named, no viewport: %s",
          error.c_str());
    usdcc::usd::RenderCamera camera;
    request.viewportCamera = camera;
    RenderJobPtr job = makeRenderJob(request, &error);
    CHECK(job && UsdGeomCamera(job->tasks.front().task.stage->GetPrimAtPath(job->spec.camera)), "the viewport's, as a camera in the snapshot");
    CHECK(!stage->GetPrimAtPath(SdfPath("/__usdccRender")), "not in the stage itself");
    registered.renderer->tiles = false;
    const SdfPath path = createRenderSettings(stage);
    setJobOption(stage->GetPrimAtPath(path), jobattr::tiles, VtValue(GfVec2i(4, 4)));
    request.settings = path;
    job = makeRenderJob(request, &error);
    CHECK(job && job->tasks.size() == 1, "a renderer without tiles renders the frame whole");
}

USDCC_TEST(final_render_embree) {
    registerHydraFinalRenderers();
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find("HdEmbreeRendererPlugin");
    CHECK(renderer != nullptr, "Embree is a final renderer");
    if (!renderer) return;
    CHECK(renderer->namespaces() == std::vector<std::string>{"embree"}, "in the embree namespace");
    CHECK(hydraNamespace(TfToken("HdArnoldRendererPlugin")) == "arnold", "Arnold's would be arnold");
    const std::vector<RenderSettingField> schema = renderer->settingsSchema();
    const auto bounces = std::find_if(schema.begin(), schema.end(), [](const RenderSettingField& f) { return f.rendererKey == TfToken("ty:maxBounces"); });
    CHECK(bounces != schema.end() && bounces->attribute == "embree:ty:maxBounces", "Max Bounces as embree:ty:maxBounces");

    QTemporaryDir dir;
    UsdStageRefPtr stage = testStage();
    const SdfPath path = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(path);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(64, 48));
    setJobOption(prim, jobattr::renderer, VtValue(std::string("HdEmbreeRendererPlugin")));
    setJobOption(prim, jobattr::output, VtValue((dir.path() + "/<stage>_<settings>").toStdString()));
    setJobOption(prim, jobattr::timeLimit, VtValue(30.0));
    const auto samples = std::find_if(schema.begin(), schema.end(), [](const RenderSettingField& f) {
        return f.rendererKey == TfToken("samplesToConvergence");
    });
    if (samples != schema.end()) setFieldValue(prim, *samples, VtValue(8));
    // A depth AOV, first, so it's the one the queue previews: where the sphere is.
    SdfPathVector products;
    UsdRenderSettings(prim).GetProductsRel().GetForwardedTargets(&products);
    UsdRenderVar depth = UsdRenderVar::Define(stage, products.front().AppendChild(TfToken("depth")));
    depth.CreateSourceNameAttr().Set(std::string("depth"));
    depth.CreateDataTypeAttr().Set(TfToken("float"));
    UsdRenderProduct(stage->GetPrimAtPath(products.front())).GetOrderedVarsRel().AddTarget(depth.GetPath(), UsdListPositionFrontOfPrependList);

    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    std::map<std::string, QImage> images;
    QString current;
    QObject::connect(&scheduler, &RenderScheduler::frameRendered,
                     [&images, &current](const QString&, int, const QImage& image) { images[current.toStdString()] = image; });

    JobRequest request;
    request.stage = stage;
    request.currentTime = 1;
    std::string error;
    current = QStringLiteral("whole");
    RenderJobPtr whole = makeRenderJob(request, &error);
    CHECK(whole != nullptr, "a job: %s", error.c_str());
    if (!whole) return;
    scheduler.submit(whole);
    CHECK(waitFor([&whole]() { return ended(whole); }, 60000) && whole->status == RenderJob::Status::Done, "Embree renders: %s",
          whole->error.c_str());

    setJobOption(prim, jobattr::tiles, VtValue(GfVec2i(2, 2)));
    setJobOption(prim, jobattr::output, VtValue((dir.path() + "/tiled_<stage>").toStdString()));
    current = QStringLiteral("tiled");
    RenderJobPtr tiled = makeRenderJob(request, &error);
    CHECK(tiled && tiled->tasks.size() == 4, "in four tiles");
    if (!tiled) return;
    scheduler.submit(tiled);
    CHECK(waitFor([&tiled]() { return ended(tiled); }, 60000) && tiled->status == RenderJob::Status::Done, "and tiled: %s",
          tiled->error.c_str());
    if (usdcc::usd::linearExrAvailable()) {
        CHECK(QFileInfo::exists(dir.path() + "/shot_Settings.exr"), "the EXR written");
    }

    const QImage a = images["whole"];
    const QImage b = images["tiled"];
    CHECK(a.size() == QSize(64, 48) && b.size() == QSize(64, 48), "both 64 × 48");
    if (a.size() != QSize(64, 48) || b.size() != QSize(64, 48)) return;
    // The sphere is above the middle: the pixels it covers (a depth other than the corner's
    // background, whatever the renderer clears depth to) are mostly in the top half.
    double top = 0, bottom = 0, difference = 0;
    const float background = reinterpret_cast<const float*>(a.constScanLine(0))[0];
    for (int y = 0; y < 48; ++y) {
        const float* ra = reinterpret_cast<const float*>(a.constScanLine(y));
        const float* rb = reinterpret_cast<const float*>(b.constScanLine(y));
        for (int x = 0; x < 64; ++x) {
            const bool coveredA = std::abs(ra[x * 4] - background) > 1e-4f;
            const bool coveredB = std::abs(rb[x * 4] - background) > 1e-4f;
            (y < 24 ? top : bottom) += coveredA ? 1 : 0;
            difference += coveredA != coveredB ? 1 : 0;
        }
    }
    CHECK(top > 50 && top > bottom * 2, "the sphere in the top half, the right way up: top %.0f, bottom %.0f", top, bottom);
    CHECK(difference / (64 * 48) < 0.03, "tiles give the frame a whole render gives: %.0f pixels differ", difference);
}

namespace {
// Renders the whole frame whatever the tile, from the scene file: R = x / width, G = y / height,
// B = the file's start time code. SceneFileRenderer cuts it to the tile.
class WholeFrameFileRenderer : public SceneFileRenderer {
public:
    std::string id() const override { return "test.file"; }
    std::string name() const override { return "File"; }
    Capabilities capabilities() const override { return {true, true}; }
    std::vector<std::string> namespaces() const override { return {"file"}; }
    std::vector<RenderSettingField> settingsSchema() const override { return {}; }
    std::vector<std::string> files;
    std::vector<std::string> customized;

protected:
    void customizeScene(const UsdStageRefPtr& stage, SceneExport& scene, const RenderTask&) override {
        stage->DefinePrim(SdfPath("/FileRendererWasHere"));
        customized.push_back(scene.product.GetString());
    }
    std::unique_ptr<RenderTaskHandle> renderFile(const RenderTask& task, const SceneExport& scene, TaskCallbacks callbacks) override {
        files.push_back(scene.file);
        const UsdStageRefPtr stage = UsdStage::Open(scene.file);
        const double time = stage ? stage->GetStartTimeCode() : -1.0;
        GfVec2i resolution(0);
        if (stage) UsdRenderProduct(stage->GetPrimAtPath(scene.product)).GetResolutionAttr().Get(&resolution);
        FloatImage whole(resolution[0], resolution[1]);
        for (int y = 0; y < whole.height; ++y)
            for (int x = 0; x < whole.width; ++x) {
                float* p = whole.row(y) + x * 4;
                p[0] = float(x) / whole.width;
                p[1] = float(y) / whole.height;
                p[2] = float(time);
                p[3] = 1.0f;
            }
        std::map<std::string, FloatImage> images;
        images[task.aovs.front().name] = whole;
        callbacks.finished(std::move(images));
        return nullptr;
    }
};
}  // namespace

USDCC_TEST(final_render_scene_export) {
    UsdStageRefPtr stage = testStage();
    const SdfPath settings = createRenderSettings(stage);
    UsdRenderSettings(stage->GetPrimAtPath(settings)).GetResolutionAttr().Set(GfVec2i(100, 50));
    stage->GetPrimAtPath(settings).CreateAttribute(TfToken("file:quality"), SdfValueTypeNames->Int, true).Set(3);
    setJobOption(stage->GetPrimAtPath(settings), jobattr::tiles, VtValue(GfVec2i(2, 1)));
    Registered registered;
    JobRequest request;
    request.stage = stage;
    request.currentTime = 2;
    request.rendererOverride = "test.gradient";
    RenderJobPtr job = makeRenderJob(request, nullptr);
    CHECK(job && job->tasks.size() == 2, "a job in two tiles");
    if (!job) return;

    QTemporaryDir dir;
    const RenderTask& right = job->tasks[1].task;
    const SceneExport scene = exportRenderScene(right, dir.path().toStdString(), [](const UsdStageRefPtr& s, SceneExport&) {
        s->DefinePrim(SdfPath("/Customized"));
    });
    CHECK(QFileInfo::exists(QString::fromStdString(scene.file)), "the scene written: %s", scene.file.c_str());
    const UsdStageRefPtr written = UsdStage::Open(scene.file);
    CHECK(written != nullptr, "and it opens");
    if (!written) return;
    std::string named;
    written->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &named);
    CHECK(scene.settings == settings && named == settings.GetString(), "the stage's own RenderSettings, named: %s", named.c_str());
    int quality = 0;
    written->GetPrimAtPath(settings).GetAttribute(TfToken("file:quality")).Get(&quality);
    CHECK(quality == 3, "with its renderer settings");
    const UsdRenderProduct product(written->GetPrimAtPath(scene.product));
    GfVec2i resolution(0);
    product.GetResolutionAttr().Get(&resolution);
    SdfPathVector cameras;
    product.GetCameraRel().GetForwardedTargets(&cameras);
    CHECK(resolution == GfVec2i(100, 50) && cameras == SdfPathVector{SdfPath("/World/Cam")}, "the product's size and camera");
    GfVec4f window(0.0f);
    product.GetDataWindowNDCAttr().Get(&window);
    CHECK(window == dataWindowNDC(right), "the tile as dataWindowNDC: %g %g %g %g", window[0], window[1], window[2], window[3]);
    CHECK(window[0] > 0.4f && window[0] < 0.5f && window[1] == 0.0f && window[2] == 1.0f && window[3] == 1.0f,
          "the right half, with its overlap");
    CHECK(written->GetStartTimeCode() == 2.0 && written->GetEndTimeCode() == 2.0, "the frame as the time range");
    CHECK(written->GetPrimAtPath(SdfPath("/Customized")).IsValid(), "and the renderer's own changes");
    CHECK(!right.stage->GetPrimAtPath(SdfPath("/Customized")).IsValid(), "the job's snapshot is left alone");

    // Without RenderSettings: made for the task, with a product and a color var.
    UsdStageRefPtr bare = testStage();
    JobRequest plain;
    plain.stage = bare;
    plain.rendererOverride = "test.gradient";
    plain.viewportCamera = usdcc::usd::RenderCamera();
    RenderJobPtr bareJob = makeRenderJob(plain, nullptr);
    CHECK(bareJob != nullptr, "a job without RenderSettings");
    if (!bareJob) return;
    const SceneExport made = exportRenderScene(bareJob->tasks.front().task, dir.path().toStdString());
    const UsdStageRefPtr madeStage = UsdStage::Open(made.file);
    SdfPathVector vars;
    UsdRenderProduct(madeStage->GetPrimAtPath(made.product)).GetOrderedVarsRel().GetForwardedTargets(&vars);
    CHECK(made.settings == SdfPath("/__usdccRender/Settings") && vars.size() == 1, "settings made, with a product and its var: %s",
          made.settings.GetText());
    CHECK(!UsdRenderProduct(madeStage->GetPrimAtPath(made.product)).GetDataWindowNDCAttr().HasAuthoredValue(),
          "no data window for a whole frame");
}

USDCC_TEST(final_render_scene_file_renderer) {
    auto renderer = std::make_shared<WholeFrameFileRenderer>();
    FinalRenderers::instance().add(renderer);
    UsdStageRefPtr stage = testStage();
    const SdfPath settings = createRenderSettings(stage);
    const UsdPrim prim = stage->GetPrimAtPath(settings);
    UsdRenderSettings(prim).GetResolutionAttr().Set(GfVec2i(30, 20));
    setJobOption(prim, jobattr::renderer, VtValue(std::string("test.file")));
    setJobOption(prim, jobattr::tiles, VtValue(GfVec2i(2, 2)));
    QTemporaryDir dir;
    setJobOption(prim, jobattr::output, VtValue((dir.path() + "/<stage>").toStdString()));
    JobRequest request;
    request.stage = stage;
    request.currentTime = 3;
    RenderJobPtr job = makeRenderJob(request, nullptr);
    CHECK(job && job->tasks.size() == 4, "four tiles");
    if (!job) {
        FinalRenderers::instance().remove("test.file");
        return;
    }
    RenderScheduler scheduler;
    scheduler.addWorker(std::make_shared<LocalWorker>());
    QImage frame;
    QObject::connect(&scheduler, &RenderScheduler::frameRendered, [&frame](const QString&, int, const QImage& image) { frame = image; });
    scheduler.submit(job);
    CHECK(waitFor([&job]() { return ended(job); }, 5000) && job->status == RenderJob::Status::Done, "done: %s", job->error.c_str());
    CHECK(renderer->files.size() == 4 && renderer->customized.size() == 4, "a scene per tile, each customized");
    bool exact = frame.size() == QSize(30, 20);
    for (int y = 0; y < 20 && exact; ++y) {
        const float* row = reinterpret_cast<const float*>(frame.constScanLine(y));
        for (int x = 0; x < 30 && exact; ++x) {
            exact = std::abs(row[x * 4] - x / 30.0f) < 1e-5f && std::abs(row[x * 4 + 1] - y / 20.0f) < 1e-5f && row[x * 4 + 2] == 3.0f;
        }
    }
    CHECK(exact, "whole frames cut to their tiles, stitched where they belong, at the frame's time");
    QCoreApplication::processEvents();
    CHECK(std::none_of(renderer->files.begin(), renderer->files.end(),
                       [](const std::string& f) { return QFileInfo::exists(QString::fromStdString(f)); }),
          "the scene files go with their tasks");
    FinalRenderers::instance().remove("test.file");
}
