#pragma once

// Every Hydra render delegate as a final renderer (docs/FINAL_RENDER.md §3.1). Each task gets
// a UsdImagingGLEngine of its own on the UI thread, driven by a timer until the delegate says
// it has converged (or the task's time limit), and its AOVs are read back as floats. Storm
// renders into an offscreen OpenGL context; every other delegate without the GPU
// (UsdImagingGLEngine's gpuEnabled = false), so it needs no OpenGL at all.

#include "usdcc/core/export.h"
#include "usdcc/usd/render/final_renderer.h"

#include <pxr/base/tf/token.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderBuffer;
PXR_NAMESPACE_CLOSE_SCOPE

namespace usdcc::render {

// Whether a delegate renders only the data window of a framing, as a tile needs.
USDCC_RENDER_API bool rendersRegions(const PXR_NS::TfToken& pluginId);

class USDCC_RENDER_API HydraFinalRenderer : public FinalRenderer {
public:
    explicit HydraFinalRenderer(PXR_NS::TfToken pluginId);

    std::string id() const override { return m_pluginId.GetString(); }
    std::string name() const override { return m_name; }
    // Storm's, when stormProblem() says one (usdcc_node's OpenGL check); none for the others.
    std::string setupProblem() const override;
    // Tiles, but not with MoonRay: it renders the whole view at a tile's size (docs/MOONRAY.md §7.8).
    Capabilities capabilities() const override { return {rendersRegions(m_pluginId), true}; }
    std::vector<std::string> namespaces() const override;
    // The delegate's settings (renderer_settings.h), defaulting to Settings > Renderers' values.
    std::vector<RenderSettingField> settingsSchema() const override;
    std::unique_ptr<RenderTaskHandle> start(const RenderTask& task, TaskCallbacks callbacks) override;

    const PXR_NS::TfToken& pluginId() const { return m_pluginId; }

private:
    PXR_NS::TfToken m_pluginId;
    std::string m_name;
};

// A Hydra delegate's settings namespace: its plugin id without Hd and RendererPlugin, with a
// lower-case first letter ("HdEmbreeRendererPlugin" → "embree", "HdArnoldRendererPlugin" → "arnold").
USDCC_RENDER_API std::string hydraNamespace(const PXR_NS::TfToken& pluginId);

// Adds a HydraFinalRenderer for each delegate there is (UsdImagingGLEngine::GetRendererPlugins()).
// Call once the USD plugins are set up (Arnold's included).
USDCC_RENDER_API void registerHydraFinalRenderers();

// The OpenGL context Storm's final renders get (docs/RENDER_NODE.md §5): an offscreen one in
// the default QSurfaceFormat, as HydraFinalRenderer makes it.
struct USDCC_RENDER_API OpenGlInfo {
    bool context = false;  // one could be made and made current
    int major = 0;
    int minor = 0;
    std::string vendor;    // GL_VENDOR
    std::string renderer;  // GL_RENDERER
    std::string version;   // GL_VERSION
    std::string why;       // no context: why not
};
// Makes that context (once; it's kept for Storm's renders) and reads what it is. Needs a
// QGuiApplication, on the UI thread; without one, no context.
USDCC_RENDER_API OpenGlInfo probeOpenGl();
// Why Storm can't render with such a context, or empty: none at all, a version below 4.5
// (4.1 on macOS, where Storm renders with Metal), or
// Microsoft's GDI renderer (Windows' software OpenGL 1.1, which a service in session 0 gets).
USDCC_RENDER_API std::string stormOpenGlProblem(const OpenGlInfo& info);
// What Storm's setupProblem() says from now on (empty: none), so a node neither offers nor
// takes Storm where it can't render. The app leaves it unset: there, Storm fails its task
// when it gets no context.
USDCC_RENDER_API void setStormProblem(std::string problem);
USDCC_RENDER_API std::string stormProblem();

// `region` of a Hydra render buffer (bottom row first, as Hydra's are), as a float RGBA image
// top row first. False for a format it can't read.
USDCC_RENDER_API bool readRenderBuffer(PXR_NS::HdRenderBuffer& buffer, const PixelRect& region, FloatImage* image);

}  // namespace usdcc::render
