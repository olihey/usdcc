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

// The prim's existing translate op, or an invalid op if it has none (unlike
// getOrCreateOp(), this never authors anything).
PXR_NS::UsdGeomXformOp findTranslateOp(const PXR_NS::UsdGeomXformable& xformable) {
    bool resetsXformStack = false;
    for (const auto& op : xformable.GetOrderedXformOps(&resetsXformStack)) {
        if (op.GetOpType() == PXR_NS::UsdGeomXformOp::TypeTranslate) {
            return op;
        }
    }
    return {};
}

PXR_NS::GfVec3d readTranslate(const PXR_NS::UsdGeomXformable& xformable) {
    const PXR_NS::UsdGeomXformOp op = findTranslateOp(xformable);
    if (!op) {
        return PXR_NS::GfVec3d(0.0);
    }
    if (op.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionFloat) {
        PXR_NS::GfVec3f v(0.0f);
        op.Get(&v);
        return PXR_NS::GfVec3d(v);
    }
    PXR_NS::GfVec3d v(0.0);
    op.Get(&v);
    return v;
}
}  // namespace

std::vector<ToolOption> MoveTool::options() const {
    std::vector<ToolOption> opts = TransformGizmoTool::options();
    opts.insert(opts.begin(), ToolOption::choice("space", "Space", m_space == Space::World ? "world" : "local",
                                                 {{"local", "Local"}, {"world", "World"}}));
    return opts;
}

void MoveTool::setOption(const std::string& id, const OptionValue& value) {
    if (id != "space") {
        TransformGizmoTool::setOption(id, value);
        return;
    }
    if (const auto* space = std::get_if<std::string>(&value)) {
        if (*space == "world") {
            m_space = Space::World;
        } else if (*space == "local") {
            m_space = Space::Local;
        }
    }
}

bool MoveTool::canDrive(PXR_NS::UsdGeomXformOp::Type type) const {
    return type == PXR_NS::UsdGeomXformOp::TypeTranslate || type == PXR_NS::UsdGeomXformOp::TypeTransform;
}

MoveTool::GizmoFrame MoveTool::frameFor(const Target& target, const PXR_NS::UsdGeomXformable& xformable) const {
    if (!target.targeted) {
        return gizmoFrame(xformable);
    }
    GizmoFrame frame = opFrame(xformable, target.index);
    if (m_space == Space::World) {
        frame.axisX = PXR_NS::GfVec3d(1.0, 0.0, 0.0);
        frame.axisY = PXR_NS::GfVec3d(0.0, 1.0, 0.0);
        frame.axisZ = PXR_NS::GfVec3d(0.0, 0.0, 1.0);
    }
    return frame;
}

GizmoGeometry MoveTool::buildGizmo(const ToolContext& ctx) const {
    GizmoGeometry geo;
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        return geo;
    }
    const Target target = resolveTarget(ctx, xformable);
    const GizmoFrame frame = frameFor(target, xformable);
    const double scale = gizmoScale(frame.origin, ctx.cameraPos);
    auto paint = [&](const PXR_NS::GfVec4f& color) { return target.mismatch ? dimmed(color) : color; };

    struct AxisEntry {
        PXR_NS::GfVec3d axis;
        PXR_NS::GfVec4f color;
        Handle handle;
    };
    const AxisEntry axes[] = {{frame.axisX, kColorX, Handle::AxisX},
                              {frame.axisY, kColorY, Handle::AxisY},
                              {frame.axisZ, kColorZ, Handle::AxisZ}};
    for (const auto& entry : axes) {
        const PXR_NS::GfVec4f color = paint((m_activeHandle == entry.handle) ? kColorActive : entry.color);
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
        const PXR_NS::GfVec4f color = paint((m_activeHandle == entry.handle) ? kColorActive : entry.color);
        shapes::appendPlaneQuad(geo, frame.origin, entry.u, entry.v, scale * 0.2, scale * 0.2, color);
    }

    shapes::appendBox(geo, frame.origin, scale * 0.05,
                      paint(m_activeHandle == Handle::Center ? kColorActive : kColorCenter));
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
    if (centerDistance < kCenterPickNdc) {
        return Handle::Center;
    }
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

