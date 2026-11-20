#include "usdcc/usd/orbit_camera.h"

#include <pxr/base/gf/math.h>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

using PXR_NS::GfVec3d;

namespace {
// The orbit's own frame is Y-up: yaw about Y, pitch above XZ. A Z-up stage's is that
// frame turned a quarter about X, Y onto Z, which puts the front (+Z) on -Y as Maya has it.
GfVec3d fromYUp(const GfVec3d& v, bool zUp) { return zUp ? GfVec3d(v[0], -v[2], v[1]) : v; }
GfVec3d toYUp(const GfVec3d& v, bool zUp) { return zUp ? GfVec3d(v[0], v[2], -v[1]) : v; }
}  // namespace

OrbitCameraAxes orbitCameraAxes(const GfVec3d& target, double distance, double yawDegrees, double pitchDegrees, bool zUp,
                                double rollDegrees) {
    const double yaw = PXR_NS::GfDegreesToRadians(yawDegrees);
    const double pitch = PXR_NS::GfDegreesToRadians(pitchDegrees);
    const GfVec3d fromTarget =
        fromYUp(GfVec3d(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)), zUp);
    OrbitCameraAxes axes;
    axes.eye = target + fromTarget * distance;
    axes.forward = -fromTarget;
    // Depends on yaw alone, so it stays put going over a pole and up turns smoothly.
    axes.right = fromYUp(GfVec3d(std::cos(yaw), 0.0, -std::sin(yaw)), zUp);
    axes.up = PXR_NS::GfCross(axes.right, axes.forward);
    if (rollDegrees != 0.0) {
        const double roll = PXR_NS::GfDegreesToRadians(rollDegrees);
        const double c = std::cos(roll), s = std::sin(roll);
        const GfVec3d right = axes.right * c - axes.up * s;
        const GfVec3d up = axes.up * c + axes.right * s;
        axes.right = right;
        axes.up = up;
    }
    return axes;
}

OrbitFraming frameOrbitCamera(const PXR_NS::GfRange3d& box, double verticalFovDegrees, double aspect,
                              double currentDistance) {
    OrbitFraming framing;
    framing.target = box.GetMidpoint();
    framing.distance = currentDistance;
    const double radius = box.GetSize().GetLength() / 2.0;
    if (!(radius > 0.0) || !std::isfinite(radius)) return framing;
    const double halfVertical = PXR_NS::GfDegreesToRadians(verticalFovDegrees) / 2.0;
    const double halfHorizontal = std::atan(std::tan(halfVertical) * std::max(aspect, 1e-6));
    // A sphere is in view when its centre is radius / sin(half angle) away: the planes
    // of the view then touch it.
    framing.distance = radius / std::sin(std::min(halfVertical, halfHorizontal));
    return framing;
}

OrbitClipping orbitClipping(double distance) {
    const OrbitClipping atDefault;
    const double scale = distance / kDefaultOrbitDistance;
    return {atDefault.nearClip * scale, atDefault.farClip * std::max(1.0, scale)};
}

OrbitAngles orbitAnglesLookingFrom(const GfVec3d& direction, double currentYaw, bool zUp) {
    const double length = direction.GetLength();
    if (length <= 0.0) return {currentYaw, 0.0};
    const GfVec3d d = toYUp(direction / length, zUp);
    OrbitAngles angles;
    angles.pitch = PXR_NS::GfRadiansToDegrees(std::asin(std::clamp(d[1], -1.0, 1.0)));
    const double horizontal = std::hypot(d[0], d[2]);
    angles.yaw = horizontal < 1e-9 ? currentYaw : PXR_NS::GfRadiansToDegrees(std::atan2(d[0], d[2]));
    return angles;
}

const char* cameraViewName(CameraView view) {
    switch (view) {
        case CameraView::Persp: return "persp";
        case CameraView::Top: return "top";
        case CameraView::Bottom: return "bottom";
        case CameraView::Front: return "front";
        case CameraView::Back: return "back";
        case CameraView::Right: return "right";
        case CameraView::Left: return "left";
    }
    return "persp";
}

const char* cameraViewLabel(CameraView view) {
    switch (view) {
        case CameraView::Persp: return "Persp";
        case CameraView::Top: return "Top";
        case CameraView::Bottom: return "Bottom";
        case CameraView::Front: return "Front";
        case CameraView::Back: return "Back";
        case CameraView::Right: return "Right";
        case CameraView::Left: return "Left";
    }
    return "Persp";
}

std::optional<CameraView> cameraViewFromName(std::string_view name) {
    for (CameraView view : kCameraViews) {
        if (name == cameraViewName(view)) return view;
    }
    return std::nullopt;
}

GfVec3d cameraViewEyeDirection(CameraView view, bool zUp) {
    // Y-up: front on +Z; Z-up: front on -Y, as Maya and the HUD's triad name them.
    switch (view) {
        case CameraView::Persp: return GfVec3d(0.0);
        case CameraView::Top: return zUp ? GfVec3d(0, 0, 1) : GfVec3d(0, 1, 0);
        case CameraView::Bottom: return zUp ? GfVec3d(0, 0, -1) : GfVec3d(0, -1, 0);
        case CameraView::Front: return zUp ? GfVec3d(0, -1, 0) : GfVec3d(0, 0, 1);
        case CameraView::Back: return zUp ? GfVec3d(0, 1, 0) : GfVec3d(0, 0, -1);
        case CameraView::Right: return GfVec3d(1, 0, 0);
        case CameraView::Left: return GfVec3d(-1, 0, 0);
    }
    return GfVec3d(0.0);
}

