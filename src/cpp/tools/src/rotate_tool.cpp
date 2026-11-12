#include "usdcc/tools/rotate_tool.h"

#include "usdcc/tools/gizmo_shapes.h"
#include "usdcc/usd/xform_math.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
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
// getOrCreateOp(), this never authors anything). Auto mode's op.
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
}  // namespace

bool RotateTool::canDrive(PXR_NS::UsdGeomXformOp::Type type) const {
    return usdcc::usd::isRotation(type) || type == PXR_NS::UsdGeomXformOp::TypeTransform;
}

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
    const Target target = resolveTarget(ctx, xformable);
    const GizmoFrame frame = target.targeted ? opFrame(xformable, target.index) : gizmoFrame(xformable);
    const int onlyAxis = target.targeted ? usdcc::usd::singleRotationAxis(target.op.GetOpType()) : -1;
    const double scale = gizmoScale(frame.origin, ctx.cameraPos);

    struct RingEntry {
        PXR_NS::GfVec3d axis;
        PXR_NS::GfVec4f color;
        Handle handle;
    };
    const RingEntry rings[] = {{frame.axisX, kColorX, Handle::AxisX},
                               {frame.axisY, kColorY, Handle::AxisY},
                               {frame.axisZ, kColorZ, Handle::AxisZ}};
    for (int i = 0; i < 3; ++i) {
        if (onlyAxis >= 0 && i != onlyAxis) {
            continue;  // a rotateX/Y/Z op turns about one axis only
        }
        const RingEntry& entry = rings[i];
        PXR_NS::GfVec4f color = (m_activeHandle == entry.handle) ? kColorActive : entry.color;
        if (target.mismatch) {
            color = dimmed(color);
        }
        shapes::appendRing(geo, frame.origin, entry.axis, scale, color);
    }
    return geo;
}

RotateTool::Handle RotateTool::pickRing(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                         const GizmoFrame& frame, int onlyAxis) const {
    const double scale = gizmoScale(frame.origin, ctx.cameraPos);

    struct RingEntry {
        PXR_NS::GfVec3d axis;
        Handle handle;
    };
    const RingEntry rings[] = {
        {frame.axisX, Handle::AxisX}, {frame.axisY, Handle::AxisY}, {frame.axisZ, Handle::AxisZ}};

    Handle best = Handle::None;
    double bestDistance = kPickThresholdNdc;

    for (int i = 0; i < 3; ++i) {
        if (onlyAxis >= 0 && i != onlyAxis) {
            continue;
        }
        GizmoGeometry ringGeo;
        shapes::appendRing(ringGeo, frame.origin, rings[i].axis, scale, PXR_NS::GfVec4f(0.0f));
        for (const auto& line : ringGeo.lines) {
            const double d = distanceToSegmentNdc(ndcPos, line.a, line.b, ctx.viewMatrix, ctx.projMatrix);
            if (d < bestDistance) {
                bestDistance = d;
                best = rings[i].handle;
            }
        }
    }
    return best;
}

