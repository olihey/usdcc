#pragma once

#include "usdcc/tools/tool.h"

#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdGeom/xformable.h>

namespace usdcc::tools {

// Shared machinery for the three gizmo-manipulation tools (MoveTool,
// RotateTool, ScaleTool): resolving the single selected, transformable prim
// a gizmo acts on, computing a screen-constant gizmo scale, projecting world
// points to normalized device coordinates for screen-space hit testing, and
// getting-or-creating the xformOp each tool writes to. Multi-prim gizmo
// editing is out of scope for M5 (see docs/PLAN.md) — with more than one
// prim selected, these tools draw no gizmo and don't handle mouse events.
class TransformGizmoTool : public Tool {
protected:
    enum class Handle { None, AxisX, AxisY, AxisZ, PlaneXY, PlaneYZ, PlaneXZ, Center };

    static constexpr double kGizmoScreenSize = 0.15;
    static constexpr double kPickThresholdNdc = 0.035;

    // The gizmo's origin and its own X/Y/Z axes, expressed in world space —
    // i.e. the selected prim's object/local space, not the world's. Scale is
    // stripped out (each axis is renormalized) so a scaled prim doesn't
    // stretch the gizmo or its drag math; a prim with no ancestor rotation
    // (the common case) just gets the world axes back unchanged.
    struct GizmoFrame {
        PXR_NS::GfVec3d origin;
        PXR_NS::GfVec3d axisX;
        PXR_NS::GfVec3d axisY;
        PXR_NS::GfVec3d axisZ;
    };

    PXR_NS::UsdGeomXformable resolveXformable(const ToolContext& ctx) const;
    GizmoFrame gizmoFrame(const PXR_NS::UsdGeomXformable& xformable) const;
    double gizmoScale(const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& cameraPos) const;

    static PXR_NS::GfVec2d projectToNdc(const PXR_NS::GfVec3d& worldPoint, const PXR_NS::GfMatrix4d& viewMatrix,
                                        const PXR_NS::GfMatrix4d& projMatrix);

    // Screen-space distance from `ndcPos` to the projected segment [a, b].
    static double distanceToSegmentNdc(const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfVec3d& a,
                                        const PXR_NS::GfVec3d& b, const PXR_NS::GfMatrix4d& viewMatrix,
                                        const PXR_NS::GfMatrix4d& projMatrix);

    // Closest point on the world-space axis line through `origin` along
    // `axisDir` to the pick ray — turns an axis-constrained drag into a 3D
    // world-space point.
    static PXR_NS::GfVec3d closestPointOnAxis(const PXR_NS::GfRay& ray, const PXR_NS::GfVec3d& origin,
                                               const PXR_NS::GfVec3d& axisDir);

    // World-space point where the pick ray crosses the plane through
    // `origin` with the given `normal` — used for plane- and free-drag.
    // Empty if the ray is parallel to the plane.
    static std::optional<PXR_NS::GfVec3d> intersectPlane(const PXR_NS::GfRay& ray, const PXR_NS::GfVec3d& origin,
                                                          const PXR_NS::GfVec3d& normal);

    // Reads (get-or-adds) an existing xformOp of the given type on
    // `xformable`, keeping the canonical translate/rotate/scale order when
    // it has to add one to an otherwise-empty or TRS-only op stack. If the
    // stack already contains some other op type (a matrix or orient op, or
    // a per-axis op), the new op is simply appended without reordering —
    // see docs/PLAN.md milestone M5 for this known limitation.
    static PXR_NS::UsdGeomXformOp getOrCreateOp(const PXR_NS::UsdGeomXformable& xformable,
                                                 PXR_NS::UsdGeomXformOp::Type opType);

    Handle m_activeHandle = Handle::None;
};

}  // namespace usdcc::tools
