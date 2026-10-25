#pragma once

#include "usdcc/tools/transform_gizmo_tool.h"

namespace usdcc::tools {

// Translate gizmo: three axis arrows (X/Y/Z-constrained drag), three plane
// handles between axis pairs (plane-constrained drag), and a center handle
// (free, screen-plane drag) — see docs/PLAN.md section 4's "translate
// gizmo: axis/plane/free" note and milestone M5.
//
// Options: "Space" (gizmo axes follow the prim's own axes, or the world's) on
// top of the shared Snap/Increment (scene units).
class MoveTool : public TransformGizmoTool {
public:
    MoveTool() : TransformGizmoTool({0.5, 0.001, 1000.0, 0.1, 3, ""}) {}

    std::string name() const override { return "Move"; }

    std::vector<ToolOption> options() const override;
    void setOption(const std::string& id, const OptionValue& value) override;

    GizmoGeometry buildGizmo(const ToolContext& ctx) const override;
    ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
    ToolResult mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
    void mouseRelease() override;

private:
    Handle pickHandle(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const GizmoFrame& frame) const;

    PXR_NS::GfVec3d m_dragAnchor{0.0};      // world-space point the drag started at, on the constrained axis/plane
    PXR_NS::GfVec3d m_dragStartLocal{0.0};  // the prim's translate-op value when the drag started
    GizmoFrame m_dragFrame;                 // gizmo origin/axes at drag start
};

}  // namespace usdcc::tools
