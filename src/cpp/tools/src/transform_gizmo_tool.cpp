#include "usdcc/tools/transform_gizmo_tool.h"

#include <pxr/base/gf/line.h>
#include <pxr/base/gf/plane.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <algorithm>

namespace usdcc::tools {

PXR_NS::UsdGeomXformable TransformGizmoTool::resolveXformable(const ToolContext& ctx) const {
    if (!ctx.stage || ctx.selection.size() != 1) {
        return PXR_NS::UsdGeomXformable();
    }
    return PXR_NS::UsdGeomXformable(ctx.stage->GetPrimAtPath(ctx.selection.front()));
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
    return frame;
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
