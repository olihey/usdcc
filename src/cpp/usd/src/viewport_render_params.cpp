#include "usdcc/usd/viewport_render_params.h"

#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/tokens.h>

namespace usdcc::usd {

float complexityValue(Complexity complexity) {
    switch (complexity) {
        case Complexity::Medium: return 1.1f;
        case Complexity::High: return 1.2f;
        case Complexity::VeryHigh: return 1.3f;
        case Complexity::Low: break;
    }
    return 1.0f;
}

bool HydraViewportWindow::rendererDrawsShadingModes(const PXR_NS::TfToken& pluginId) {
    // Before the engine exists the renderer is empty, and Storm is the one it starts with.
    return pluginId.IsEmpty() || pluginId == "HdStormRendererPlugin";
}

ShadingMode effectiveShading(const HydraViewportWindow::DisplayOptions& display, const PXR_NS::TfToken& pluginId) {
    return HydraViewportWindow::rendererDrawsShadingModes(pluginId) ? display.shading : ShadingMode::SmoothShaded;
}

PXR_NS::UsdImagingGLDrawMode drawModeFor(ShadingMode shading) {
    switch (shading) {
        case ShadingMode::FlatShaded: return PXR_NS::UsdImagingGLDrawMode::DRAW_SHADED_FLAT;
        case ShadingMode::WireframeOnShaded: return PXR_NS::UsdImagingGLDrawMode::DRAW_WIREFRAME_ON_SURFACE;
        case ShadingMode::Wireframe:
        case ShadingMode::HiddenLine: return PXR_NS::UsdImagingGLDrawMode::DRAW_WIREFRAME;
        case ShadingMode::SmoothShaded: break;
    }
    return PXR_NS::UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
}

PXR_NS::UsdImagingGLRenderParams viewportRenderParams(const HydraViewportWindow::DisplayOptions& display,
                                                      ShadingMode shading) {
    PXR_NS::UsdImagingGLRenderParams params;
    params.frame = PXR_NS::UsdTimeCode::Default();
    params.drawMode = drawModeFor(shading);
    params.complexity = complexityValue(display.complexity);
    params.showGuides = display.showGuides;
    params.showProxy = display.showProxy;
    params.showRender = display.showRender;
    params.cullStyle = display.cullBackfaces ? PXR_NS::UsdImagingGLCullStyle::CULL_STYLE_BACK_UNLESS_DOUBLE_SIDED
                                             : PXR_NS::UsdImagingGLCullStyle::CULL_STYLE_NOTHING;
    params.enableLighting = true;
    params.enableSceneMaterials = display.sceneMaterials;
    params.enableSceneLights = display.sceneLights;
    params.clearColor = PXR_NS::GfVec4f(static_cast<float>(display.background.redF()),
                                        static_cast<float>(display.background.greenF()),
                                        static_cast<float>(display.background.blueF()), 1.0f);
    return params;
}

PXR_NS::TfTokenVector viewportPurposes(const HydraViewportWindow::DisplayOptions& display) {
    PXR_NS::TfTokenVector purposes{PXR_NS::UsdGeomTokens->default_};
    if (display.showGuides) purposes.push_back(PXR_NS::UsdGeomTokens->guide);
    if (display.showProxy) purposes.push_back(PXR_NS::UsdGeomTokens->proxy);
    if (display.showRender) purposes.push_back(PXR_NS::UsdGeomTokens->render);
    return purposes;
}

}  // namespace usdcc::usd