ToolResult RotateTool::beginGizmoDrag(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                      const PXR_NS::GfRay& ray) {
    const PXR_NS::UsdGeomXformable xformable = resolveXformable(ctx);
    if (!xformable) {
        m_activeHandle = Handle::None;
        return {};
    }
    const Target target = resolveTarget(ctx, xformable);
    const GizmoFrame frame = target.targeted ? opFrame(xformable, target.index) : gizmoFrame(xformable);
    const int onlyAxis = target.targeted ? usdcc::usd::singleRotationAxis(target.op.GetOpType()) : -1;
    m_activeHandle = pickRing(ctx, ndcPos, frame, onlyAxis);
    if (m_activeHandle == Handle::None) {
        return {};
    }
    if (target.mismatch) {
        // The target is an op Rotate can't edit: the dimmed gizmo swallows the
        // press and does nothing (see mousePress()).
        m_activeHandle = Handle::None;
        ToolResult result;
        result.handled = true;
        return result;
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
    m_dragTotalDegrees = 0.0;
    m_readout = std::string("Rotate ") + axisLetter(m_activeHandle) + "  0\xC2\xB0";

    m_targetOp = target.targeted ? target.op : PXR_NS::UsdGeomXformOp();
    if (m_targetOp) {
        // Whatever kind of rotation it is, its matrix is the starting point.
        m_dragStartRotation =
            m_targetOp.GetOpTransform(PXR_NS::UsdTimeCode::Default()).ExtractRotationMatrix();
        m_dragStartMatrix = readOpMatrix(m_targetOp);  // used if it's a matrix op
        const std::vector<double> value = readOpNumbers(m_targetOp);
        if (value.size() == 3) {
            m_lastEuler = PXR_NS::GfVec3d(value[0], value[1], value[2]);
        } else if (value.size() == 1) {
            m_startAngle = value[0];
        } else if (value.size() == 4) {
            m_lastQuat = PXR_NS::GfVec4d(value[0], value[1], value[2], value[3]);
        }
    } else {
        m_lastEuler = readEuler(findRotateOp(xformable));
        m_dragStartRotation = usdcc::usd::eulerToMatrix(m_lastEuler, PXR_NS::UsdGeomXformOp::TypeRotateXYZ);
    }

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
    m_readout = std::string("Rotate ") + axisLetter(m_activeHandle) + "  " + formatReadoutNumber(appliedDegrees, 1) + "\xC2\xB0";

    // Spin the orientation the op had at press about the ring's axis (the
    // op's own axis — the frame follows its rotation), then express the
    // result in the op's own terms. Adding the angle to one Euler component
    // instead would only turn about that ring while the other two are zero.
    const int axisIndex = m_activeHandle == Handle::AxisX ? 0 : (m_activeHandle == Handle::AxisY ? 1 : 2);
    PXR_NS::GfVec3d axisInObjectSpace(0.0);
    axisInObjectSpace[axisIndex] = 1.0;
    const PXR_NS::GfMatrix3d rotated =
        PXR_NS::GfMatrix3d(PXR_NS::GfRotation(axisInObjectSpace, appliedDegrees)) * m_dragStartRotation;

    if (m_targetOp) {
        const PXR_NS::UsdGeomXformOp::Type type = m_targetOp.GetOpType();
        if (type == PXR_NS::UsdGeomXformOp::TypeTransform) {
            // A matrix op: turn its 3x3 part about the ring's axis as drawn
            // (row `axisIndex` of the matrix, in the op's value space), applied
            // after the matrix's own scale — A' = A * Rot. That turns the
            // object rigidly about the axis the user sees and never adds shear,
            // even with non-uniform scale. The translation row stays.
            const PXR_NS::GfMatrix3d a = m_dragStartMatrix.ExtractRotationMatrix();
            PXR_NS::GfVec3d axisInValueSpace(a[axisIndex][0], a[axisIndex][1], a[axisIndex][2]);
            if (axisInValueSpace.GetLength() > 1e-12) {
                axisInValueSpace.Normalize();
                const PXR_NS::GfMatrix3d turned =
                    a * PXR_NS::GfMatrix3d(PXR_NS::GfRotation(axisInValueSpace, appliedDegrees));
                PXR_NS::GfMatrix4d m = m_dragStartMatrix;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        m[r][c] = turned[r][c];
                    }
                }
                writeOpMatrix(m_targetOp, m);
            }
        } else if (usdcc::usd::singleRotationAxis(type) >= 0) {
            writeOpNumbers(m_targetOp, {m_startAngle + appliedDegrees});
        } else if (type == PXR_NS::UsdGeomXformOp::TypeOrient) {
            const PXR_NS::GfQuatd q = PXR_NS::GfMatrix4d(rotated, PXR_NS::GfVec3d(0.0)).ExtractRotation().GetQuat();
            PXR_NS::GfVec4d value(q.GetReal(), q.GetImaginary()[0], q.GetImaginary()[1], q.GetImaginary()[2]);
            // q and -q are the same rotation: keep the sign continuous.
            if (PXR_NS::GfDot(value, m_lastQuat) < 0.0) {
                value = -value;
            }
            m_lastQuat = value;
            writeOpNumbers(m_targetOp, {value[0], value[1], value[2], value[3]});
        } else {
            m_lastEuler = usdcc::usd::matrixToEuler(rotated, type, m_lastEuler);
            writeOpNumbers(m_targetOp, {m_lastEuler[0], m_lastEuler[1], m_lastEuler[2]});
        }
        ToolResult result;
        result.handled = true;
        return result;
    }

    // Auto mode. Don't author a rotate op until the rotation actually amounts
    // to something (e.g. a snapped drag that hasn't yet reached its first
    // increment).
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
    const PXR_NS::GfVec3d euler =
        usdcc::usd::matrixToEuler(rotated, PXR_NS::UsdGeomXformOp::TypeRotateXYZ, m_lastEuler);
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

void RotateTool::mouseRelease() {
    m_activeHandle = Handle::None;
    m_readout.clear();
    m_targetOp = PXR_NS::UsdGeomXformOp();
}

}  // namespace usdcc::tools
