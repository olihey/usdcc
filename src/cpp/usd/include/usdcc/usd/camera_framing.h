#pragma once

#include "usdcc/usd/orbit_camera.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>

#include <pxr/usd/usd/stage.h>

#include <optional>
#include <string>
#include <utility>

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

// ---- The resolution gate and safe frames (docs/VIEWPORT_CAMERA.md §4, §6) ----------------

// An image size. A viewport only takes its shape from it: the gate is fitted in the window.
struct Resolution {
    int width = 0;
    int height = 0;
    bool operator==(const Resolution&) const = default;
};
inline constexpr int kMinResolution = 16;
inline constexpr int kMaxResolution = 16384;
// Viewport/resolution's values: "fill" (none), "renderSettings", or "<width>x<height>".
inline constexpr const char* kFillViewport = "fill";
inline constexpr const char* kRenderSettingsResolution = "renderSettings";
// "1920x1080" (or "1920 x 1080", "1920×1080"): the size, each side 16-16384. Nothing for
// "fill", "renderSettings" or anything else.
std::optional<Resolution> parseResolution(const std::string& text);
std::string resolutionName(const Resolution& resolution);
// The stage's render settings' resolution: its renderSettingsPrimPath's, else the first
// UsdRenderSettings prim's. Nothing without one.
std::optional<Resolution> renderSettingsResolution(const PXR_NS::UsdStagePtr& stage);

// A rectangle in a window's pixels, from its top left.
struct ViewRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double aspect() const { return height > 0.0 ? width / height : 1.0; }
    bool operator==(const ViewRect&) const = default;
};
// The gate in a window `width` by `height`: the resolution's shape, as large as fits, centred;
// the whole window without one.
ViewRect gateRect(double width, double height, const std::optional<Resolution>& resolution);
// `rect` shrunk to `percent` of its width and height about its centre: a safe frame.
ViewRect safeRect(const ViewRect& rect, double percent);
// The largest rect of `aspect` (width over height) in `rect`, centred: an aspect guide.
ViewRect aspectRect(const ViewRect& rect, double aspect);
// Render scale (§5): the size Hydra renders at for a framebuffer `width` by `height` at
// `percent` of it (25-100), at least a pixel each way.
std::pair<int, int> scaledBufferSize(int width, int height, int percent);
// The scales a viewport offers.
inline constexpr int kRenderScales[] = {100, 75, 50, 25};

// `frustum`, which sees the gate, widened to see the whole window around it (overscan), so
// the gate shows exactly what it frames and the window more beside it.
PXR_NS::GfFrustum overscanFrustum(const PXR_NS::GfFrustum& frustum, const ViewRect& gate, double width, double height);

}  // namespace usdcc::usd
