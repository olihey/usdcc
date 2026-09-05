#pragma once

#include "usdcc/tools/transform_gizmo_tool.h"

namespace usdcc::tools {

// Scale gizmo: three axis handles (per-axis scale) plus a center handle
// (uniform scale, driven by drag distance from the gizmo origin) — see
// docs/PLAN.md milestone M5.
class ScaleTool : public TransformGizmoTool {
public:
    std::string name() const override { return "Scale"; }

    GizmoGeometry buildGizmo(const ToolContext& ctx) const override;
    ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
    ToolResult mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
    void mouseRelease() override;

private:
    Handle pickHandle(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const GizmoFrame& frame) const;

    GizmoFrame m_dragFrame;          // gizmo origin/axes (the prim's object space) at drag start
    double m_dragStartParam = 1.0;  // signed distance along the axis, or from the origin (uniform), at drag start
    PXR_NS::GfVec3f m_dragStartScale{1.0f, 1.0f, 1.0f};
};

}  // namespace usdcc::tools
