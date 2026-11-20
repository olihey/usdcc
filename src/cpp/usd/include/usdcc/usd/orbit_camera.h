#pragma once

#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec3d.h>

#include <optional>
#include <string_view>

namespace usdcc::usd {

// The viewport's orbit camera, free to turn any way: Qt-free, for tests.
struct OrbitCameraAxes {
    PXR_NS::GfVec3d eye;
    PXR_NS::GfVec3d forward;
    PXR_NS::GfVec3d right;
    PXR_NS::GfVec3d up;
};

// Where a new viewport's camera starts, and where Reset Camera puts it: looking down
// at the origin from above the ground, at an angle. A positive pitch puts the eye
// above its target.
constexpr double kDefaultOrbitDistance = 10.0;
constexpr double kDefaultOrbitYaw = 45.0;
constexpr double kDefaultOrbitPitch = 20.0;

// Where the orbit camera goes to fit `box` in the view, keeping the way it looks: at the
// box's centre, far enough back that the box's bounding sphere fits both the vertical
// field of view (`verticalFovDegrees`) and the horizontal one (from `aspect`, width over
// height). A box of no size (a point) keeps `currentDistance`.
struct OrbitFraming {
    PXR_NS::GfVec3d target;
    double distance = kDefaultOrbitDistance;
};
OrbitFraming frameOrbitCamera(const PXR_NS::GfRange3d& box, double verticalFovDegrees, double aspect,
                              double currentDistance);

// The near and far clipping planes for an orbit camera `distance` from its target. At the
// default distance they are 0.1 and 10000; they scale with the distance, so whatever the
// camera is fitted to stays in between, however big or small. Closer in, only the near
// plane moves: the far one stays at 10000.
struct OrbitClipping {
    double nearClip = 0.1;
    double farClip = 10000.0;
};
OrbitClipping orbitClipping(double distance);

// The camera at `distance` from `target`, by yaw and pitch in degrees. Its axes come
// from the angles rather than from a world up vector, so they carry on smoothly over
// the poles and it ends up upside down, rather than flipping or breaking there.
//
// The angles are about the stage's up axis: yaw turns around it, pitch is the height
// above the ground. `zUp`: that axis is Z and the ground XY, else Y and XZ. The same
// angles look at the ground the same way on either, so a Z-up stage stands upright.
//
// `rollDegrees` turns the camera about the way it looks: positive turns its up towards
// its right, so the picture turns the other way. A stage camera's pose can have it
// (docs/VIEWPORT_CAMERA.md §2.3); the orbit camera's own drags leave it as it is.
OrbitCameraAxes orbitCameraAxes(const PXR_NS::GfVec3d& target, double distance, double yawDegrees, double pitchDegrees,
                                bool zUp = false, double rollDegrees = 0.0);

// The yaw and pitch, in degrees, that put the eye on the side `direction` points to from
// the target (any length but zero), looking back at it: (1, 0, 0) looks along -X from +X.
// Straight up or down (a pole) keeps `currentYaw`, as any yaw looks the same way there.
// For the camera orbitCameraAxes() makes with the same `zUp`.
// The viewport HUD's axis triad snaps the camera with it (docs/VIEWPORT_HUD.md §2.2).
struct OrbitAngles {
    double yaw = kDefaultOrbitYaw;
    double pitch = kDefaultOrbitPitch;
};
OrbitAngles orbitAnglesLookingFrom(const PXR_NS::GfVec3d& direction, double currentYaw, bool zUp = false);

// The named views a viewport can look from (docs/WORKSPACES.md §3.5.2), as in Maya
// by where the eye is. Persp is the free orbit camera.
enum class CameraView { Persp, Top, Bottom, Front, Back, Right, Left };
inline constexpr CameraView kCameraViews[] = {CameraView::Persp, CameraView::Top,   CameraView::Front, CameraView::Right,
                                              CameraView::Left,  CameraView::Back, CameraView::Bottom};
// "persp", "top", ... as workspace files and sessions write them.
const char* cameraViewName(CameraView view);
// "Persp", "Top", ... as the HUD and menus show them.
const char* cameraViewLabel(CameraView view);
std::optional<CameraView> cameraViewFromName(std::string_view name);
// The side of the target the eye is on for an axis view, following the stage's up
// axis: Top is always above the ground. Zero for Persp.
PXR_NS::GfVec3d cameraViewEyeDirection(CameraView view, bool zUp);
// The orbit angles of a view: the default ones for Persp. The same on either up axis,
// as the angles are about it.
OrbitAngles orbitAnglesForView(CameraView view, bool zUp);
// The axis view whose eye direction is `direction` (a HUD axis), if one is.
std::optional<CameraView> cameraViewLookingFrom(const PXR_NS::GfVec3d& direction, bool zUp);
// Whether the orbit drag tumbles the view (docs/WORKSPACES.md §3.5.3): an orthographic
// axis view stays on its axis; a perspective view, or an orthographic one that isn't on
// an axis ("Ortho"), turns.
bool cameraViewTumbles(CameraView view, bool orthographic);

// Fit to View for an orthographic camera: the target at the box's centre, and the
// view's height so that the box, seen along `forward` with `right` and `up` across
// the view, fits both ways with a little room (`aspect`: width over height). A box
// of no size keeps `currentSize`.
struct OrthographicFraming {
    PXR_NS::GfVec3d target;
    double size = kDefaultOrbitDistance;
};
OrthographicFraming frameOrthographic(const PXR_NS::GfRange3d& box, const OrbitCameraAxes& axes, double aspect,
                                      double currentSize);
// The orthographic view height that shows at the target what a perspective camera
// `distance` away with `verticalFovDegrees` does, and back.
double orthographicSizeFor(double distance, double verticalFovDegrees);
double perspectiveDistanceFor(double size, double verticalFovDegrees);

// Panning (the middle drag) moves the camera across its view so that what the cursor was
// on when the drag began stays under it. In a perspective view, how far that is depends
// on how far away the point is: nearer things pass the cursor faster.
//
// The scene units across the view that one pixel covers, `depth` in front of the eye
// (along the view, not along the cursor's ray), in a perspective view `viewHeight`
// pixels high. The camera's depths don't change while it pans, so it holds for the drag.
double panUnitsPerPixel(double depth, double verticalFovDegrees, double viewHeight);
// The target after the cursor moved `dx` pixels right and `dy` down: the camera goes the
// other way, and the scene with the cursor. Orthographic, a pixel covers the view's
// height over `viewHeight` whatever the depth.
PXR_NS::GfVec3d panOrbitTarget(const PXR_NS::GfVec3d& target, const OrbitCameraAxes& axes, double dx, double dy,
                               double unitsPerPixel);
// How far in front of the eye is what a perspective view's depth buffer holds as
// `bufferDepth`: 0 on the near plane, 1 on the far one. Nothing from there on: nothing
// was drawn at that pixel.
std::optional<double> perspectiveDepthFromBuffer(double bufferDepth, const OrbitClipping& clipping);
// How far in front of the eye (along `axes.forward`) the ray from it along `direction`
// meets the stage's ground, the plane through the origin that the grid is on, no further
// than `halfExtent` from the origin along either ground axis: where the grid is drawn.
// Nothing if it doesn't.
std::optional<double> groundDepthAlongRay(const OrbitCameraAxes& axes, const PXR_NS::GfVec3d& direction, bool zUp,
                                          double halfExtent);

// `degrees` as the same angle in (-180, 180], so that turning on and on doesn't lose
// precision.
double wrapDegrees(double degrees);

// Whether dragging should turn the other way to look the same: past a pole the
// camera is upside down, and without this the scene would move against the mouse.
bool orbitIsUpsideDown(double pitchDegrees);

// Where to put a cursor that has reached the edge of the screen spanning left to right
// and top to bottom (inclusive), so that a drag can carry on: just inside the opposite
// edge. Nothing if it isn't at an edge.
struct CursorPosition {
    int x = 0;
    int y = 0;
};
std::optional<CursorPosition> wrapCursorAtEdges(int x, int y, int left, int top, int right, int bottom);

}  // namespace usdcc::usd
