#include "rtx_final.h"

#include "rtx_panel.h"
#include "stage_bridge.h"
#include "usdcc/core/settings.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/var.h>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <algorithm>

namespace usdcc::rtx {

namespace {
using namespace PXR_NS;
using usdcc::render::FloatImage;
using usdcc::render::RenderSettingField;
using usdcc::render::RenderTask;
using usdcc::render::SceneExport;
using usdcc::render::TaskCallbacks;

constexpr int kPollMs = 30;

const VtValue* setting(const RenderTask& task, const char* key) {
    const auto found = task.settings.find(TfToken(key));
    return found == task.settings.end() ? nullptr : &found->second;
}

Mode taskMode(const RenderTask& task) {
    const VtValue* value = setting(task, kRtxModeAttribute);
    if (value && value->IsHolding<TfToken>()) return modeFromToken(value->UncheckedGet<TfToken>().GetString());
    if (value && value->IsHolding<std::string>()) return modeFromToken(value->UncheckedGet<std::string>());
    return Mode::PathTracing;
}

int taskBudget(const RenderTask& task, Mode mode) {
    const VtValue* value = setting(task, kRtxSamplesAttribute);
    if (value && value->IsHolding<int>()) return std::clamp(value->UncheckedGet<int>(), 1, 65536);
    return std::clamp(usdcc::core::Settings::instance().get<int>(
                          mode == Mode::PathTracing ? "RtxRender/sampleBudget" : "RtxRender/frameBudget", 128),
                      1, 65536);
}

LightMatch taskLight(const RenderTask& task) {
    auto& settings = usdcc::core::Settings::instance();
    LightMatch light{settings.get<bool>(kMatchStormLightsKey, true), settings.get<double>(kLightMultiplierKey, 1.0)};
    if (const VtValue* on = setting(task, kRtxMatchLightsAttribute); on && on->IsHolding<bool>()) light.on = on->UncheckedGet<bool>();
    if (const VtValue* multiplier = setting(task, kRtxLightMultiplierAttribute)) {
        const VtValue asDouble = VtValue::Cast<double>(*multiplier);
        if (asDouble.IsHolding<double>()) light.multiplier = asDouble.UncheckedGet<double>();
    }
    light.multiplier = std::clamp(light.multiplier, kMinLightMultiplier, kMaxLightMultiplier);
    return light;
}

double taskAspect(const RenderTask& task) {
    return task.resolution[0] * static_cast<double>(task.pixelAspect) / std::max(1, task.resolution[1]);
}

const char* modeToken(Mode mode) {
    return mode == Mode::PathTracing ? "PathTracing" : mode == Mode::Minimal ? "MinimalRendering" : "RealTimePathTracing";
}

// One task: an engine of its own, polled until its budget is reached.
class RtxFinalTask final : public QObject, public usdcc::render::RenderTaskHandle {
public:
    RtxFinalTask(std::unique_ptr<Engine> engine, std::shared_ptr<Snapshot> snapshot, int budget, double timeLimit,
                 std::string aov, TaskCallbacks callbacks)
        : m_engine(std::move(engine)), m_timeLimit(timeLimit), m_aov(std::move(aov)), m_callbacks(std::move(callbacks)) {
        m_engine->setVisible(true);
        m_engine->setActive(true);
        m_engine->replace(std::move(snapshot), budget);
        m_clock.start();
        connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
        m_timer.start(kPollMs);
    }
    ~RtxFinalTask() override { stop(); }
    void cancel() override {
        m_ended = true;
        stop();
    }

private:
    void stop() {
        m_timer.stop();
        if (m_engine) {
            m_engine->close();  // the service drains what it has started
            m_engine.reset();
        }
    }
    void end(const std::function<void()>& report) {
        m_ended = true;
        stop();
        report();
    }
    void poll() {
        if (m_ended || !m_engine) return;
        const EngineUpdate update = m_engine->poll();
        if (!update.error.isEmpty()) {
            const std::string error = update.error.toStdString();
            end([this, error] { if (m_callbacks.failed) m_callbacks.failed("RTX failed: " + error); });
            return;
        }
        if (update.frame) m_frame = update.frame;
        const bool outOfTime = m_timeLimit > 0 && m_clock.elapsed() / 1000.0 >= m_timeLimit;
        if (m_frame && (update.done || outOfTime)) {
            finish();
            return;
        }
        if (m_callbacks.progress) {
            const double fraction = m_timeLimit > 0 ? m_clock.elapsed() / 1000.0 / m_timeLimit : -1.0;
            m_callbacks.progress(fraction, update.status.isEmpty() ? "Rendering" : update.status.toStdString());
        }
    }
    void finish() {
        std::vector<float> rgba;
        const QSize size = m_frame->size();
        if (!m_frame->readLinear(rgba) || rgba.size() != size_t(size.width()) * size_t(size.height()) * 4) {
            end([this] { if (m_callbacks.failed) m_callbacks.failed("RTX's frame couldn't be read back"); });
            return;
        }
        FloatImage image(size.width(), size.height());
        image.rgba = std::move(rgba);
        std::map<std::string, FloatImage> images;
        images[m_aov] = std::move(image);
        end([this, &images] { if (m_callbacks.finished) m_callbacks.finished(std::move(images)); });
    }

