#include "usdcc/usd/orbit_camera.h"

#include <pxr/base/gf/math.h>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

using PXR_NS::GfVec3d;

OrbitCameraAxes orbitCameraAxes(const GfVec3d& target, double distance, double yawDegrees, double pitchDegrees) {
    const double yaw = PXR_NS::GfDegreesToRadians(yawDegrees);
    const double pitch = PXR_NS::GfDegreesToRadians(pitchDegrees);
    const GfVec3d fromTarget(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    OrbitCameraAxes axes;
    axes.eye = target + fromTarget * distance;
    axes.forward = -fromTarget;
    // Depends on yaw alone, so it stays put going over a pole and up turns smoothly.
    axes.right = GfVec3d(std::cos(yaw), 0.0, -std::sin(yaw));
    axes.up = PXR_NS::GfCross(axes.right, axes.forward);
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
