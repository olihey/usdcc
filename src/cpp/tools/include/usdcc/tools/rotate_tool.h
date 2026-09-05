#pragma once

#include "usdcc/tools/transform_gizmo_tool.h"

namespace usdcc::tools {

// Rotate gizmo: three axis rings (X/Y/Z), drawn along the selected prim's
// own object-space axes (see TransformGizmoTool::gizmoFrame()) and each
// dragged tangentially to spin the prim about that axis — see
// docs/PLAN.md milestone M5.
//
// Known limitation: each ring drives the corresponding component of the
// prim's own rotateXYZ Euler op directly (ring X adds to the op's X degree
// value, etc.), which is exact for a single-axis rotation (the common case,
// and what M5's own verification exercises) but only an approximation once
// the prim already has a compound rotation across more than one axis, since
// Euler components aren't independent once more than one is non-zero. Doing
// this exactly would mean composing/decomposing a GfRotation through the
// op's existing value on every drag step — left as a follow-up rather than
// blocking M5 on it.
class RotateTool : public TransformGizmoTool {
public:
    std::string name() const override { return "Rotate"; }

    GizmoGeometry buildGizmo(const ToolContext& ctx) const override;
    ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
    ToolResult mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
    void mouseRelease() override;

private:
    Handle pickRing(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const GizmoFrame& frame) const;
    static PXR_NS::GfVec3d axisForHandle(Handle handle, const GizmoFrame& frame);

    GizmoFrame m_dragFrame;                        // gizmo origin/axes (the prim's object space) at drag start
    PXR_NS::GfVec3d m_lastVector{1.0, 0.0, 0.0};  // last frame's origin-to-hit vector, projected onto the ring plane
};

}  // namespace usdcc::tools