    std::unique_ptr<Engine> m_engine;
    std::shared_ptr<GpuFrame> m_frame;
    double m_timeLimit;
    std::string m_aov;
    TaskCallbacks m_callbacks;
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_ended = false;
};

class RtxFinalRenderer final : public usdcc::render::SceneFileRenderer {
public:
    RtxFinalRenderer(FinalEngineFactory engines, RuntimeCheck problem)
        : m_engines(std::move(engines)), m_problem(std::move(problem)) {}

    std::string id() const override { return kRtxFinalRendererId; }
    std::string name() const override { return "RTX"; }
    // Whole frames only: whether ovrtx honours dataWindowNDC hasn't been checked on a GPU.
    Capabilities capabilities() const override { return {false, false}; }
    // UsdRenderComputeSpec matches a namespace by its first part only: "omni" takes omni:rtx:*.
    std::vector<std::string> namespaces() const override { return {"omni"}; }
    std::string setupProblem() const override {
        const std::optional<QString> problem = m_problem ? m_problem() : std::nullopt;
        return problem ? problem->toStdString() : std::string();
    }

    std::vector<RenderSettingField> settingsSchema() const override {
        auto& settings = usdcc::core::Settings::instance();
        std::vector<RenderSettingField> fields;
        RenderSettingField mode;
        mode.attribute = kRtxModeAttribute;
        mode.rendererKey = TfToken(kRtxModeAttribute);
        mode.label = "Mode";
        mode.help = "Path Tracing refines toward the sample budget; the others render that many frames";
        mode.type = RenderSettingField::Type::Choice;
        mode.choices = {"PathTracing", "RealTimePathTracing", "MinimalRendering"};
        mode.defaultValue = VtValue(TfToken("PathTracing"));
        fields.push_back(mode);
        RenderSettingField samples;
        samples.attribute = kRtxSamplesAttribute;
        samples.rendererKey = TfToken(kRtxSamplesAttribute);
        samples.label = "Samples / frames";
        samples.help = "Path Tracing's samples per pixel, or the other modes' frames";
        samples.type = RenderSettingField::Type::Int;
        samples.defaultValue = VtValue(settings.get<int>("RtxRender/sampleBudget", 128));
        samples.hasRange = true;
        samples.minimum = 1;
        samples.maximum = 65536;
        fields.push_back(samples);
        RenderSettingField match;
        match.attribute = kRtxMatchLightsAttribute;
        match.rendererKey = TfToken(kRtxMatchLightsAttribute);
        match.label = "Match Storm light intensities";
        match.group = "Lighting";
        match.type = RenderSettingField::Type::Bool;
        match.defaultValue = VtValue(settings.get<bool>(kMatchStormLightsKey, true));
        fields.push_back(match);
        RenderSettingField multiplier;
        multiplier.attribute = kRtxLightMultiplierAttribute;
        multiplier.rendererKey = TfToken(kRtxLightMultiplierAttribute);
        multiplier.label = "Intensity multiplier";
        multiplier.group = "Lighting";
        multiplier.type = RenderSettingField::Type::Float;
        multiplier.defaultValue = VtValue(static_cast<float>(settings.get<double>(kLightMultiplierKey, 1.0)));
        multiplier.hasRange = true;
        multiplier.minimum = kMinLightMultiplier;
        multiplier.maximum = kMaxLightMultiplier;
        fields.push_back(multiplier);
        return fields;
    }

protected:
    void customizeScene(const UsdStageRefPtr& stage, SceneExport& scene, const RenderTask& task) override {
        const Mode mode = taskMode(task);
        const LightMatch light = taskLight(task);
        // Light matching lifts everything by 1/k, and ovrtx's emission has that already.
        if (light.on) scaleEmission(stage->GetRootLayer(), kRtxDefaultExposure);
        SdfPath root("/__usdccRtxFinal");
        for (int i = 1; stage->GetPrimAtPath(root); ++i) root = SdfPath("/__usdccRtxFinal_" + std::to_string(i));
        UsdGeomScope::Define(stage, root);
        const usdcc::usd::RenderCamera camera = finalCamera(stage, task.camera, task.frame, taskAspect(task));
        UsdGeomCamera privateCamera = definePrivateCamera(stage, root.AppendChild(TfToken("Camera")), camera, light);
        scene.camera = privateCamera.GetPath();

        UsdRenderProduct product(stage->GetPrimAtPath(scene.product));
        product.GetCameraRel().SetTargets({privateCamera.GetPath()});
        product.GetPrim().CreateAttribute(TfToken(kRtxModeAttribute), SdfValueTypeNames->Token, false).Set(TfToken(modeToken(mode)));
        if (mode == Mode::PathTracing) {
            product.GetPrim().CreateAttribute(TfToken("omni:rtx:pt:samplesPerPixel"), SdfValueTypeNames->UInt, false).Set(1u);
        }
        // HdrColor only: linear, what the app's outputs take.
        UsdRenderVar color = UsdRenderVar::Define(stage, product.GetPath().AppendChild(TfToken("HdrColor")));
        color.CreateSourceNameAttr().Set(std::string("HdrColor"));
        product.GetOrderedVarsRel().SetTargets({color.GetPath()});
    }

