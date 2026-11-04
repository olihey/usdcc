#include "usdcc/usd/stage.h"
#include "usdcc/tools/transform_gizmo_tool.h"

#include "usdcc/usd/value_type_info.h"
#include "usdcc/usd/xform_math.h"

#include <pxr/base/gf/line.h>
#include <pxr/base/gf/plane.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <algorithm>
#include <cmath>

namespace usdcc::tools {

namespace {
constexpr const char* kSnapId = "snap";
constexpr const char* kSnapIncrementId = "snapIncrement";
}  // namespace

std::vector<ToolOption> TransformGizmoTool::options() const {
    std::vector<ToolOption> opts = {
        ToolOption::toggle(kSnapId, "Snap", m_snapEnabled),
        ToolOption::number(kSnapIncrementId, "Increment", m_snapIncrement, m_snapSpec.min, m_snapSpec.max,
                           m_snapSpec.step, m_snapSpec.decimals, m_snapSpec.suffix, kSnapId)};
    for (ToolOption& selectOption : SelectTool::options()) {
        opts.push_back(std::move(selectOption));
    }
    return opts;
}

void TransformGizmoTool::setOption(const std::string& id, const OptionValue& value) {
    if (id == kSnapId) {
        if (const auto* on = std::get_if<bool>(&value)) {
            m_snapEnabled = *on;
        }
    } else if (id == kSnapIncrementId) {
        if (const auto* increment = std::get_if<double>(&value)) {
            m_snapIncrement = std::clamp(*increment, m_snapSpec.min, m_snapSpec.max);
        }
    } else {
        SelectTool::setOption(id, value);
    }
}

ToolResult TransformGizmoTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                          const PXR_NS::GfRay& ray) {
    ToolResult result = beginGizmoDrag(ctx, ndcPos, ray);
    if (result.handled) {
        // A press on a dimmed gizmo (a target this tool can't drive) is
        // swallowed without grabbing a handle: no drag, so no edit.
        result.startsEdit = m_activeHandle != Handle::None;
        return result;
    }
    m_activeHandle = Handle::None;
    return SelectTool::mousePress(ctx, ndcPos, ray);
}

double TransformGizmoTool::snapped(double value) const {
    if (!m_snapEnabled || m_snapIncrement <= 0.0) {
        return value;
    }
    return std::round(value / m_snapIncrement) * m_snapIncrement;
}

PXR_NS::UsdGeomXformable TransformGizmoTool::resolveXformable(const ToolContext& ctx) const {
    if (!ctx.stage || ctx.selection.size() != 1) {
        return PXR_NS::UsdGeomXformable();
    }
    const PXR_NS::UsdPrim prim = ctx.stage->GetPrimAtPath(ctx.selection.front());
    // No gizmo for a prim edits can't reach: outside the variant being edited.
    return usdcc::usd::editTargetReaches(prim) ? PXR_NS::UsdGeomXformable(prim) : PXR_NS::UsdGeomXformable();
}

TransformGizmoTool::GizmoFrame TransformGizmoTool::gizmoFrame(const PXR_NS::UsdGeomXformable& xformable) const {
    GizmoFrame frame;
    if (!xformable) {
        frame.origin = PXR_NS::GfVec3d(0.0);
        frame.axisX = PXR_NS::GfVec3d(1.0, 0.0, 0.0);
        frame.axisY = PXR_NS::GfVec3d(0.0, 1.0, 0.0);
        frame.axisZ = PXR_NS::GfVec3d(0.0, 0.0, 1.0);
        return frame;
    }
    const PXR_NS::GfMatrix4d worldXform = xformable.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode::Default());
    frame.origin = worldXform.ExtractTranslation();
    // TransformDir() on a unit basis vector picks out that row of the
    // matrix — i.e. the world-space direction of the prim's own local axis
    // — and ignores translation, exactly what an axis *direction* needs.
    frame.axisX = worldXform.TransformDir(PXR_NS::GfVec3d(1.0, 0.0, 0.0)).GetNormalized();
    frame.axisY = worldXform.TransformDir(PXR_NS::GfVec3d(0.0, 1.0, 0.0)).GetNormalized();
    frame.axisZ = worldXform.TransformDir(PXR_NS::GfVec3d(0.0, 0.0, 1.0)).GetNormalized();
    if (m_space == Space::World) {
        frame.axisX = PXR_NS::GfVec3d(1.0, 0.0, 0.0);
        frame.axisY = PXR_NS::GfVec3d(0.0, 1.0, 0.0);
        frame.axisZ = PXR_NS::GfVec3d(0.0, 0.0, 1.0);
    }
    return frame;
}

