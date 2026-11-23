#include "usdcc/usd/render_camera.h"

#include <pxr/base/gf/camera.h>
#include <pxr/usd/usdGeom/camera.h>

#include <cmath>
#include <numbers>

namespace usdcc::usd {

namespace {
constexpr double kDegrees = 180.0 / std::numbers::pi;
}  // namespace

double scaledFov(double degrees, double ratio) {
    // tan(half angle) scales with the side.
    return 2.0 * std::atan(std::tan(degrees / kDegrees / 2.0) * ratio) * kDegrees;
}

RenderCamera RenderCamera::atAspect(double newAspect) const {
    RenderCamera camera = *this;
    if (newAspect <= 0.0) return camera;
    camera.aspect = newAspect;
    if (prim.IsEmpty()) {
        camera.fovX = scaledFov(fovY, newAspect);
    } else {
        camera.fovY = scaledFov(fovX, 1.0 / newAspect);
    }
    return camera;
}

std::optional<RenderCamera> primRenderCamera(const PXR_NS::UsdPrim& prim, double aspect, PXR_NS::UsdTimeCode time) {
    const PXR_NS::UsdGeomCamera usdCamera(prim);
    if (!usdCamera) return std::nullopt;
    const PXR_NS::GfCamera gfCamera = usdCamera.GetCamera(time);
    RenderCamera camera;
    camera.prim = prim.GetPath();
    camera.cameraToWorld = gfCamera.GetTransform();
    camera.fovX = gfCamera.GetFieldOfView(PXR_NS::GfCamera::FOVHorizontal);
    camera.nearClip = gfCamera.GetClippingRange().GetMin();
    camera.farClip = gfCamera.GetClippingRange().GetMax();
    if (gfCamera.GetProjection() == PXR_NS::GfCamera::Orthographic) {
        camera.orthographic = true;
        camera.orthoWidth = gfCamera.GetHorizontalAperture() * PXR_NS::GfCamera::APERTURE_UNIT;
    }
    // As its aperture frames it, then at the image's aspect ratio.
    const double apertureAspect = gfCamera.GetAspectRatio();
    camera.aspect = apertureAspect > 0.0 ? apertureAspect : 1.0;
    camera.fovY = scaledFov(camera.fovX, 1.0 / camera.aspect);
    return camera.atAspect(aspect);
}

}  // namespace usdcc::usd
