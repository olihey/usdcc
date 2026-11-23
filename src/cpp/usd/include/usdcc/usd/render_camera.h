#pragma once

// A camera as a renderer is given it (docs/RENDER_VIEW_PANEL.md §3), without widgets: the
// final renders' jobs (usdcc_render) and the Render View panel (usdcc_usd_ui) share it.

#include "usdcc/core/export.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/timeCode.h>

#include <optional>

namespace usdcc::usd {

// Where it is, and how much it sees at the image's aspect ratio.
struct USDCC_RENDER_API RenderCamera {
    PXR_NS::GfMatrix4d cameraToWorld{1.0};
    // Fields of view, in degrees, at `aspect`.
    double fovX = 45.0;
    double fovY = 45.0;
    // Width / height of the image.
    double aspect = 1.0;
    double nearClip = 0.1;
    double farClip = 10000.0;
    bool orthographic = false;
    // What an orthographic camera sees across, in scene units.
    double orthoWidth = 0.0;
    // The camera prim; empty for a viewport's camera.
    PXR_NS::SdfPath prim;

    // The same camera at another aspect ratio: a viewport's or the default camera
    // keeps its vertical field of view (a wider image shows more at the sides), a
    // camera prim its horizontal one, as its aperture frames it.
    RenderCamera atAspect(double newAspect) const;
};

// A field of view across one side from the one across the other, for an image `ratio`
// times as long on the side wanted (degrees).
USDCC_RENDER_API double scaledFov(double degrees, double ratio);

// The camera of a UsdGeomCamera prim at `aspect`, at `time` (an animated camera where it
// is then); nothing for a prim that isn't one.
USDCC_RENDER_API std::optional<RenderCamera> primRenderCamera(const PXR_NS::UsdPrim& prim, double aspect,
                                                              PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default());

}  // namespace usdcc::usd