OrbitAngles orbitAnglesForView(CameraView view, bool zUp) {
    if (view == CameraView::Persp) return {};
    // Straight down or up the up axis, yaw 0 puts X to the right. The angles come out the
    // same for either up axis: the table is the Y-up one, turned as the camera is.
    return orbitAnglesLookingFrom(cameraViewEyeDirection(view, zUp), 0.0, zUp);
}

std::optional<CameraView> cameraViewLookingFrom(const GfVec3d& direction, bool zUp) {
    const double length = direction.GetLength();
    if (length <= 0.0) return std::nullopt;
    for (CameraView view : kCameraViews) {
        if (view == CameraView::Persp) continue;
        if (PXR_NS::GfDot(cameraViewEyeDirection(view, zUp), direction / length) > 0.999) return view;
    }
    return std::nullopt;
}

OrthographicFraming frameOrthographic(const PXR_NS::GfRange3d& box, const OrbitCameraAxes& axes, double aspect,
                                      double currentSize) {
    OrthographicFraming framing;
    framing.target = box.GetMidpoint();
    framing.size = currentSize;
    if (box.IsEmpty()) return framing;
    double minRight = 0.0, maxRight = 0.0, minUp = 0.0, maxUp = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
        const GfVec3d offset = box.GetCorner(corner) - framing.target;
        const double right = PXR_NS::GfDot(offset, axes.right);
        const double up = PXR_NS::GfDot(offset, axes.up);
        minRight = std::min(minRight, right);
        maxRight = std::max(maxRight, right);
        minUp = std::min(minUp, up);
        maxUp = std::max(maxUp, up);
    }
    const double width = maxRight - minRight;
    const double height = maxUp - minUp;
    const double size = std::max(height, width / std::max(aspect, 1e-6));
    if (!(size > 0.0) || !std::isfinite(size)) return framing;
    // A tenth more, so the box doesn't touch the edges.
    framing.size = size * 1.1;
    return framing;
}

bool cameraViewTumbles(CameraView view, bool orthographic) { return !orthographic || view == CameraView::Persp; }

double orthographicSizeFor(double distance, double verticalFovDegrees) {
    return 2.0 * distance * std::tan(PXR_NS::GfDegreesToRadians(verticalFovDegrees) / 2.0);
}

double perspectiveDistanceFor(double size, double verticalFovDegrees) {
    const double tangent = std::tan(PXR_NS::GfDegreesToRadians(verticalFovDegrees) / 2.0);
    return tangent > 0.0 ? size / (2.0 * tangent) : size;
}

double panUnitsPerPixel(double depth, double verticalFovDegrees, double viewHeight) {
    // The view's height at that depth, over its height in pixels.
    return orthographicSizeFor(depth, verticalFovDegrees) / std::max(viewHeight, 1.0);
}

GfVec3d panOrbitTarget(const GfVec3d& target, const OrbitCameraAxes& axes, double dx, double dy, double unitsPerPixel) {
    return target + axes.right * (-dx * unitsPerPixel) + axes.up * (dy * unitsPerPixel);
}

std::optional<double> perspectiveDepthFromBuffer(double bufferDepth, const OrbitClipping& clipping) {
    if (!(bufferDepth >= 0.0) || bufferDepth >= 1.0) return std::nullopt;
    const double nearClip = clipping.nearClip;
    const double farClip = clipping.farClip;
    // The projection's z, undone: buffer depth d is NDC z = 2d - 1.
    const double depth = farClip * nearClip / (farClip - bufferDepth * (farClip - nearClip));
    if (!(depth > 0.0) || !std::isfinite(depth)) return std::nullopt;
    return depth;
}

std::optional<double> groundDepthAlongRay(const OrbitCameraAxes& axes, const GfVec3d& direction, bool zUp, double halfExtent) {
    const int upAxis = zUp ? 2 : 1;
    if (std::abs(direction[upAxis]) < 1e-12) return std::nullopt;  // along the ground
    const double along = -axes.eye[upAxis] / direction[upAxis];
    if (!(along > 0.0) || !std::isfinite(along)) return std::nullopt;  // behind the eye
    const GfVec3d point = axes.eye + direction * along;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis != upAxis && std::abs(point[axis]) > halfExtent) return std::nullopt;
    }
    const double depth = PXR_NS::GfDot(point - axes.eye, axes.forward);
    if (!(depth > 0.0)) return std::nullopt;
    return depth;
}

double wrapDegrees(double degrees) {
    const double wrapped = std::fmod(degrees, 360.0);
    if (wrapped > 180.0) return wrapped - 360.0;
    if (wrapped <= -180.0) return wrapped + 360.0;
    return wrapped;
}

bool orbitIsUpsideDown(double pitchDegrees) { return std::cos(PXR_NS::GfDegreesToRadians(pitchDegrees)) < 0.0; }

std::optional<CursorPosition> wrapCursorAtEdges(int x, int y, int left, int top, int right, int bottom) {
    // At the edge or, after a quick move, past it onto another screen. The new place
    // is two pixels in, so that it doesn't count as at the edge itself.
    constexpr int kEdge = 1;
    constexpr int kInside = 2;
    CursorPosition wrapped{x, y};
    bool moved = false;
    if (x <= left + kEdge - 1) {
        wrapped.x = right - kInside;
        moved = true;
    } else if (x >= right - kEdge + 1) {
        wrapped.x = left + kInside;
        moved = true;
    }
    if (y <= top + kEdge - 1) {
        wrapped.y = bottom - kInside;
        moved = true;
    } else if (y >= bottom - kEdge + 1) {
        wrapped.y = top + kInside;
        moved = true;
    }
    if (!moved) return std::nullopt;
    return wrapped;
}

}  // namespace usdcc::usd
