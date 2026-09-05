#include "usdcc/tools/move_tool.h"

#include "usdcc/tools/gizmo_shapes.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

namespace usdcc::tools {

namespace {
constexpr PXR_NS::GfVec4f kColorX(0.85f, 0.2f, 0.2f, 1.0f);
constexpr PXR_NS::GfVec4f kColorY(0.3f, 0.8f, 0.2f, 1.0f);
constexpr PXR_NS::GfVec4f kColorZ(0.25f, 0.4f, 0.95f, 1.0f);
constexpr PXR_NS::GfVec4f kColorActive(1.0f, 0.9f, 0.1f, 1.0f);
constexpr PXR_NS::GfVec4f kColorCenter(0.9f, 0.9f, 0.9f, 1.0f);

// Converts a world-space translation delta into the prim's own parent
// space, since a translate xformOp is authored relative to the parent, not
// the world. Only the parent's transform matters here — the prim's own
// existing ops (rotate/scale) must not also be un-applied, since they still
// have to affect the child's rendered position exactly as before.
PXR_NS::GfMatrix4d parentToWorld(const PXR_NS::UsdPrim& prim) {
    if (const PXR_NS::UsdPrim parent = prim.GetParent()) {
        if (const PXR_NS::UsdGeomXformable parentXformable{parent}) {
            return parentXformable.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode::Default());
        }
    }
    return PXR_NS::GfMatrix4d(1.0);
}
}  // namespace

GizmoGeometry MoveTool::buildGizmo(const ToolContext& ctx) const {
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
        shapes::appendCone(geo, frame.origin + entry.axis * scale, entry.axis, scale * 0.2, scale * 0.06, color);
    }

    struct PlaneEntry {
        PXR_NS::GfVec3d u, v;
        PXR_NS::GfVec4f color;
        Handle handle;
    };
    const PlaneEntry planes[] = {{frame.axisX, frame.axisY, kColorZ, Handle::PlaneXY},
                                 {frame.axisY, frame.axisZ, kColorX, Handle::PlaneYZ},
                                 {frame.axisZ, frame.axisX, kColorY, Handle::PlaneXZ}};
    for (const auto& entry : planes) {
        const PXR_NS::GfVec4f color = (m_activeHandle == entry.handle) ? kColorActive : entry.color;
        shapes::appendPlaneQuad(geo, frame.origin, entry.u, entry.v, scale * 0.2, scale * 0.2, color);
    }

    shapes::appendBox(geo, frame.origin, scale * 0.05, m_activeHandle == Handle::Center ? kColorActive : kColorCenter);
    return geo;
}

MoveTool::Handle MoveTool::pickHandle(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
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

    const double centerDistance =
        (ndcPos - projectToNdc(frame.origin, ctx.viewMatrix, ctx.projMatrix)).GetLength();
    if (centerDistance < bestDistance) {
        bestDistance = centerDistance;
        best = Handle::Center;
    }

    for (const auto& entry : axes) {
        const double d = distanceToSegmentNdc(ndcPos, frame.origin, frame.origin + entry.axis * scale, ctx.viewMatrix,
                                               ctx.projMatrix);
        if (d < bestDistance) {
            bestDistance = d;
            best = entry.handle;
        }
    }

    struct PlaneEntry {
        PXR_NS::GfVec3d center;
        Handle handle;
    };
    const PlaneEntry planes[] = {{frame.origin + (frame.axisX + frame.axisY) * (scale * 0.3), Handle::PlaneXY},
                                 {frame.origin + (frame.axisY + frame.axisZ) * (scale * 0.3), Handle::PlaneYZ},
                                 {frame.origin + (frame.axisZ + frame.axisX) * (scale * 0.3), Handle::PlaneXZ}};
    for (const auto& entry : planes) {
        const double d = (ndcPos - projectToNdc(entry.center, ctx.viewMatrix, ctx.projMatrix)).GetLength();
        if (d < bestDistance) {
            bestDistance = d;
            best = entry.handle;
        }
    }

    return best;
}

