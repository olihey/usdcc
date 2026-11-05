#pragma once

#include <pxr/base/gf/vec3d.h>

#include <optional>

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

// The camera at `distance` from `target`, by yaw and pitch in degrees. Its axes come
// from the angles rather than from a world up vector, so they carry on smoothly over
// the poles and it ends up upside down, rather than flipping or breaking there.
OrbitCameraAxes orbitCameraAxes(const PXR_NS::GfVec3d& target, double distance, double yawDegrees, double pitchDegrees);

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
