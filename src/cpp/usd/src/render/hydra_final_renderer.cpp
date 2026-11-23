#include "usdcc/usd/render/hydra_final_renderer.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/renderer_settings.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/range2f.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdLux/boundableLightBase.h>
#include <pxr/usd/usdLux/nonboundableLightBase.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cctype>
#include <cstring>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::render {

namespace {

const TfToken kStorm("HdStormRendererPlugin");
// How often a delegate that renders in its own threads is asked how it's doing (ms).
constexpr int kPollMs = 20;

// The offscreen OpenGL context Storm renders in: one, made on first use, kept until the app
// goes (its surface must be made on the UI thread, as everything here is).
class OffscreenGl : public QObject {
public:
    static OffscreenGl* instance() {
        static QPointer<OffscreenGl> gl;
        if (!gl) gl = new OffscreenGl(QCoreApplication::instance());
        return gl;
    }

    bool makeCurrent() {
        // Without a QGuiApplication (usdcc_node --no-gpu) there's no OpenGL at all.
        if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return false;
        if (!m_context) {
            m_surface = new QOffscreenSurface(nullptr, this);
            m_surface->setFormat(QSurfaceFormat::defaultFormat());
            m_surface->create();
            m_context = new QOpenGLContext(this);
            m_context->setFormat(QSurfaceFormat::defaultFormat());
            if (!m_context->create()) {
                USDCC_LOG_WARNING("usdcc.render", "No OpenGL context for Storm's final renders");
            }
        }
        return m_context->isValid() && m_surface->isValid() && m_context->makeCurrent(m_surface);
    }

private:
    explicit OffscreenGl(QObject* parent) : QObject(parent) {}

    QOffscreenSurface* m_surface = nullptr;
    QOpenGLContext* m_context = nullptr;
};

bool stageHasLights(const UsdStageRefPtr& stage) {
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<UsdLuxBoundableLightBase>() || prim.IsA<UsdLuxNonboundableLightBase>()) return true;
    }
    return false;
}

float componentAt(const unsigned char* data, HdFormat component, size_t index) {
    switch (component) {
    case HdFormatUNorm8: return data[index] / 255.0f;
    case HdFormatSNorm8: return std::max(-1.0f, reinterpret_cast<const int8_t*>(data)[index] / 127.0f);
    case HdFormatFloat16: return static_cast<float>(reinterpret_cast<const GfHalf*>(data)[index]);
    case HdFormatFloat32: return reinterpret_cast<const float*>(data)[index];
    case HdFormatInt32: return static_cast<float>(reinterpret_cast<const int32_t*>(data)[index]);
    case HdFormatUInt16: return static_cast<float>(reinterpret_cast<const uint16_t*>(data)[index]);
    default: return 0.0f;
    }
}

// One task: an engine, a timer, and what it reports.
class HydraTask : public QObject, public RenderTaskHandle {
public:
    HydraTask(TfToken pluginId, RenderTask task, TaskCallbacks callbacks)
        : m_pluginId(std::move(pluginId)), m_task(std::move(task)), m_callbacks(std::move(callbacks)) {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this]() { step(); });
        m_timer.start(0);
    }

    ~HydraTask() override { release(); }

    void cancel() override {
        m_ended = true;
        m_timer.stop();
        release();
    }

