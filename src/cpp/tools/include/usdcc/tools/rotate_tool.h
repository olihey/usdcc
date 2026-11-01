#pragma once

#include "usdcc/tools/transform_gizmo_tool.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/vec4d.h>

namespace usdcc::tools {

// Rotate gizmo: three axis rings (X/Y/Z), drawn along the selected prim's
// own object-space axes (see TransformGizmoTool::gizmoFrame()) and each
// dragged tangentially to spin the prim about that axis — see
// docs/PLAN.md milestone M5.
//
// A drag spins the prim about the grabbed ring's axis exactly, whatever
// rotation it already has: the orientation at press is rotated about that
// object-space axis by the angle swept so far, then converted back to the
// rotateXYZ op's Euler degrees (choosing, of the equivalent triples, the one
// nearest the previous value so the numbers don't jump). Adding the angle to
// one Euler component instead is only right while the other two are zero.
//
// Options: the shared Snap/Increment, in degrees.
class RotateTool : public TransformGizmoTool {
public:
    // "\xC2\xB0" is the UTF-8 degree sign, spelled out so the source file's
    // encoding can't affect it.
    RotateTool() : TransformGizmoTool({15.0, 0.1, 180.0, 5.0, 1, "\xC2\xB0"}) {}

    std::string name() const override { return "Rotate"; }

    GizmoGeometry buildGizmo(const ToolContext& ctx) const override;
    ToolResult mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
    void mouseRelease() override;

protected:
    ToolResult beginGizmoDrag(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                              const PXR_NS::GfRay& ray) override;
    // Any rotate op (the six Euler orders, rotateX/Y/Z) and orient.
    bool canDrive(PXR_NS::UsdGeomXformOp::Type type) const override;

private:
    // `onlyAxis` (0-2) restricts picking to one ring, for a targeted rotateX/Y/Z op.
    Handle pickRing(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const GizmoFrame& frame,
                    int onlyAxis = -1) const;
    static PXR_NS::GfVec3d axisForHandle(Handle handle, const GizmoFrame& frame);

    // Set while dragging a targeted op (Auto mode leaves it invalid).
    PXR_NS::UsdGeomXformOp m_targetOp;
    double m_startAngle = 0.0;  // a single-axis op's value at press
    PXR_NS::GfVec4d m_lastQuat{1.0, 0.0, 0.0, 0.0};  // an orient op's last value (real, i, j, k)

    GizmoFrame m_dragFrame;                        // gizmo origin/axes (the prim's object space) at drag start
    PXR_NS::GfVec3d m_lastVector{1.0, 0.0, 0.0};  // last frame's origin-to-hit vector, projected onto the ring plane

    // The drag is measured from where it started rather than applied step by
    // step, so snapping can round the whole rotation: the prim's rotation at
    // press, turned by the total angle swept since.
    PXR_NS::GfMatrix3d m_dragStartRotation{1.0};
    PXR_NS::GfVec3d m_lastEuler{0.0};  // the Euler degrees last written (or at press); keeps them continuous
    double m_dragTotalDegrees = 0.0;
};

}  // namespace usdcc::tools