TransformGizmoTool::Target TransformGizmoTool::resolveTarget(const ToolContext& ctx,
                                                             const PXR_NS::UsdGeomXformable& xformable) const {
    Target target;
    if (ctx.targetOpName.IsEmpty() || !xformable) {
        return target;
    }
    bool resets = false;
    const std::vector<PXR_NS::UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);
    for (size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].IsInverseOp() || ops[i].GetOpName() != ctx.targetOpName) {
            continue;
        }
        if (canDrive(ops[i].GetOpType())) {
            target.op = ops[i];
            target.index = static_cast<int>(i);
            target.targeted = true;
        } else {
            target.mismatch = true;
        }
        break;
    }
    return target;
}

PXR_NS::GfMatrix4d TransformGizmoTool::opValueSpace(const PXR_NS::UsdGeomXformable& xformable, int opIndex) {
    bool resets = false;
    const std::vector<PXR_NS::UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);
    const PXR_NS::GfMatrix4d parent =
        resets ? PXR_NS::GfMatrix4d(1.0) : xformable.ComputeParentToWorldTransform(PXR_NS::UsdTimeCode::Default());
    return usdcc::usd::composeOps(ops, 0, static_cast<size_t>(std::max(opIndex, 0))) * parent;
}

TransformGizmoTool::GizmoFrame TransformGizmoTool::opFrame(const PXR_NS::UsdGeomXformable& xformable,
                                                            int opIndex) const {
    bool resets = false;
    const std::vector<PXR_NS::UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);
    if (opIndex < 0 || opIndex >= static_cast<int>(ops.size())) {
        return gizmoFrame(xformable);
    }
    const PXR_NS::GfMatrix4d m =
        ops[static_cast<size_t>(opIndex)].GetOpTransform(PXR_NS::UsdTimeCode::Default()) * opValueSpace(xformable, opIndex);
    GizmoFrame frame;
    frame.origin = m.ExtractTranslation();
    frame.axisX = m.TransformDir(PXR_NS::GfVec3d(1.0, 0.0, 0.0)).GetNormalized();
    frame.axisY = m.TransformDir(PXR_NS::GfVec3d(0.0, 1.0, 0.0)).GetNormalized();
    frame.axisZ = m.TransformDir(PXR_NS::GfVec3d(0.0, 0.0, 1.0)).GetNormalized();
    return frame;
}

std::vector<double> TransformGizmoTool::readOpNumbers(const PXR_NS::UsdGeomXformOp& op) {
    PXR_NS::VtValue value;
    if (!op || !op.GetAttr().Get(&value, PXR_NS::UsdTimeCode::Default())) {
        return {};
    }
    return usdcc::usd::numericComponents(value);
}

bool TransformGizmoTool::writeOpNumbers(const PXR_NS::UsdGeomXformOp& op, const std::vector<double>& numbers) {
    const PXR_NS::VtValue value = usdcc::usd::makeNumericValue(op.GetAttr().GetTypeName(), numbers);
    return !value.IsEmpty() && op.GetAttr().Set(value, PXR_NS::UsdTimeCode::Default());
}

bool TransformGizmoTool::isMatrixOp(const PXR_NS::UsdGeomXformOp& op) {
    return op && op.GetOpType() == PXR_NS::UsdGeomXformOp::TypeTransform;
}

PXR_NS::GfMatrix4d TransformGizmoTool::readOpMatrix(const PXR_NS::UsdGeomXformOp& op) {
    PXR_NS::GfMatrix4d m(1.0);
    if (op) op.Get(&m, PXR_NS::UsdTimeCode::Default());  // transform ops are always double precision
    return m;
}

bool TransformGizmoTool::writeOpMatrix(const PXR_NS::UsdGeomXformOp& op, const PXR_NS::GfMatrix4d& matrix) {
    return op && op.Set(matrix, PXR_NS::UsdTimeCode::Default());
}

PXR_NS::GfVec4f TransformGizmoTool::dimmed(const PXR_NS::GfVec4f& color) {
    const float grey = (color[0] + color[1] + color[2]) / 3.0f * 0.5f + 0.15f;
    return PXR_NS::GfVec4f(grey, grey, grey, color[3]);
}

double TransformGizmoTool::gizmoScale(const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& cameraPos) const {
    return std::max(0.01, (cameraPos - origin).GetLength() * kGizmoScreenSize);
}

