#include "usdcc/usd/camera_framing.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/range1d.h>
#include <pxr/base/gf/range2d.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdRender/settings.h>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

using PXR_NS::GfCamera;
using PXR_NS::GfFrustum;
using PXR_NS::GfMatrix4d;
using PXR_NS::GfVec3d;

GfVec3d stageCameraScale(const GfCamera& camera) {
    const GfMatrix4d& m = camera.GetTransform();
    GfVec3d scale(1.0);
    for (int i = 0; i < 3; ++i) {
        const double length = GfVec3d(m[i][0], m[i][1], m[i][2]).GetLength();
        if (length > 1e-12 && std::isfinite(length)) scale[i] = length;
    }
    return scale;
}

std::optional<double> stageCameraFocusDistance(const GfCamera& camera) {
    const double distance = camera.GetFocusDistance() * stageCameraScale(camera)[2];
    if (!(distance > 0.0) || !std::isfinite(distance)) return std::nullopt;
    return distance;
}

GfFrustum stageCameraFrustum(const GfCamera& camera, double aspect) {
    // GetFrustum() takes the transform's rotation and position only.
    GfFrustum frustum = camera.GetFrustum();
    const GfVec3d scale = stageCameraScale(camera);
    if (scale != GfVec3d(1.0)) {
        const PXR_NS::GfRange1d nearFar = frustum.GetNearFar();
        frustum.SetNearFar(PXR_NS::GfRange1d(nearFar.GetMin() * scale[2], nearFar.GetMax() * scale[2]));
        if (frustum.GetProjectionType() == GfFrustum::Orthographic) {
            const PXR_NS::GfRange2d window = frustum.GetWindow();
            const PXR_NS::GfVec2d s(scale[0], scale[1]);
            frustum.SetWindow(PXR_NS::GfRange2d(PXR_NS::GfCompMult(window.GetMin(), s), PXR_NS::GfCompMult(window.GetMax(), s)));
        }
    }
    if (aspect > 0.0 && std::isfinite(aspect)) PXR_NS::CameraUtilConformWindow(&frustum, PXR_NS::CameraUtilFit, aspect);
    return frustum;
}

GfMatrix4d orbitCameraToWorld(const OrbitCameraAxes& axes) {
    GfMatrix4d cameraToWorld(1.0);
    cameraToWorld.SetRow(0, PXR_NS::GfVec4d(axes.right[0], axes.right[1], axes.right[2], 0.0));
    cameraToWorld.SetRow(1, PXR_NS::GfVec4d(axes.up[0], axes.up[1], axes.up[2], 0.0));
    cameraToWorld.SetRow(2, PXR_NS::GfVec4d(-axes.forward[0], -axes.forward[1], -axes.forward[2], 0.0));
    cameraToWorld.SetRow(3, PXR_NS::GfVec4d(axes.eye[0], axes.eye[1], axes.eye[2], 1.0));
    return cameraToWorld;
}

OrbitPose orbitPoseFromFrustum(const GfFrustum& frustum, double targetDistance, double currentYaw, bool zUp) {
    const GfMatrix4d m = frustum.ComputeViewInverse();
    const GfVec3d up = GfVec3d(m[1][0], m[1][1], m[1][2]).GetNormalized();
    const GfVec3d forward = -GfVec3d(m[2][0], m[2][1], m[2][2]).GetNormalized();
    const GfVec3d eye = frustum.GetPosition();

    OrbitPose pose;
    pose.distance = targetDistance > 0.0 && std::isfinite(targetDistance) ? targetDistance : kDefaultOrbitDistance;
    pose.target = eye + forward * pose.distance;
    const OrbitAngles angles = orbitAnglesLookingFrom(-forward, currentYaw, zUp);
    pose.yaw = angles.yaw;
    pose.pitch = angles.pitch;
    // The roll that turns the level camera's up onto this one's (orbitCameraAxes()).
    const OrbitCameraAxes level = orbitCameraAxes(pose.target, pose.distance, pose.yaw, pose.pitch, zUp);
    pose.roll = PXR_NS::GfRadiansToDegrees(std::atan2(PXR_NS::GfDot(up, level.right), PXR_NS::GfDot(up, level.up)));
    if (std::abs(pose.roll) < 1e-9) pose.roll = 0.0;

    const PXR_NS::GfVec2d size = frustum.GetWindow().GetSize();
    pose.orthographic = frustum.GetProjectionType() == GfFrustum::Orthographic;
    if (pose.orthographic) {
        pose.orthoSize = size[1];
    } else {
        // The window is on the plane one unit in front of the eye.
        pose.fovY = PXR_NS::GfRadiansToDegrees(2.0 * std::atan(size[1] / 2.0));
    }
    return pose;
}

