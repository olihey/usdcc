#include "usdcc/tools/scale_tool.h"

#include "usdcc/tools/gizmo_shapes.h"

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
constexpr PXR_NS::GfVec4f kColorCenter(0.9f, 0.9f, 0.9f, 1.0f);
// Ratios below this are refused (rather than clamped) to keep a fast
// mouse-away-from-origin motion from ever flipping a prim through zero
// scale into a mirrored/degenerate one.
constexpr double kMinScaleRatio = 0.05;
}  // namespace

GizmoGeometry ScaleTool::buildGizmo(const ToolContext& ctx) const {
    GizmoGeometry geo;
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        return geo;
    }
    const GizmoFrame frame = gizmoFrame(xformable);
    const double scale = gizmoScale(frame.origin, ctx.cameraPos);

    struct AxisEntry {
        PXR_NS::GfVec3d axis;
        PXR_NS::GfVec4f color;
        Handle handle;
    };
    const AxisEntry axes[] = {{frame.axisX, kColorX, Handle::AxisX},
                              {frame.axisY, kColorY, Handle::AxisY},
                              {frame.axisZ, kColorZ, Handle::AxisZ}};
    for (const auto& entry : axes) {
        const PXR_NS::GfVec4f color = (m_activeHandle == entry.handle) ? kColorActive : entry.color;
        shapes::appendAxisLine(geo, frame.origin, entry.axis, scale * 0.8, color);
        shapes::appendBox(geo, frame.origin + entry.axis * scale * 0.85, scale * 0.05, color);
    }

    shapes::appendBox(geo, frame.origin, scale * 0.06, m_activeHandle == Handle::Center ? kColorActive : kColorCenter);
    return geo;
}

ScaleTool::Handle ScaleTool::pickHandle(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                        const GizmoFrame& frame) const {
    const double scale = gizmoScale(frame.origin, ctx.cameraPos);

    struct AxisEntry {
        PXR_NS::GfVec3d axis;
        Handle handle;
    };
    const AxisEntry axes[] = {
        {frame.axisX, Handle::AxisX}, {frame.axisY, Handle::AxisY}, {frame.axisZ, Handle::AxisZ}};

    Handle best = Handle::None;
    double bestDistance = kPickThresholdNdc;

    const double centerDistance = (ndcPos - projectToNdc(frame.origin, ctx.viewMatrix, ctx.projMatrix)).GetLength();
    if (centerDistance < bestDistance) {
        bestDistance = centerDistance;
        best = Handle::Center;
    }

    for (const auto& entry : axes) {
        const double d =
            distanceToSegmentNdc(ndcPos, frame.origin, frame.origin + entry.axis * scale, ctx.viewMatrix,
                                  ctx.projMatrix);
        if (d < bestDistance) {
            bestDistance = d;
            best = entry.handle;
        }
    }
    return best;
}

namespace {
PXR_NS::GfVec3f readScale(const PXR_NS::UsdGeomXformOp& op) {
    if (!op) {
        return PXR_NS::GfVec3f(1.0f, 1.0f, 1.0f);
    }
    if (op.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionFloat) {
        PXR_NS::GfVec3f v(1.0f, 1.0f, 1.0f);
        op.Get(&v);
        return v;
    }
    PXR_NS::GfVec3d v(1.0, 1.0, 1.0);
    op.Get(&v);
    return PXR_NS::GfVec3f(v);
}

void writeScale(const PXR_NS::UsdGeomXformOp& op, const PXR_NS::GfVec3f& value) {
    if (op.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionFloat) {
        op.Set(value);
    } else {
        op.Set(PXR_NS::GfVec3d(value));
    }
}
}  // namespace

ToolResult ScaleTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        m_activeHandle = Handle::None;
        return {};
    }
    const GizmoFrame frame = gizmoFrame(xformable);
    m_activeHandle = pickHandle(ctx, ndcPos, frame);
    if (m_activeHandle == Handle::None) {
        return {};
    }
    m_dragFrame = frame;
    m_dragStartScale = readScale(getOrCreateOp(xformable, PXR_NS::UsdGeomXformOp::TypeScale));

    PXR_NS::GfVec3d axis(0.0);
    switch (m_activeHandle) {
        case Handle::AxisX:
            axis = frame.axisX;
            break;
        case Handle::AxisY:
            axis = frame.axisY;
            break;
        case Handle::AxisZ:
            axis = frame.axisZ;
            break;
        default:
            break;
    }

    if (m_activeHandle == Handle::Center) {
        const auto hit = intersectPlane(ray, frame.origin, (frame.origin - ctx.cameraPos).GetNormalized());
        if (!hit) {
            m_activeHandle = Handle::None;
            return {};
        }
        m_dragStartParam = std::max(1e-6, (*hit - frame.origin).GetLength());
    } else {
        const PXR_NS::GfVec3d pointOnAxis = closestPointOnAxis(ray, frame.origin, axis);
        m_dragStartParam = PXR_NS::GfDot(pointOnAxis - frame.origin, axis);
        if (std::abs(m_dragStartParam) < 1e-6) {
            m_dragStartParam = m_dragStartParam < 0.0 ? -1e-6 : 1e-6;
        }
    }

    ToolResult result;
    result.handled = true;
    return result;
}

ToolResult ScaleTool::mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d&, const PXR_NS::GfRay& ray) {
    if (m_activeHandle == Handle::None) {
        return {};
    }
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        return {};
    }

    double ratio = 1.0;
    if (m_activeHandle == Handle::Center) {
        const auto hit = intersectPlane(ray, m_dragFrame.origin, (m_dragFrame.origin - ctx.cameraPos).GetNormalized());
        if (!hit) {
            ToolResult result;
            result.handled = true;
            return result;
        }
        const double param = std::max(1e-6, (*hit - m_dragFrame.origin).GetLength());
        ratio = param / m_dragStartParam;
    } else {
        PXR_NS::GfVec3d axis(0.0);
        switch (m_activeHandle) {
            case Handle::AxisX:
                axis = m_dragFrame.axisX;
                break;
            case Handle::AxisY:
                axis = m_dragFrame.axisY;
                break;
            case Handle::AxisZ:
                axis = m_dragFrame.axisZ;
                break;
            default:
                return {};
        }
        const PXR_NS::GfVec3d pointOnAxis = closestPointOnAxis(ray, m_dragFrame.origin, axis);
        const double param = PXR_NS::GfDot(pointOnAxis - m_dragFrame.origin, axis);
        ratio = param / m_dragStartParam;
    }

    if (!std::isfinite(ratio) || std::abs(ratio) < kMinScaleRatio) {
        ToolResult result;
        result.handled = true;
        return result;
    }

    PXR_NS::UsdGeomXformOp scaleOp = getOrCreateOp(xformable, PXR_NS::UsdGeomXformOp::TypeScale);
    if (!scaleOp) {
        return {};
    }

    PXR_NS::GfVec3f newScale = m_dragStartScale;
    if (m_activeHandle == Handle::Center) {
        newScale = m_dragStartScale * static_cast<float>(ratio);
    } else {
        const int axisIndex = m_activeHandle == Handle::AxisX ? 0 : (m_activeHandle == Handle::AxisY ? 1 : 2);
        newScale[axisIndex] = m_dragStartScale[axisIndex] * static_cast<float>(ratio);
    }
    writeScale(scaleOp, newScale);

    ToolResult result;
    result.handled = true;
    return result;
}

void ScaleTool::mouseRelease() { m_activeHandle = Handle::None; }

}  // namespace usdcc::tools
