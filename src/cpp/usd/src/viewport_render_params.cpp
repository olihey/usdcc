#include "usdcc/usd/viewport_render_params.h"

#include "usdcc/usd/color_management.h"

#include <pxr/base/gf/vec4f.h>

#include <array>
#include <cmath>
#include <pxr/imaging/hdx/tokens.h>
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
    // Clear to nothing: Hydra's composite lays the render over the viewport's own
    // background (its glClear), which is a display color and mustn't go through the view.
    params.clearColor = PXR_NS::GfVec4f(0.0f, 0.0f, 0.0f, 0.0f);
    applyColorView(params, display.colorView);
    return params;
}

PXR_NS::GfVec4f opaqueClearColor(const QColor& background, const PXR_NS::UsdImagingGLRenderParams& params) {
    std::array<float, 3> rgb{static_cast<float>(background.redF()), static_cast<float>(background.greenF()),
                             static_cast<float>(background.blueF())};
    const PXR_NS::TfToken& mode = params.colorCorrectionMode;
    if (mode == PXR_NS::HdxColorCorrectionTokens->openColorIO) {
        rgb = ColorManagement::instance().fromDisplay(rgb, {params.ocioDisplay.GetString(), params.ocioView.GetString()});
    } else if (mode == PXR_NS::HdxColorCorrectionTokens->sRGB) {
        for (float& c : rgb) c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return PXR_NS::GfVec4f(rgb[0], rgb[1], rgb[2], 1.0f);
}

void applyColorView(PXR_NS::UsdImagingGLRenderParams& params, const std::string& viewOverride) {
    const ColorManagement& color = ColorManagement::instance();
    const DisplayView target = color.displayView(viewOverride);
    if (viewOverride == kRawView || (color.available() && target.view == kRawView)) {
        params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->disabled;
    } else if (color.available()) {
        params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->openColorIO;
        params.ocioDisplay = PXR_NS::TfToken(target.display);
        params.ocioView = PXR_NS::TfToken(target.view);
        params.ocioColorSpace = PXR_NS::TfToken(color.workingSpace());
    } else {
        // No config: Hydra's own sRGB encode, what an un-tone-mapped sRGB view does.
        params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->sRGB;
    }
}

PXR_NS::TfTokenVector viewportPurposes(const HydraViewportWindow::DisplayOptions& display) {
    PXR_NS::TfTokenVector purposes{PXR_NS::UsdGeomTokens->default_};
    if (display.showGuides) purposes.push_back(PXR_NS::UsdGeomTokens->guide);
    if (display.showProxy) purposes.push_back(PXR_NS::UsdGeomTokens->proxy);
    if (display.showRender) purposes.push_back(PXR_NS::UsdGeomTokens->render);
    return purposes;
}

}  // namespace usdcc::usd