ToolResult MoveTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
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

    std::optional<PXR_NS::GfVec3d> anchor;
    switch (m_activeHandle) {
        case Handle::AxisX:
            anchor = closestPointOnAxis(ray, frame.origin, frame.axisX);
            break;
        case Handle::AxisY:
            anchor = closestPointOnAxis(ray, frame.origin, frame.axisY);
            break;
        case Handle::AxisZ:
            anchor = closestPointOnAxis(ray, frame.origin, frame.axisZ);
            break;
        case Handle::PlaneXY:
            anchor = intersectPlane(ray, frame.origin, frame.axisZ);
            break;
        case Handle::PlaneYZ:
            anchor = intersectPlane(ray, frame.origin, frame.axisX);
            break;
        case Handle::PlaneXZ:
            anchor = intersectPlane(ray, frame.origin, frame.axisY);
            break;
        case Handle::Center:
            anchor = intersectPlane(ray, frame.origin, (frame.origin - ctx.cameraPos).GetNormalized());
            break;
        default:
            break;
    }
    if (!anchor) {
        m_activeHandle = Handle::None;
        return {};
    }
    m_dragAnchor = *anchor;

    ToolResult result;
    result.handled = true;
    return result;
}

ToolResult MoveTool::mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d&, const PXR_NS::GfRay& ray) {
    if (m_activeHandle == Handle::None) {
        return {};
    }
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        return {};
    }

    std::optional<PXR_NS::GfVec3d> newPoint;
    switch (m_activeHandle) {
        case Handle::AxisX:
            newPoint = closestPointOnAxis(ray, m_dragFrame.origin, m_dragFrame.axisX);
            break;
        case Handle::AxisY:
            newPoint = closestPointOnAxis(ray, m_dragFrame.origin, m_dragFrame.axisY);
            break;
        case Handle::AxisZ:
            newPoint = closestPointOnAxis(ray, m_dragFrame.origin, m_dragFrame.axisZ);
            break;
        case Handle::PlaneXY:
            newPoint = intersectPlane(ray, m_dragFrame.origin, m_dragFrame.axisZ);
            break;
        case Handle::PlaneYZ:
            newPoint = intersectPlane(ray, m_dragFrame.origin, m_dragFrame.axisX);
            break;
        case Handle::PlaneXZ:
            newPoint = intersectPlane(ray, m_dragFrame.origin, m_dragFrame.axisY);
            break;
        case Handle::Center:
            newPoint = intersectPlane(ray, m_dragFrame.origin, (m_dragFrame.origin - ctx.cameraPos).GetNormalized());
            break;
        default:
            return {};
    }
    if (!newPoint) {
        ToolResult result;
        result.handled = true;
        return result;
    }

    const PXR_NS::GfVec3d worldDelta = *newPoint - m_dragAnchor;
    if (worldDelta.GetLength() < 1e-9) {
        ToolResult result;
        result.handled = true;
        return result;
    }

    PXR_NS::UsdGeomXformOp translateOp = getOrCreateOp(xformable, PXR_NS::UsdGeomXformOp::TypeTranslate);
    if (!translateOp) {
        return {};
    }

    const PXR_NS::GfMatrix4d worldToParent = parentToWorld(xformable.GetPrim()).GetInverse();
    const PXR_NS::GfVec3d localDelta = worldToParent.TransformDir(worldDelta);

    PXR_NS::GfVec3d currentValue(0.0);
    const bool isFloat = translateOp.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionFloat;
    if (isFloat) {
        PXR_NS::GfVec3f v(0.0f);
        translateOp.Get(&v);
        currentValue = PXR_NS::GfVec3d(v);
    } else {
        translateOp.Get(&currentValue);
    }
    const PXR_NS::GfVec3d newValue = currentValue + localDelta;
    if (isFloat) {
        translateOp.Set(PXR_NS::GfVec3f(newValue));
    } else {
        translateOp.Set(newValue);
    }

    m_dragAnchor = *newPoint;

    ToolResult result;
    result.handled = true;
    return result;
}

void MoveTool::mouseRelease() { m_activeHandle = Handle::None; }

}  // namespace usdcc::tools
