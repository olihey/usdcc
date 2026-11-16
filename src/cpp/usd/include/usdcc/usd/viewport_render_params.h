#pragma once

// What a viewport's display options (docs/VIEWPORT_DISPLAY.md) mean to Hydra: one place
// that turns them into render params, so the frame, picking (TestIntersection()), the
// selection overlay and Fit to View all agree on what's drawn.

#include "usdcc/core/export.h"

#include "usdcc/usd/hydra_viewport_window.h"

#include <pxr/base/tf/token.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <string>

namespace usdcc::usd {

// params.complexity for each step: 1.0, 1.1, 1.2, 1.3 (usdview's).
USDCC_USD_UI_API float complexityValue(Complexity complexity);

// The shading the renderer `pluginId` draws: `display.shading` for Storm, Smooth Shaded
// for any other (they ignore the draw mode).
USDCC_USD_UI_API ShadingMode effectiveShading(const HydraViewportWindow::DisplayOptions& display,
                                              const PXR_NS::TfToken& pluginId);

// The draw mode a shading mode renders with. Hidden Line is DRAW_WIREFRAME, its second
// pass (the first draws depth only, DRAW_GEOM_ONLY).
USDCC_USD_UI_API PXR_NS::UsdImagingGLDrawMode drawModeFor(ShadingMode shading);

// The params a viewport renders `shading` with: the draw mode, complexity, purposes,
// culling, the stage's materials and lights, and the view transform (applyColorView()).
// The clear color is transparent: the viewport clears its own framebuffer to the
// background, a display color, and Hydra's composite lays the render over it. Not the
// frame time or the highlight, which the caller sets.
USDCC_USD_UI_API PXR_NS::UsdImagingGLRenderParams viewportRenderParams(const HydraViewportWindow::DisplayOptions& display,
                                                                       ShadingMode shading);

// The color correction Hydra applies (docs/COLOR_MANAGEMENT.md §4): OCIO with Settings'
// display and view (or `viewOverride`, a viewport's own) and the working space; Hydra's
// sRGB encode without a config; none for the Raw view. viewportRenderParams() calls it.
USDCC_USD_UI_API void applyColorView(PXR_NS::UsdImagingGLRenderParams& params, const std::string& viewOverride);

// The clear color for a renderer that fills its misses with the clear color, opaque,
// instead of leaving them transparent for the composite (Embree): the background taken
// back through `params`' view transform, so that the transform shows it as itself.
USDCC_USD_UI_API PXR_NS::GfVec4f opaqueClearColor(const QColor& background,
                                                 const PXR_NS::UsdImagingGLRenderParams& params);

// The purposes the viewport draws, for a UsdGeomBBoxCache: default, and each of guide,
// proxy and render that shows.
USDCC_USD_UI_API PXR_NS::TfTokenVector viewportPurposes(const HydraViewportWindow::DisplayOptions& display);

}  // namespace usdcc::usd