private:
    bool storm() const { return m_pluginId == kStorm; }

    bool current() { return !storm() || OffscreenGl::instance()->makeCurrent(); }

    void release() {
        if (!m_engine) return;
        current();  // Storm frees its GL objects in its context
        m_engine.reset();
    }

    void fail(const std::string& error) {
        m_ended = true;
        release();
        if (m_callbacks.failed) m_callbacks.failed(error);
    }

    void setUp() {
        if (storm() && !OffscreenGl::instance()->makeCurrent()) {
            throw std::runtime_error("Storm needs OpenGL, and there's no OpenGL context here");
        }
        UsdImagingGLEngine::Parameters parameters;
        parameters.rendererPluginId = m_pluginId;
        parameters.gpuEnabled = storm();
        m_engine = std::make_unique<UsdImagingGLEngine>(parameters);
        if (m_engine->GetCurrentRendererId() != m_pluginId && !m_engine->SetRendererPlugin(m_pluginId)) {
            throw std::runtime_error("couldn't start the render delegate");
        }
        m_engine->SetEnablePresentation(false);

        // Settings > Renderers' values, then the RenderSettings prim's.
        for (const auto& [key, value] : usdcc::usd::rendererSettingDefaults(m_pluginId)) {
            m_engine->SetRendererSetting(key, value);
        }
        for (const auto& [key, value] : m_task.settings) m_engine->SetRendererSetting(key, value);
        if (!storm() && !m_task.renderSettings.IsEmpty()) {
            m_engine->SetActiveRenderSettingsPrimPath(m_task.renderSettings);
        }

        // The AOVs it has; one it hasn't is left out of the result.
        const TfTokenVector supported = m_engine->GetRendererAovs();
        TfTokenVector aovs;
        for (const RenderAov& aov : m_task.aovs) {
            const TfToken source(aov.sourceName);
            if (std::find(supported.begin(), supported.end(), source) == supported.end()) {
                USDCC_LOG_WARNING("usdcc.render", "{} has no AOV '{}': {} is left out", m_engine->GetRendererDisplayName(m_pluginId),
                                  aov.sourceName, aov.name);
                continue;
            }
            if (std::find(aovs.begin(), aovs.end(), source) == aovs.end()) aovs.push_back(source);
        }
        if (aovs.empty()) throw std::runtime_error("the renderer has none of the AOVs asked for");
        // Color first: what the engine treats as the main one.
        const auto color = std::find(aovs.begin(), aovs.end(), HdAovTokens->color);
        if (color != aovs.end()) std::rotate(aovs.begin(), color, color + 1);
        m_engine->SetRendererAovs(aovs);

        const int width = m_task.resolution[0];
        const int height = m_task.resolution[1];
        m_engine->SetRenderBufferSize(GfVec2i(width, height));
        const PixelRect& r = m_task.region;
        m_engine->SetFraming(CameraUtilFraming(GfRange2f(GfVec2f(0.0f, 0.0f), GfVec2f(float(width), float(height))),
                                               GfRect2i(GfVec2i(r.x, r.y), r.width, r.height), m_task.pixelAspect));
        m_engine->SetCameraPath(m_task.camera);

        // A stage without lights gets Hydra's dome, as a viewport has it.
        GlfSimpleLightVector lights;
        if (!stageHasLights(m_task.stage)) {
            GlfSimpleLight dome;
            dome.SetIsDomeLight(true);
            if (UsdGeomGetStageUpAxis(m_task.stage) == UsdGeomTokens->z) {
                dome.SetTransform(GfMatrix4d().SetRotate(GfRotation(GfVec3d::XAxis(), 90.0)));
            }
            lights.push_back(dome);
        }
        m_engine->SetLightingState(lights, GlfSimpleMaterial(), GfVec4f(0.0f, 0.0f, 0.0f, 1.0f));

        m_params.frame = UsdTimeCode(m_task.frame);
        m_params.enableLighting = true;
        m_params.enableSceneLights = true;
        m_params.enableSceneMaterials = true;
        m_params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;  // linear: the outputs are
        m_params.clearColor = GfVec4f(0.0f, 0.0f, 0.0f, 0.0f);
        m_params.highlight = false;
        m_params.showGuides = false;
        m_params.showProxy = false;
        m_params.showRender = true;
        if (!m_task.includedPurposes.empty()) {
            const auto has = [this](const TfToken& purpose) {
                return std::find(m_task.includedPurposes.begin(), m_task.includedPurposes.end(), purpose) !=
                       m_task.includedPurposes.end();
            };
            m_params.showGuides = has(UsdGeomTokens->guide);
            m_params.showProxy = has(UsdGeomTokens->proxy);
            m_params.showRender = has(UsdGeomTokens->render);
        }
        m_clock.start();
    }

    void step() {
        if (m_ended) return;
        try {
            if (!m_engine) {
                if (m_callbacks.progress) m_callbacks.progress(-1.0, "Starting " + UsdImagingGLEngine::GetRendererDisplayName(m_pluginId));
                setUp();
            }
            if (!current()) throw std::runtime_error("lost the OpenGL context");
            m_engine->Render(m_task.stage->GetPseudoRoot(), m_params);
            ++m_renders;
        } catch (const std::exception& error) {
            fail(std::string("Hydra failed: ") + error.what());
            return;
        }
        if (m_ended) return;

        const double seconds = m_clock.elapsed() / 1000.0;
        const bool outOfTime = m_task.timeLimit > 0 && seconds >= m_task.timeLimit;
        // Storm draws in one go, but a frame or two loads what it found in the first.
        const bool converged = m_engine->IsConverged() && m_renders >= (storm() ? 3 : 1);
        if (converged || outOfTime) {
            finish();
            return;
        }
        if (m_callbacks.progress) {
            double fraction = -1.0;
            const VtDictionary stats = m_engine->GetRenderStats();
            const auto percent = stats.find("percentDone");
            if (percent != stats.end()) {
                const VtValue asDouble = VtValue::Cast<double>(percent->second);
                if (asDouble.IsHolding<double>()) fraction = asDouble.UncheckedGet<double>() / 100.0;
            }
            if (fraction < 0 && m_task.timeLimit > 0) fraction = seconds / m_task.timeLimit;
            m_callbacks.progress(fraction, "Rendering");
        }
        m_timer.start(storm() ? 0 : kPollMs);
    }

    void finish() {
        std::map<std::string, FloatImage> images;
        std::string error;
        try {
            current();
            for (const RenderAov& aov : m_task.aovs) {
                HdRenderBuffer* buffer = m_engine->GetAovRenderBuffer(TfToken(aov.sourceName));
                if (!buffer) continue;
                buffer->Resolve();
                FloatImage image;
                if (readRenderBuffer(*buffer, m_task.region, &image)) {
                    images[aov.name] = std::move(image);
                } else {
                    USDCC_LOG_WARNING("usdcc.render", "Couldn't read the AOV '{}' back (format {})", aov.sourceName,
                                      static_cast<int>(buffer->GetFormat()));
                }
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        m_ended = true;
        release();
        if (images.empty()) {
            if (m_callbacks.failed) m_callbacks.failed(error.empty() ? "The renderer gave no image back" : error);
            return;
        }
        if (m_callbacks.finished) m_callbacks.finished(std::move(images));
    }

    TfToken m_pluginId;
    RenderTask m_task;
    TaskCallbacks m_callbacks;
    std::unique_ptr<UsdImagingGLEngine> m_engine;
    UsdImagingGLRenderParams m_params;
    QTimer m_timer;
    QElapsedTimer m_clock;
    int m_renders = 0;
    bool m_ended = false;
};

}  // namespace