GfCamera cameraSeeingView(const GfCamera& camera, const OrbitView& view, double aspect) {
    GfCamera result = camera;
    result.SetTransform(view.cameraToWorld);
    aspect = aspect > 0.0 && std::isfinite(aspect) ? aspect : 1.0;
    double horizontal = camera.GetHorizontalAperture();
    double vertical = camera.GetVerticalAperture();
    if (!(horizontal > 0.0) || !(vertical > 0.0)) {
        horizontal = GfCamera::DEFAULT_HORIZONTAL_APERTURE;
        vertical = GfCamera::DEFAULT_VERTICAL_APERTURE;
    }
    const GfVec3d scale = stageCameraScale(camera);
    if (view.orthographic) {
        // The apertures are in the camera's own space: the world's over its scale.
        const double cameraAspect = (horizontal * scale[0]) / (vertical * scale[1]);
        // stageCameraFrustum() fits the aperture inside the window: a narrower camera fills
        // the window's height, a wider one its width.
        result.SetProjection(GfCamera::Orthographic);
        const double height = std::max(view.orthoSize, 1e-9);
        double worldWidth = 0.0;
        double worldHeight = 0.0;
        if (cameraAspect <= aspect) {
            worldHeight = height;
            worldWidth = height * cameraAspect;
        } else {
            worldWidth = height * aspect;
            worldHeight = worldWidth / cameraAspect;
        }
        horizontal = worldWidth / scale[0] / GfCamera::APERTURE_UNIT;
        vertical = worldHeight / scale[1] / GfCamera::APERTURE_UNIT;
        result.SetHorizontalAperture(static_cast<float>(horizontal));
        result.SetVerticalAperture(static_cast<float>(vertical));
    } else {
        const double cameraAspect = horizontal / vertical;
        const bool fillsHeight = cameraAspect <= aspect;
        result.SetProjection(GfCamera::Perspective);
        result.SetHorizontalAperture(static_cast<float>(horizontal));
        result.SetVerticalAperture(static_cast<float>(vertical));
        // The window one unit in front of the eye; the aperture over the focal length
        // (in the same units: GfCamera's APERTURE_UNIT and FOCAL_LENGTH_UNIT agree).
        const double height = 2.0 * std::tan(PXR_NS::GfDegreesToRadians(std::clamp(view.fovY, 1e-3, 179.0)) / 2.0);
        const double focal = fillsHeight ? vertical / height : horizontal / (height * aspect);
        result.SetFocalLength(static_cast<float>(focal));
    }
    return result;
}

std::optional<Resolution> parseResolution(const std::string& text) {
    std::string compact;
    for (const char c : text) {
        if (c != ' ') compact += c;
    }
    // "×" is two bytes in UTF-8; "x" or "X" one.
    std::size_t at = compact.find("\xC3\x97");
    std::size_t skip = 2;
    if (at == std::string::npos) {
        at = compact.find_first_of("xX");
        skip = 1;
    }
    if (at == std::string::npos || at == 0) return std::nullopt;
    try {
        std::size_t used = 0;
        const int width = std::stoi(compact.substr(0, at), &used);
        if (used != at) return std::nullopt;
        const std::string rest = compact.substr(at + skip);
        const int height = std::stoi(rest, &used);
        if (used != rest.size()) return std::nullopt;
        if (width < kMinResolution || height < kMinResolution || width > kMaxResolution || height > kMaxResolution) {
            return std::nullopt;
        }
        return Resolution{width, height};
    } catch (...) {
        return std::nullopt;
    }
}

