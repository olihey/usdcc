#include "usdcc/tools/rotate_tool.h"

#include "usdcc/tools/gizmo_shapes.h"

#include <pxr/base/gf/math.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <algorithm>
#include <cmath>

namespace usdcc::tools {

namespace {
constexpr PXR_NS::GfVec4f kColorX(0.85f, 0.2f, 0.2f, 1.0f);
constexpr PXR_NS::GfVec4f kColorY(0.3f, 0.8f, 0.2f, 1.0f);
constexpr PXR_NS::GfVec4f kColorZ(0.25f, 0.4f, 0.95f, 1.0f);
constexpr PXR_NS::GfVec4f kColorActive(1.0f, 0.9f, 0.1f, 1.0f);

// Projects `point - origin` onto the plane perpendicular to `axis` and
// normalizes it — the reference vector used to measure rotation angle.
std::optional<PXR_NS::GfVec3d> ringVector(const PXR_NS::GfVec3d& point, const PXR_NS::GfVec3d& origin,
                                           const PXR_NS::GfVec3d& axis) {
    PXR_NS::GfVec3d v = point - origin;
    v -= axis * PXR_NS::GfDot(v, axis);
    if (v.GetLength() < 1e-9) {
        return std::nullopt;
    }
    return v.GetNormalized();
}
}  // namespace

PXR_NS::GfVec3d RotateTool::axisForHandle(Handle handle, const GizmoFrame& frame) {
    switch (handle) {
        case Handle::AxisX:
            return frame.axisX;
        case Handle::AxisY:
            return frame.axisY;
        case Handle::AxisZ:
            return frame.axisZ;
        default:
            return PXR_NS::GfVec3d(0.0);
    }
}

GizmoGeometry RotateTool::buildGizmo(const ToolContext& ctx) const {
    GizmoGeometry geo;
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        return geo;
    }
    const GizmoFrame frame = gizmoFrame(xformable);
    const double scale = gizmoScale(frame.origin, ctx.cameraPos);

    struct RingEntry {
        PXR_NS::GfVec3d axis;
        PXR_NS::GfVec4f color;
        Handle handle;
    };
    const RingEntry rings[] = {{frame.axisX, kColorX, Handle::AxisX},
                               {frame.axisY, kColorY, Handle::AxisY},
                               {frame.axisZ, kColorZ, Handle::AxisZ}};
    for (const auto& entry : rings) {
        const PXR_NS::GfVec4f color = (m_activeHandle == entry.handle) ? kColorActive : entry.color;
        shapes::appendRing(geo, frame.origin, entry.axis, scale, color);
    }
    return geo;
}

RotateTool::Handle RotateTool::pickRing(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                         const GizmoFrame& frame) const {
    const double scale = gizmoScale(frame.origin, ctx.cameraPos);

    struct RingEntry {
        PXR_NS::GfVec3d axis;
        Handle handle;
    };
    const RingEntry rings[] = {
        {frame.axisX, Handle::AxisX}, {frame.axisY, Handle::AxisY}, {frame.axisZ, Handle::AxisZ}};

    Handle best = Handle::None;
    double bestDistance = kPickThresholdNdc;

    for (const auto& entry : rings) {
        GizmoGeometry ringGeo;
        shapes::appendRing(ringGeo, frame.origin, entry.axis, scale, PXR_NS::GfVec4f(0.0f));
        for (const auto& line : ringGeo.lines) {
            const double d = distanceToSegmentNdc(ndcPos, line.a, line.b, ctx.viewMatrix, ctx.projMatrix);
            if (d < bestDistance) {
                bestDistance = d;
                best = entry.handle;
            }
        }
    }
    return best;
}

ToolResult RotateTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        m_activeHandle = Handle::None;
        return {};
    }
    const GizmoFrame frame = gizmoFrame(xformable);
    m_activeHandle = pickRing(ctx, ndcPos, frame);
    if (m_activeHandle == Handle::None) {
        return {};
    }
    m_dragFrame = frame;

    const PXR_NS::GfVec3d axis = axisForHandle(m_activeHandle, frame);
    const auto hit = intersectPlane(ray, frame.origin, axis);
    const auto vector = hit ? ringVector(*hit, frame.origin, axis) : std::nullopt;
    if (!vector) {
        m_activeHandle = Handle::None;
        return {};
    }
    m_lastVector = *vector;

    ToolResult result;
    result.handled = true;
    return result;
}

ToolResult RotateTool::mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d&, const PXR_NS::GfRay& ray) {
    if (m_activeHandle == Handle::None) {
        return {};
    }
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        return {};
    }

    const PXR_NS::GfVec3d axis = axisForHandle(m_activeHandle, m_dragFrame);
    const auto hit = intersectPlane(ray, m_dragFrame.origin, axis);
    const auto currentVector = hit ? ringVector(*hit, m_dragFrame.origin, axis) : std::nullopt;
    if (!currentVector) {
        ToolResult result;
        result.handled = true;
        return result;
    }

    const double cosAngle = std::clamp(PXR_NS::GfDot(m_lastVector, *currentVector), -1.0, 1.0);
    double deltaAngle = std::acos(cosAngle);
    const PXR_NS::GfVec3d cross = PXR_NS::GfCross(m_lastVector, *currentVector);
    if (PXR_NS::GfDot(cross, axis) < 0.0) {
        deltaAngle = -deltaAngle;
    }
    const double deltaDegrees = PXR_NS::GfRadiansToDegrees(deltaAngle);
    m_lastVector = *currentVector;

    if (std::abs(deltaDegrees) < 1e-6) {
        ToolResult result;
        result.handled = true;
        return result;
    }

    PXR_NS::UsdGeomXformOp rotateOp = getOrCreateOp(xformable, PXR_NS::UsdGeomXformOp::TypeRotateXYZ);
    if (!rotateOp) {
        return {};
    }

    const bool isDouble = rotateOp.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionDouble;
    const int axisIndex = m_activeHandle == Handle::AxisX ? 0 : (m_activeHandle == Handle::AxisY ? 1 : 2);
    if (isDouble) {
        PXR_NS::GfVec3d v(0.0);
        rotateOp.Get(&v);
        v[axisIndex] += deltaDegrees;
        rotateOp.Set(v);
    } else {
        PXR_NS::GfVec3f v(0.0f);
        rotateOp.Get(&v);
        v[axisIndex] += static_cast<float>(deltaDegrees);
        rotateOp.Set(v);
    }

    ToolResult result;
    result.handled = true;
    return result;
}

void RotateTool::mouseRelease() { m_activeHandle = Handle::None; }

}  // namespace usdcc::tools
