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

class USDCC_USD_UI_API HydraFinalRenderer : public FinalRenderer {
public:
    explicit HydraFinalRenderer(PXR_NS::TfToken pluginId);

    std::string id() const override { return m_pluginId.GetString(); }
    std::string name() const override { return m_name; }
    Capabilities capabilities() const override { return {true, true}; }
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
USDCC_USD_UI_API std::string hydraNamespace(const PXR_NS::TfToken& pluginId);

// Adds a HydraFinalRenderer for each delegate there is (UsdImagingGLEngine::GetRendererPlugins()).
// Call once the USD plugins are set up (Arnold's included).
USDCC_USD_UI_API void registerHydraFinalRenderers();

// `region` of a Hydra render buffer (bottom row first, as Hydra's are), as a float RGBA image
// top row first. False for a format it can't read.
USDCC_USD_UI_API bool readRenderBuffer(PXR_NS::HdRenderBuffer& buffer, const PixelRect& region, FloatImage* image);

}  // namespace usdcc::render