std::string resolutionName(const Resolution& resolution) {
    return std::to_string(resolution.width) + "x" + std::to_string(resolution.height);
}

std::optional<Resolution> renderSettingsResolution(const PXR_NS::UsdStagePtr& stage) {
    if (!stage) return std::nullopt;
    PXR_NS::UsdRenderSettings settings = PXR_NS::UsdRenderSettings::GetStageRenderSettings(stage);
    if (!settings) {
        for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
            if (prim.IsA<PXR_NS::UsdRenderSettings>()) {
                settings = PXR_NS::UsdRenderSettings(prim);
                break;
            }
        }
    }
    PXR_NS::GfVec2i size(0, 0);
    if (!settings || !settings.GetResolutionAttr().Get(&size) || size[0] <= 0 || size[1] <= 0) return std::nullopt;
    return Resolution{size[0], size[1]};
}

ViewRect gateRect(double width, double height, const std::optional<Resolution>& resolution) {
    if (!resolution || resolution->width <= 0 || resolution->height <= 0 || width <= 0.0 || height <= 0.0) {
        return ViewRect{0.0, 0.0, std::max(width, 0.0), std::max(height, 0.0)};
    }
    return aspectRect(ViewRect{0.0, 0.0, width, height}, double(resolution->width) / double(resolution->height));
}

ViewRect safeRect(const ViewRect& rect, double percent) {
    const double f = std::clamp(percent, 0.0, 100.0) / 100.0;
    const double w = rect.width * f, h = rect.height * f;
    return ViewRect{rect.x + (rect.width - w) / 2.0, rect.y + (rect.height - h) / 2.0, w, h};
}

ViewRect aspectRect(const ViewRect& rect, double aspect) {
    if (!(aspect > 0.0) || rect.height <= 0.0) return rect;
    double w = rect.width, h = rect.width / aspect;
    if (h > rect.height) {
        h = rect.height;
        w = h * aspect;
    }
    return ViewRect{rect.x + (rect.width - w) / 2.0, rect.y + (rect.height - h) / 2.0, w, h};
}

PXR_NS::GfFrustum overscanFrustum(const PXR_NS::GfFrustum& frustum, const ViewRect& gate, double width, double height) {
    if (gate.width <= 0.0 || gate.height <= 0.0) return frustum;
    PXR_NS::GfFrustum widened = frustum;
    const PXR_NS::GfRange2d window = frustum.GetWindow();
    const PXR_NS::GfVec2d size = window.GetSize();
    // The gate's window spans the gate; the window's pixels outside it carry on at the same scale.
    const double perPixelX = size[0] / gate.width, perPixelY = size[1] / gate.height;
    const PXR_NS::GfVec2d min = window.GetMin();
    const PXR_NS::GfVec2d max = window.GetMax();
    // Pixel rows count down from the top; the window's y up.
    widened.SetWindow(PXR_NS::GfRange2d(
        PXR_NS::GfVec2d(min[0] - gate.x * perPixelX, min[1] - (height - gate.y - gate.height) * perPixelY),
        PXR_NS::GfVec2d(max[0] + (width - gate.x - gate.width) * perPixelX, max[1] + gate.y * perPixelY)));
    return widened;
}

std::pair<int, int> scaledBufferSize(int width, int height, int percent) {
    const double f = std::clamp(percent, 1, 100) / 100.0;
    return {std::max(1, int(std::lround(width * f))), std::max(1, int(std::lround(height * f)))};
}

}  // namespace usdcc::usd