std::string hydraNamespace(const TfToken& pluginId) {
    std::string name = pluginId.GetString();
    if (name.rfind("Hd", 0) == 0) name = name.substr(2);
    for (const char* suffix : {"RendererPlugin", "RenderPlugin", "Plugin"}) {
        const std::string s(suffix);
        if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
            name = name.substr(0, name.size() - s.size());
            break;
        }
    }
    if (name.empty()) return pluginId.GetString();
    name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
    return name;
}

HydraFinalRenderer::HydraFinalRenderer(TfToken pluginId)
    : m_pluginId(std::move(pluginId)), m_name(UsdImagingGLEngine::GetRendererDisplayName(m_pluginId)) {
    if (m_name.empty()) m_name = m_pluginId.GetString();
}

std::vector<std::string> HydraFinalRenderer::namespaces() const { return {hydraNamespace(m_pluginId)}; }

std::vector<RenderSettingField> HydraFinalRenderer::settingsSchema() const {
    std::vector<RenderSettingField> fields;
    const usdcc::usd::RendererSettingValues defaults = usdcc::usd::rendererSettingDefaults(m_pluginId);
    const std::string ns = hydraNamespace(m_pluginId);
    for (const usdcc::usd::RendererSettingInfo& info : usdcc::usd::rendererSettings(m_pluginId)) {
        RenderSettingField field;
        field.rendererKey = info.key;
        field.attribute = settingAttribute(ns, info.key.GetString());
        field.label = info.label;
        const auto preset = defaults.find(info.key);
        field.defaultValue = preset != defaults.end() ? preset->second : info.defaultValue;
        switch (info.type) {
        case usdcc::core::SettingType::Bool: field.type = RenderSettingField::Type::Bool; break;
        case usdcc::core::SettingType::Int: field.type = RenderSettingField::Type::Int; break;
        case usdcc::core::SettingType::Float: field.type = RenderSettingField::Type::Float; break;
        default: field.type = RenderSettingField::Type::String; break;
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

std::unique_ptr<RenderTaskHandle> HydraFinalRenderer::start(const RenderTask& task, TaskCallbacks callbacks) {
    if (!task.stage) {
        if (callbacks.failed) callbacks.failed("No stage to render");
        return nullptr;
    }
    return std::make_unique<HydraTask>(m_pluginId, task, std::move(callbacks));
}

namespace {
std::string& stormProblemText() {
    static std::string text;
    return text;
}

std::string glString(QOpenGLFunctions* gl, GLenum name) {
    const auto* text = reinterpret_cast<const char*>(gl->glGetString(name));
    return text ? std::string(text) : std::string();
}
}  // namespace

std::string HydraFinalRenderer::setupProblem() const {
    return m_pluginId == kStorm ? stormProblemText() : std::string();
}

void setStormProblem(std::string problem) { stormProblemText() = std::move(problem); }

std::string stormProblem() { return stormProblemText(); }

OpenGlInfo probeOpenGl() {
    OpenGlInfo info;
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        info.why = "started without the GPU (no QGuiApplication)";
        return info;
    }
    if (!OffscreenGl::instance()->makeCurrent()) {
        info.why = "no OpenGL context could be made";
        return info;
    }
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) {
        info.why = "no OpenGL context could be made current";
        return info;
    }
    info.context = true;
    info.major = context->format().majorVersion();
    info.minor = context->format().minorVersion();
    QOpenGLFunctions* gl = context->functions();
    info.vendor = glString(gl, GL_VENDOR);
    info.renderer = glString(gl, GL_RENDERER);
    info.version = glString(gl, GL_VERSION);
    return info;
}

std::string stormOpenGlProblem(const OpenGlInfo& info) {
#ifdef __APPLE__
    // Storm itself renders with Metal there (HgiMetal); 4.1, the newest macOS has, is
    // enough for the context it is given (docs/MACOS.md).
    constexpr int kNeededMinor = 1;
#else
    constexpr int kNeededMinor = 5;
#endif
    const std::string needs = "Storm needs OpenGL 4." + std::to_string(kNeededMinor);
    if (!info.context) return needs + ", and there's no OpenGL here: " + info.why;
    // Windows' own software OpenGL, all a service in session 0 gets.
    if (info.renderer.find("GDI Generic") != std::string::npos) {
        return needs + " on a GPU, and this session only has Microsoft's GDI renderer (OpenGL " +
               std::to_string(info.major) + "." + std::to_string(info.minor) +
               "); run it in a logged-on session (docs/RENDER_NODE.md §5)";
    }
    if (info.major < 4 || (info.major == 4 && info.minor < kNeededMinor)) {
        return needs + ", and this context is " + std::to_string(info.major) + "." +
               std::to_string(info.minor) + " (" + info.renderer + ")";
    }
    return {};
}

void registerHydraFinalRenderers() {
    for (const TfToken& plugin : UsdImagingGLEngine::GetRendererPlugins()) {
        FinalRenderers::instance().add(std::make_shared<HydraFinalRenderer>(plugin));
    }
}

bool readRenderBuffer(HdRenderBuffer& buffer, const PixelRect& region, FloatImage* image) {
    const HdFormat format = buffer.GetFormat();
    const size_t components = HdGetComponentCount(format);
    const HdFormat component = HdGetComponentFormat(format);
    if (format == HdFormatInvalid || components == 0 || components > 4) return false;
    if (component != HdFormatUNorm8 && component != HdFormatSNorm8 && component != HdFormatFloat16 &&
        component != HdFormatFloat32 && component != HdFormatInt32 && component != HdFormatUInt16) {
        return false;
    }
    const int width = static_cast<int>(buffer.GetWidth());
    const int height = static_cast<int>(buffer.GetHeight());
    const PixelRect whole{0, 0, width, height};
    if (region.empty() || !whole.contains(region)) return false;
    const auto* data = static_cast<const unsigned char*>(buffer.Map());
    if (!data) return false;
    const size_t componentBytes = HdDataSizeOfFormat(format) / components;
    *image = FloatImage(region.width, region.height);
    for (int y = 0; y < region.height; ++y) {
        // Hydra's rows run bottom to top.
        const int bufferRow = height - 1 - (region.y + y);
        float* out = image->row(y);
        for (int x = 0; x < region.width; ++x) {
            const size_t pixel = static_cast<size_t>(bufferRow) * static_cast<size_t>(width) + static_cast<size_t>(region.x + x);
            const unsigned char* at = data + pixel * components * componentBytes;
            float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (size_t c = 0; c < components; ++c) v[c] = componentAt(at, component, c);
            if (components == 1) v[1] = v[2] = v[0];
            out[x * 4 + 0] = v[0];
            out[x * 4 + 1] = v[1];
            out[x * 4 + 2] = v[2];
            out[x * 4 + 3] = components == 4 ? v[3] : 1.0f;
        }
    }
    buffer.Unmap();
    return true;
}

}  // namespace usdcc::render
