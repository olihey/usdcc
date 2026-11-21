#include "usdcc/usd/render/render_job.h"

#include "usdcc/usd/render/render_network.h"

#include "usdcc/usd/time_range.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::render {

namespace {

constexpr int kTileOverlap = 8;  // pixels: room for the pixel filter at a tile's edge

std::string newJobId() {
    static std::atomic<int> next{1};
    return "job-" + std::to_string(next++);
}

std::string baseName(const std::string& name) {
    const QFileInfo info(QString::fromStdString(name));
    return info.completeBaseName().toStdString();
}

// The private camera a job renders through when the prim names none: `camera` at the image's aspect.
SdfPath addCamera(const UsdStageRefPtr& stage, const usdcc::usd::RenderCamera& viewCamera, double aspect) {
    SdfPath root("/__usdccRender");
    for (int i = 1; stage->GetPrimAtPath(root); ++i) root = SdfPath("/__usdccRender_" + std::to_string(i));
    UsdGeomScope::Define(stage, root);
    UsdGeomCamera camera = UsdGeomCamera::Define(stage, root.AppendChild(TfToken("Camera")));
    const usdcc::usd::RenderCamera view = viewCamera.atAspect(aspect);
    camera.MakeMatrixXform().Set(view.cameraToWorld);
    const double focal = 50.0;
    float horizontal = 0.0f;
    float vertical = 0.0f;
    if (view.orthographic) {
        // An orthographic camera's aperture is in tenths of a scene unit.
        horizontal = static_cast<float>(view.orthoWidth / GfCamera::APERTURE_UNIT);
        vertical = static_cast<float>(horizontal / std::max(1e-6, aspect));
        camera.CreateProjectionAttr().Set(UsdGeomTokens->orthographic);
    } else {
        const double toRadians = 3.14159265358979323846 / 180.0;
        horizontal = static_cast<float>(2.0 * focal * std::tan(view.fovX * toRadians / 2.0));
        vertical = static_cast<float>(2.0 * focal * std::tan(view.fovY * toRadians / 2.0));
        camera.CreateProjectionAttr().Set(UsdGeomTokens->perspective);
    }
    camera.CreateFocalLengthAttr().Set(static_cast<float>(focal));
    camera.CreateHorizontalApertureAttr().Set(horizontal);
    camera.CreateVerticalApertureAttr().Set(vertical);
    camera.CreateClippingRangeAttr().Set(GfVec2f(static_cast<float>(view.nearClip), static_cast<float>(view.farClip)));
    return camera.GetPath();
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

bool hasFrameToken(const std::string& pattern) {
    return pattern.find("<frame>") != std::string::npos || pattern.find("####") != std::string::npos ||
           pattern.find("$F") != std::string::npos;
}

// `pattern` with ".<frame>" (or `_<what>`) put in before its extension.
std::string insertBeforeExtension(const std::string& pattern, const std::string& part) {
    const size_t slash = pattern.find_last_of("/\\");
    const size_t dot = pattern.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return pattern + part;
    return pattern.substr(0, dot) + part + pattern.substr(dot);
}

}  // namespace

int RenderJob::doneTasks() const {
    return static_cast<int>(std::count_if(tasks.begin(), tasks.end(), [](const JobTask& t) { return t.status == JobTask::Status::Done; }));
}

double RenderJob::progress() const {
    if (tasks.empty()) return 0.0;
    double sum = 0.0;
    for (const JobTask& task : tasks) {
        if (task.status == JobTask::Status::Done) {
            sum += 1.0;
        } else if (task.status == JobTask::Status::Running && task.progress > 0) {
            sum += std::min(task.progress, 1.0);
        }
    }
    return sum / static_cast<double>(tasks.size());
}

std::vector<PixelRect> tileRegions(int width, int height, int tilesX, int tilesY, int overlap, std::vector<PixelRect>* cores) {
    std::vector<PixelRect> regions;
    tilesX = std::clamp(tilesX, 1, std::max(1, width));
    tilesY = std::clamp(tilesY, 1, std::max(1, height));
    if (cores) cores->clear();
    for (int ty = 0; ty < tilesY; ++ty) {
        const int y0 = height * ty / tilesY;
        const int y1 = height * (ty + 1) / tilesY;
        for (int tx = 0; tx < tilesX; ++tx) {
            const int x0 = width * tx / tilesX;
            const int x1 = width * (tx + 1) / tilesX;
            const PixelRect core{x0, y0, x1 - x0, y1 - y0};
            const int gx0 = std::max(0, x0 - (tx > 0 ? overlap : 0));
            const int gy0 = std::max(0, y0 - (ty > 0 ? overlap : 0));
            const int gx1 = std::min(width, x1 + (tx + 1 < tilesX ? overlap : 0));
            const int gy1 = std::min(height, y1 + (ty + 1 < tilesY ? overlap : 0));
            regions.push_back({gx0, gy0, gx1 - gx0, gy1 - gy0});
            if (cores) cores->push_back(core);
        }
    }
    return regions;
}

std::string frameText(double frame) {
    char buffer[64];
    const double whole = std::floor(frame);
    if (std::abs(frame - std::round(frame)) < 1e-6) {
        std::snprintf(buffer, sizeof(buffer), "%04d", static_cast<int>(std::lround(frame)));
    } else {
        // 12.5 → "0012.500": still sorts with the whole frames.
        std::snprintf(buffer, sizeof(buffer), "%04d.%03d", static_cast<int>(whole),
                      static_cast<int>(std::lround((frame - whole) * 1000.0)));
    }
    return buffer;
}

std::string outputPath(const RenderJob& job, const ProductSpec& product, const std::string& aov, double frame) {
    const bool png = job.spec.options.format == "png";
    const std::string extension = png ? ".png" : ".exr";
    const bool manyFrames = job.frames.size() > 1;
    std::string pattern;
    if (!job.spec.options.output.empty()) {
        pattern = job.spec.options.output;
        if (job.spec.products.size() > 1 && pattern.find("<product>") == std::string::npos) {
            pattern = insertBeforeExtension(pattern, "_<product>");
        }
    } else if (!product.productName.empty()) {
        pattern = product.productName;  // UsdRender's own: the product's file
    } else {
        pattern = job.spec.products.size() > 1 ? "<stage>_<settings>_<product>" : "<stage>_<settings>";
    }
    // The extension the format says, unless the pattern has the format's own.
    const QString suffix = QFileInfo(QString::fromStdString(pattern)).suffix().toLower();
    if (suffix != (png ? QStringLiteral("png") : QStringLiteral("exr"))) {
        if (suffix == QStringLiteral("png") || suffix == QStringLiteral("exr")) {
            pattern = pattern.substr(0, pattern.size() - 4);
        }
        pattern += extension;
    }
    if (manyFrames && !hasFrameToken(pattern)) pattern = insertBeforeExtension(pattern, ".<frame>");
    // PNG has one image a file: each AOV but the first gets its own.
    if (png && !aov.empty() && pattern.find("<aov>") == std::string::npos && !product.aovs.empty() &&
        product.aovs.front().name != aov) {
        pattern = insertBeforeExtension(pattern, "_<aov>");
    }

    const std::string frameString = frameText(frame);
    std::string path = pattern;
    path = replaceAll(path, "<stage>", job.stageName);
    path = replaceAll(path, "<settings>", job.settingsName);
    path = replaceAll(path, "<product>", product.name);
    path = replaceAll(path, "<aov>", aov);
    path = replaceAll(path, "<renderer>", job.rendererName);
    path = replaceAll(path, "<frame>", frameString);
    path = replaceAll(path, "####", frameString);
    path = replaceAll(path, "$F4", frameString);
    path = replaceAll(path, "$F", frameString);
    QString result = QString::fromStdString(path);
    if (QDir::isRelativePath(result)) result = QDir(QString::fromStdString(defaultOutputFolder())).filePath(result);
    return QDir::cleanPath(result).toStdString();
}

RenderJobPtr makeRenderJob(const JobRequest& request, std::string* error) {
    const auto fail = [error](const std::string& why) -> RenderJobPtr {
        if (error) *error = why;
        return nullptr;
    };
    if (!request.stage) return fail("No stage to render.");
    const SdfPath settingsPath = request.settings.IsEmpty() ? defaultRenderSettings(request.stage) : request.settings;

    // The renderer first, as its namespaces pick the settings.
    ResolvedSpec spec = resolveRenderSpec(request.stage, settingsPath, nullptr);
    const std::string rendererId = request.rendererOverride.empty() ? resolvedRendererId(spec) : request.rendererOverride;
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(rendererId);
    if (!renderer) return fail(rendererId.empty() ? "There is no renderer." : "There is no renderer '" + rendererId + "'.");
    if (const std::string problem = renderer->setupProblem(); !problem.empty()) {
        return fail(renderer->name() + " can't render here: " + problem);
    }
    spec = resolveRenderSpec(request.stage, settingsPath, renderer.get());
    if (!renderer->capabilities().tiles) {
        spec.options.tilesX = 1;
        spec.options.tilesY = 1;
    }

    auto job = std::make_shared<RenderJob>();
    job->id = newJobId();
    job->rendererId = rendererId;
    job->rendererName = renderer->name();
    job->stageName = baseName(request.stage->GetRootLayer()->GetDisplayName());
    if (job->stageName.empty()) job->stageName = "untitled";
    job->settingsName = settingsPath.IsEmpty() ? "default" : settingsPath.GetName();

    // Frames.
    if (request.frames == JobRequest::Frames::Current) {
        job->frames = {request.currentTime};
    } else {
        const usdcc::usd::TimeRange range = usdcc::usd::stageTimeRange(request.stage);
        const double start = spec.options.frameStart.value_or(range.start);
        const double end = std::max(start, spec.options.frameEnd.value_or(range.end));
        const double step = spec.options.frameStep > 0 ? spec.options.frameStep : 1.0;
        for (int i = 0;; ++i) {
            const double frame = start + i * step;
            if (frame > end + 1e-9 || i > 100000) break;
            job->frames.push_back(frame);
        }
    }
    if (job->frames.empty()) return fail("No frames to render.");

    // The snapshot: everything the stage shows now, unsaved edits and the session layer
    // included, with asset paths resolved under the stage's resolver context.
    UsdStageRefPtr snapshot;
    {
        ArResolverContextBinder binder(request.stage->GetPathResolverContext());
        const SdfLayerRefPtr flat = request.stage->Flatten();
        if (!flat) return fail("Couldn't flatten the stage.");
        snapshot = UsdStage::Open(flat, request.stage->GetPathResolverContext());
    }
    if (!snapshot) return fail("Couldn't make the job's copy of the stage.");

    // The camera.
    SdfPath camera = spec.camera;
    if (!camera.IsEmpty() && !UsdGeomCamera(snapshot->GetPrimAtPath(camera))) {
        return fail("The render settings' camera " + camera.GetString() + " isn't a camera.");
    }
    const double aspect = spec.resolution[0] * static_cast<double>(spec.pixelAspect) / std::max(1, spec.resolution[1]);
    if (camera.IsEmpty()) {
        if (!request.viewportCamera) return fail("No camera: pick one in Render Settings, or click in a viewport of the stage.");
        camera = addCamera(snapshot, *request.viewportCamera, aspect);
    }
    spec.camera = camera;

    job->title = job->stageName + " · " + job->settingsName;
    if (request.dispatch) {
        request.dispatch->applyTo(spec.options);
    } else if (RenderNetwork* network = RenderNetwork::instance()) {
        network->dispatch().applyTo(spec.options);
    }
    // Peers that resolve the stage's paths themselves get it as it is (render_bundle.h).
    if (spec.options.usesPeers() && spec.options.delivery == "shared") job->shared = captureSharedScene(request.stage);
    job->spec = spec;

    // Tasks: each frame, whole or in tiles.
    std::vector<PixelRect> cores;
    const std::vector<PixelRect> regions =
        tileRegions(spec.resolution[0], spec.resolution[1], spec.options.tilesX, spec.options.tilesY, kTileOverlap, &cores);
    std::vector<RenderAov> aovs;
    for (const ProductSpec& product : spec.products) {
        for (const RenderAov& aov : product.aovs) {
            const bool known = std::any_of(aovs.begin(), aovs.end(), [&aov](const RenderAov& a) { return a.name == aov.name; });
            if (!known) aovs.push_back(aov);
        }
    }
    const std::map<TfToken, VtValue> settings = rendererValues(spec, renderer->settingsSchema());
    for (size_t f = 0; f < job->frames.size(); ++f) {
        for (size_t t = 0; t < regions.size(); ++t) {
            JobTask task;
            task.frameIndex = static_cast<int>(f);
            task.core = cores[t];
            RenderTask& r = task.task;
            r.jobId = job->id;
            r.index = static_cast<int>(job->tasks.size());
            r.frame = job->frames[f];
            r.stage = snapshot;
            r.renderSettings = spec.settings;
            r.camera = camera;
            r.resolution = spec.resolution;
            r.pixelAspect = spec.pixelAspect;
            r.aovs = aovs;
            r.settings = settings;
            r.includedPurposes = spec.includedPurposes;
            r.region = regions[t];
            r.timeLimit = spec.options.timeLimit;
            job->tasks.push_back(std::move(task));
        }
    }
    return job;
}

}  // namespace usdcc::render
