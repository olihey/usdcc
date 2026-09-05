#pragma once

#include "usdcc/tools/tool.h"

namespace usdcc::tools::shapes {

// Small, reusable world-space geometry builders shared by the gizmo tools
// (MoveTool, RotateTool, ScaleTool) — see docs/PLAN.md milestone M5.

// A straight line from `origin` to `origin + axis * length`.
void appendAxisLine(GizmoGeometry& geo, const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& axis, double length,
                     const PXR_NS::GfVec4f& color);

// A small solid cone (as triangles), apex at `apex`, base centered at
// `apex - axis * height` — an arrow tip for the move gizmo's axis handles.
void appendCone(GizmoGeometry& geo, const PXR_NS::GfVec3d& apex, const PXR_NS::GfVec3d& axis, double height,
                 double radius, const PXR_NS::GfVec4f& color);

// A small solid cube centered at `center` — used for the scale gizmo's axis
// tips and uniform-scale/free-move center handle.
void appendBox(GizmoGeometry& geo, const PXR_NS::GfVec3d& center, double halfSize, const PXR_NS::GfVec4f& color);

// A flat quad spanning `[inset, inset + size]` along `uAxis`/`vAxis` from
// `origin` — a plane-constrained move handle.
void appendPlaneQuad(GizmoGeometry& geo, const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& uAxis,
                      const PXR_NS::GfVec3d& vAxis, double inset, double size, const PXR_NS::GfVec4f& color);

// A circular ring (as line segments) centered at `origin`, lying in the
// plane perpendicular to `axis` — a rotate handle.
void appendRing(GizmoGeometry& geo, const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& axis, double radius,
                 const PXR_NS::GfVec4f& color);

}  // namespace usdcc::tools::shapes