    std::unique_ptr<usdcc::render::RenderTaskHandle> renderFile(const RenderTask& task, const SceneExport& scene,
                                                                TaskCallbacks callbacks) override {
        if (const std::string problem = setupProblem(); !problem.empty()) {
            if (callbacks.failed) callbacks.failed(problem);
            return nullptr;
        }
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->file = QString::fromStdString(scene.file);
        snapshot->product = QString::fromStdString(scene.product.GetString());
        snapshot->camera = QString::fromStdString(scene.camera.GetString());
        snapshot->view = cameraValues(finalCamera(task.stage, task.camera, task.frame, taskAspect(task)));
        snapshot->size = QSize(task.resolution[0], task.resolution[1]);
        snapshot->mode = taskMode(task);
        snapshot->light = taskLight(task);
        snapshot->colorVar = QStringLiteral("HdrColor");
        std::unique_ptr<Engine> engine;
        try {
            engine = m_engines ? m_engines() : nullptr;
        } catch (const std::exception& error) {
            if (callbacks.failed) callbacks.failed(std::string("RTX couldn't start: ") + error.what());
            return nullptr;
        }
        if (!engine) {
            if (callbacks.failed) callbacks.failed("RTX couldn't start");
            return nullptr;
        }
        const std::string aov = task.aovs.empty() ? std::string("color") : task.aovs.front().name;
        const int budget = taskBudget(task, snapshot->mode);
        return std::make_unique<RtxFinalTask>(std::move(engine), std::move(snapshot), budget, task.timeLimit, aov,
                                              std::move(callbacks));
    }

private:
    FinalEngineFactory m_engines;
    RuntimeCheck m_problem;
};

}  // namespace

Mode modeFromToken(const std::string& token) {
    if (token == "PathTracing") return Mode::PathTracing;
    if (token == "MinimalRendering") return Mode::Minimal;
    return Mode::RealTime;
}

usdcc::usd::RenderCamera finalCamera(const UsdStageRefPtr& stage, const SdfPath& path, double frame, double aspect) {
    const UsdGeomCamera prim(stage ? stage->GetPrimAtPath(path) : UsdPrim());
    if (!prim) throw std::runtime_error("no camera at " + path.GetString());
    const GfCamera camera = prim.GetCamera(UsdTimeCode(frame));
    usdcc::usd::RenderCamera result;
    result.cameraToWorld = camera.GetTransform();
    result.fovX = camera.GetFieldOfView(GfCamera::FOVHorizontal);
    result.fovY = camera.GetFieldOfView(GfCamera::FOVVertical);
    result.aspect = camera.GetAspectRatio() > 0 ? camera.GetAspectRatio() : 1.0;
    result.nearClip = camera.GetClippingRange().GetMin();
    result.farClip = camera.GetClippingRange().GetMax();
    result.orthographic = camera.GetProjection() == GfCamera::Orthographic;
    result.orthoWidth = camera.GetHorizontalAperture() * GfCamera::APERTURE_UNIT;
    result.prim = path;
    return result.atAspect(aspect);
}

std::shared_ptr<usdcc::render::FinalRenderer> createRtxFinalRenderer(FinalEngineFactory engines, RuntimeCheck problem) {
    return std::make_shared<RtxFinalRenderer>(std::move(engines), std::move(problem));
}

}  // namespace usdcc::rtx
