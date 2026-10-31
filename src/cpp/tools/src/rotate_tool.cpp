#include "usdcc/tools/rotate_tool.h"

#include "usdcc/tools/gizmo_shapes.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <algorithm>
#include <cmath>
#include <limits>

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

// The prim's existing rotateXYZ op, or an invalid op if it has none (unlike
// getOrCreateOp(), this never authors anything).
PXR_NS::UsdGeomXformOp findRotateOp(const PXR_NS::UsdGeomXformable& xformable) {
    bool resetsXformStack = false;
    for (const auto& op : xformable.GetOrderedXformOps(&resetsXformStack)) {
        if (op.GetOpType() == PXR_NS::UsdGeomXformOp::TypeRotateXYZ) {
            return op;
        }
    }
    return {};
}

PXR_NS::GfVec3d readEuler(const PXR_NS::UsdGeomXformOp& op) {
    if (!op) {
        return PXR_NS::GfVec3d(0.0);
    }
    if (op.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionDouble) {
        PXR_NS::GfVec3d v(0.0);
        op.Get(&v);
        return v;
    }
    PXR_NS::GfVec3f v(0.0f);
    op.Get(&v);
    return PXR_NS::GfVec3d(v);
}

// What a rotateXYZ op with these Euler degrees rotates by. USD composes it as
// Rx * Ry * Rz in its row-vector convention (X is applied first).
PXR_NS::GfMatrix3d eulerToRotation(const PXR_NS::GfVec3d& degrees) {
    return PXR_NS::GfMatrix3d(PXR_NS::GfRotation(PXR_NS::GfVec3d::XAxis(), degrees[0])) *
           PXR_NS::GfMatrix3d(PXR_NS::GfRotation(PXR_NS::GfVec3d::YAxis(), degrees[1])) *
           PXR_NS::GfMatrix3d(PXR_NS::GfRotation(PXR_NS::GfVec3d::ZAxis(), degrees[2]));
}

// `angle` shifted by whole turns to lie within half a turn of `reference`.
double nearestTurn(double angle, double reference) {
    return angle - 360.0 * std::round((angle - reference) / 360.0);
}

// The rotateXYZ Euler degrees that produce `rotation` (the inverse of
// eulerToRotation()). Every orientation has two such triples, plus whole turns
// of each component, so this returns the one closest to `hint` (the previous
// value): without that, a drag could make the numbers jump between equivalent
// triples from one mouse move to the next.
PXR_NS::GfVec3d rotationToEuler(const PXR_NS::GfMatrix3d& m, const PXR_NS::GfVec3d& hint) {
    // With M = Rx(a) * Ry(b) * Rz(c): M[0][2] = -sin(b), M[1][2] = sin(a)cos(b),
    // M[2][2] = cos(a)cos(b), M[0][1] = cos(b)sin(c), M[0][0] = cos(b)cos(c).
    const double sinB = std::clamp(-m[0][2], -1.0, 1.0);
    const double cosB = std::sqrt(std::max(0.0, 1.0 - sinB * sinB));
    double a = 0.0, b = std::asin(sinB), c = 0.0;
    if (cosB > 1e-6) {
        a = std::atan2(m[1][2], m[2][2]);
        c = std::atan2(m[0][1], m[0][0]);
    } else {
        // Gimbal lock (Y at +-90 degrees): only a -/+ c is determined. Keep Z
        // where it was and solve for X.
        c = PXR_NS::GfDegreesToRadians(hint[2]);
        a = sinB > 0.0 ? std::atan2(m[1][0], m[1][1]) + c : std::atan2(-m[1][0], m[1][1]) - c;
    }
    const PXR_NS::GfVec3d first(PXR_NS::GfRadiansToDegrees(a), PXR_NS::GfRadiansToDegrees(b),
                                PXR_NS::GfRadiansToDegrees(c));
    const PXR_NS::GfVec3d second(first[0] + 180.0, 180.0 - first[1], first[2] + 180.0);

    PXR_NS::GfVec3d best = first;
    double bestDistance = std::numeric_limits<double>::max();
    for (PXR_NS::GfVec3d candidate : {first, second}) {
        for (int i = 0; i < 3; ++i) {
            candidate[i] = nearestTurn(candidate[i], hint[i]);
        }
        const double distance = (candidate - hint).GetLengthSq();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
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
    m_lastEuler = readEuler(findRotateOp(xformable));
    m_dragStartRotation = eulerToRotation(m_lastEuler);
    m_dragTotalDegrees = 0.0;

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

    // The angle swept since the mouse went down; snapping rounds this total,
    // not each small step, and the result is applied on top of the value the
    // op had at press.
    m_dragTotalDegrees += deltaDegrees;
    const double appliedDegrees = snapped(m_dragTotalDegrees);

    // Don't author a rotate op until the rotation actually amounts to something
    // (e.g. a snapped drag that hasn't yet reached its first increment).
    PXR_NS::UsdGeomXformOp rotateOp = findRotateOp(xformable);
    if (!rotateOp) {
        if (std::abs(appliedDegrees) < 1e-9) {
            ToolResult result;
            result.handled = true;
            return result;
        }
        rotateOp = getOrCreateOp(xformable, PXR_NS::UsdGeomXformOp::TypeRotateXYZ);
        if (!rotateOp) {
            return {};
        }
    }

    // Spin the orientation the prim had at press about its own axis (the one
    // the ring is drawn on), then express the result as rotateXYZ Euler
    // degrees. Adding the angle to one Euler component instead would only turn
    // the prim about that ring while the other two components are zero.
    const int axisIndex = m_activeHandle == Handle::AxisX ? 0 : (m_activeHandle == Handle::AxisY ? 1 : 2);
    PXR_NS::GfVec3d axisInObjectSpace(0.0);
    axisInObjectSpace[axisIndex] = 1.0;
    const PXR_NS::GfMatrix3d rotated =
        PXR_NS::GfMatrix3d(PXR_NS::GfRotation(axisInObjectSpace, appliedDegrees)) * m_dragStartRotation;
    const PXR_NS::GfVec3d euler = rotationToEuler(rotated, m_lastEuler);
    m_lastEuler = euler;
    if (rotateOp.GetPrecision() == PXR_NS::UsdGeomXformOp::PrecisionDouble) {
        rotateOp.Set(euler);
    } else {
        rotateOp.Set(PXR_NS::GfVec3f(euler));
    }

    ToolResult result;
    result.handled = true;
    return result;
}

void RotateTool::mouseRelease() { m_activeHandle = Handle::None; }

}  // namespace usdcc::tools