PXR_NS::GfVec2d TransformGizmoTool::projectToNdc(const PXR_NS::GfVec3d& worldPoint, const PXR_NS::GfMatrix4d& viewMatrix,
                                                  const PXR_NS::GfMatrix4d& projMatrix) {
    const PXR_NS::GfMatrix4d viewProj = viewMatrix * projMatrix;
    const PXR_NS::GfVec3d ndc = viewProj.Transform(worldPoint);
    return PXR_NS::GfVec2d(ndc[0], ndc[1]);
}

double TransformGizmoTool::distanceToSegmentNdc(const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfVec3d& a,
                                                 const PXR_NS::GfVec3d& b, const PXR_NS::GfMatrix4d& viewMatrix,
                                                 const PXR_NS::GfMatrix4d& projMatrix) {
    const PXR_NS::GfVec2d pa = projectToNdc(a, viewMatrix, projMatrix);
    const PXR_NS::GfVec2d pb = projectToNdc(b, viewMatrix, projMatrix);
    const PXR_NS::GfVec2d ab = pb - pa;
    const double lenSq = PXR_NS::GfDot(ab, ab);
    double t = lenSq > 1e-12 ? PXR_NS::GfDot(ndcPos - pa, ab) / lenSq : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const PXR_NS::GfVec2d closest = pa + ab * t;
    return (ndcPos - closest).GetLength();
}

PXR_NS::GfVec3d TransformGizmoTool::closestPointOnAxis(const PXR_NS::GfRay& ray, const PXR_NS::GfVec3d& origin,
                                                        const PXR_NS::GfVec3d& axisDir) {
    const PXR_NS::GfLine axisLine(origin, axisDir);
    PXR_NS::GfVec3d pointOnAxis = origin;
    PXR_NS::GfFindClosestPoints(ray, axisLine, nullptr, &pointOnAxis);
    return pointOnAxis;
}

std::optional<PXR_NS::GfVec3d> TransformGizmoTool::intersectPlane(const PXR_NS::GfRay& ray,
                                                                    const PXR_NS::GfVec3d& origin,
                                                                    const PXR_NS::GfVec3d& normal) {
    const PXR_NS::GfPlane plane(normal, origin);
    double distance = 0.0;
    if (!ray.Intersect(plane, &distance)) {
        return std::nullopt;
    }
    return ray.GetPoint(distance);
}

PXR_NS::UsdGeomXformOp TransformGizmoTool::getOrCreateOp(const PXR_NS::UsdGeomXformable& xformable,
                                                          PXR_NS::UsdGeomXformOp::Type opType) {
    bool resetsXformStack = false;
    std::vector<PXR_NS::UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resetsXformStack);
    for (const auto& op : ops) {
        if (op.GetOpType() == opType) {
            return op;
        }
    }

    PXR_NS::UsdGeomXformOp newOp;
    switch (opType) {
        case PXR_NS::UsdGeomXformOp::TypeTranslate:
            newOp = xformable.AddTranslateOp();
            break;
        case PXR_NS::UsdGeomXformOp::TypeRotateXYZ:
            newOp = xformable.AddRotateXYZOp();
            break;
        case PXR_NS::UsdGeomXformOp::TypeScale:
            newOp = xformable.AddScaleOp();
            break;
        default:
            return {};
    }

    // AddXOp() appends to the end of the op order; re-sort into the
    // conventional translate/rotate/scale order, but only when every op on
    // the stack is one of those three known types (see this method's header
    // comment for why a stack with something else on it is left alone).
    ops = xformable.GetOrderedXformOps(&resetsXformStack);
    bool onlyKnownOps = true;
    PXR_NS::UsdGeomXformOp translateOp, rotateOp, scaleOp;
    for (const auto& op : ops) {
        switch (op.GetOpType()) {
            case PXR_NS::UsdGeomXformOp::TypeTranslate:
                translateOp = op;
                break;
            case PXR_NS::UsdGeomXformOp::TypeRotateXYZ:
                rotateOp = op;
                break;
            case PXR_NS::UsdGeomXformOp::TypeScale:
                scaleOp = op;
                break;
            default:
                onlyKnownOps = false;
                break;
        }
    }
    if (onlyKnownOps) {
        std::vector<PXR_NS::UsdGeomXformOp> canonicalOrder;
        if (translateOp) {
            canonicalOrder.push_back(translateOp);
        }
        if (rotateOp) {
            canonicalOrder.push_back(rotateOp);
        }
        if (scaleOp) {
            canonicalOrder.push_back(scaleOp);
        }
        xformable.SetXformOpOrder(canonicalOrder, resetsXformStack);
    }

    return newOp;
}

}  // namespace usdcc::tools
