#pragma once

#include "usdcc/usd/orbit_camera.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>

#include <optional>

namespace usdcc::usd {

// Looking through a stage's camera from a viewport, and moving between it and the orbit
// camera (docs/VIEWPORT_CAMERA.md §2.3, §3). Qt-free, for tests.

// A camera's lengths (clipping range, focus distance, an orthographic aperture) are in
// its own space, so a scaled camera, or one under a scaled parent, has them scaled too in
// the world, as Hydra draws it. This is that scale: the lengths of its transform's X, Y
// and Z axes, each 1 where the transform squashes an axis flat.
PXR_NS::GfVec3d stageCameraScale(const PXR_NS::GfCamera& camera);

// The camera's focus distance in the world (along its view), or nothing when it has none.
std::optional<double> stageCameraFocusDistance(const PXR_NS::GfCamera& camera);

// What a viewport `aspect` wide (width over height) sees through `camera`: its frustum,
// with the aperture fitted inside the window (CameraUtilFit) so the whole frame shows,
// and more beside it or above and below it where the shapes differ. Its clipping range
// and an orthographic window are in the world (stageCameraScale()).
PXR_NS::GfFrustum stageCameraFrustum(const PXR_NS::GfCamera& camera, double aspect);

// The camera-to-world matrix of an orbit camera's axes: it looks down its -Z with +Y up,
// as a UsdGeomCamera does.
PXR_NS::GfMatrix4d orbitCameraToWorld(const OrbitCameraAxes& axes);

// The orbit camera's pose that sees what `frustum` sees (Detach), with its target
// `targetDistance` in front of the eye (the default distance when that isn't more than
// 0). `currentYaw` is kept looking straight up or down, as orbitAnglesLookingFrom()
// does; the roll then makes up the difference. An off-centre window (a lens shift) can't
// be had: the pose sees the same width and height, centred.
struct OrbitPose {
    PXR_NS::GfVec3d target{0.0};
    double distance = kDefaultOrbitDistance;
    double yaw = kDefaultOrbitYaw;
    double pitch = kDefaultOrbitPitch;
    double roll = 0.0;
    bool orthographic = false;
    double orthoSize = kDefaultOrbitDistance;  // orthographic: the view's height
    std::optional<double> fovY;                // perspective: the vertical field of view
};
OrbitPose orbitPoseFromFrustum(const PXR_NS::GfFrustum& frustum, double targetDistance, double currentYaw, bool zUp);

// `camera`, changed to see what an orbit view sees in a window `aspect` wide, as
// stageCameraFrustum() fits it: the view's transform and projection, and the lens for
// them. Perspective, the focal length changes; orthographic, the apertures, keeping their
// shape, in `camera`'s own space (stageCameraScale(): the camera keeps its scale). Clipping,
// aperture offsets, focus and the rest stay as they were.
struct OrbitView {
    PXR_NS::GfMatrix4d cameraToWorld{1.0};
    bool orthographic = false;
    double fovY = 45.0;                        // perspective
    double orthoSize = kDefaultOrbitDistance;  // orthographic
};
PXR_NS::GfCamera cameraSeeingView(const PXR_NS::GfCamera& camera, const OrbitView& view, double aspect);

}  // namespace usdcc::usd