ToolResult MoveTool::beginGizmoDrag(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        m_activeHandle = Handle::None;
        return {};
    }
    const Target target = resolveTarget(ctx, xformable);
    const GizmoFrame frame = frameFor(target, xformable);
    m_activeHandle = pickHandle(ctx, ndcPos, frame);
    if (m_activeHandle == Handle::None) {
        return {};
    }
    if (target.mismatch) {
        // The target is an op Move can't edit: the dimmed gizmo swallows the
        // press and does nothing (see mousePress()).
        m_activeHandle = Handle::None;
        ToolResult result;
        result.handled = true;
        return result;
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
    m_targetOp = target.targeted ? target.op : PXR_NS::UsdGeomXformOp();
    if (isMatrixOp(m_targetOp)) {
        // A matrix op: Move edits its translation row.
        m_dragStartMatrix = readOpMatrix(m_targetOp);
        m_dragStartLocal = m_dragStartMatrix.ExtractTranslation();
        m_worldToValue = opValueSpace(xformable, target.index).GetInverse();
    } else if (m_targetOp) {
        const std::vector<double> start = readOpNumbers(m_targetOp);
        m_dragStartLocal = start.size() == 3 ? PXR_NS::GfVec3d(start[0], start[1], start[2]) : PXR_NS::GfVec3d(0.0);
        m_worldToValue = opValueSpace(xformable, target.index).GetInverse();
    } else {
        m_dragStartLocal = readTranslate(xformable);
    }

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

    // Total movement since the mouse went down, always measured from the fixed
    // press anchor (not accumulated frame to frame) so snapping can round the
    // whole drag rather than each tiny step.
    PXR_NS::GfVec3d worldDelta = *newPoint - m_dragAnchor;
    if (snapEnabled()) {
        // Round the movement along each of the gizmo's axes. A constrained
        // drag only has a component along its own axis/plane, so the others
        // round to zero.
        worldDelta = m_dragFrame.axisX * snapped(PXR_NS::GfDot(worldDelta, m_dragFrame.axisX)) +
                     m_dragFrame.axisY * snapped(PXR_NS::GfDot(worldDelta, m_dragFrame.axisY)) +
                     m_dragFrame.axisZ * snapped(PXR_NS::GfDot(worldDelta, m_dragFrame.axisZ));
    }

    // A targeted op: its value lives in the space before it in the stack.
    if (m_targetOp) {
        const PXR_NS::GfVec3d value = m_dragStartLocal + m_worldToValue.TransformDir(worldDelta);
        if (isMatrixOp(m_targetOp)) {
            PXR_NS::GfMatrix4d m = m_dragStartMatrix;
            m.SetTranslateOnly(value);  // the 3x3 part (rotation, scale) stays as it was
            writeOpMatrix(m_targetOp, m);
        } else {
            writeOpNumbers(m_targetOp, {value[0], value[1], value[2]});
        }
        ToolResult result;
        result.handled = true;
        return result;
    }

    // Don't author a translate op just because the mouse jittered, or because
    // a snapped drag is still sitting on its starting value.
    PXR_NS::UsdGeomXformOp translateOp = findTranslateOp(xformable);
    if (!translateOp) {
        if (worldDelta.GetLength() < 1e-9) {
            ToolResult result;
            result.handled = true;
            return result;
        }
        translateOp = getOrCreateOp(xformable, PXR_NS::UsdGeomXformOp::TypeTranslate);
        if (!translateOp) {
            return {};
        }
    }

    const PXR_NS::GfMatrix4d worldToParent = parentToWorld(xformable.GetPrim()).GetInverse();
    const PXR_NS::GfVec3d newValue = m_dragStartLocal + worldToParent.TransformDir(worldDelta);
    if (translateOp.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionFloat) {
        translateOp.Set(PXR_NS::GfVec3f(newValue));
    } else {
        translateOp.Set(newValue);
    }

    ToolResult result;
    result.handled = true;
    return result;
}

void MoveTool::mouseRelease() {
    m_activeHandle = Handle::None;
    m_targetOp = PXR_NS::UsdGeomXformOp();
}

}  // namespace usdcc::tools
