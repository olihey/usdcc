#pragma once

// What a viewport's display options (docs/VIEWPORT_DISPLAY.md) mean to Hydra: one place
// that turns them into render params, so the frame, picking (TestIntersection()), the
// selection overlay and Fit to View all agree on what's drawn.

#include "usdcc/core/export.h"

#include "usdcc/usd/hydra_viewport_window.h"

#include <pxr/base/tf/token.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

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
// culling, the stage's materials and lights, and the background as the clear color.
// Not the frame time or the highlight, which the caller sets.
USDCC_USD_UI_API PXR_NS::UsdImagingGLRenderParams viewportRenderParams(const HydraViewportWindow::DisplayOptions& display,
                                                                       ShadingMode shading);

// The purposes the viewport draws, for a UsdGeomBBoxCache: default, and each of guide,
// proxy and render that shows.
USDCC_USD_UI_API PXR_NS::TfTokenVector viewportPurposes(const HydraViewportWindow::DisplayOptions& display);

}  // namespace usdcc::usd
