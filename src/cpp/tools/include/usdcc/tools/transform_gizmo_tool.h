#pragma once

#include "usdcc/tools/select_tool.h"

#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdGeom/xformable.h>

namespace usdcc::tools {

// Shared machinery for the three gizmo-manipulation tools (MoveTool,
// RotateTool, ScaleTool): resolving the single selected, transformable prim
// a gizmo acts on, computing a screen-constant gizmo scale, projecting world
// points to normalized device coordinates for screen-space hit testing, and
// getting-or-creating the xformOp each tool writes to. Multi-prim gizmo
// editing is out of scope for M5 (see docs/PLAN.md) — with more than one
// prim selected, these tools draw no gizmo and every press is a selection click.
//
// Also owns the options all three share: "Snap" (on/off) and its "Increment".
// Snapping is relative — the drag's total change since the mouse went down is
// rounded to a multiple of the increment — and its unit (scene units, degrees,
// scale factor) is each tool's own, described by SnapSpec.
//
// These tools are SelectTools too, so objects can be picked while one is
// active: a press on a gizmo handle starts that tool's drag (beginGizmoDrag());
// a press anywhere else selects like SelectTool does, including its "Mode"
// option, which comes after the gizmo's own options.
class TransformGizmoTool : public SelectTool {
public:
    std::vector<ToolOption> options() const override;
    void setOption(const std::string& id, const OptionValue& value) override;

    ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) final;

    bool snapEnabled() const { return m_snapEnabled; }
    double snapIncrement() const { return m_snapIncrement; }

protected:
    // How a tool presents and defaults its snap increment.
    struct SnapSpec {
        double increment;  // default increment
        double min;
        double max;
        double step;       // spin-box arrow step
        int decimals;
        const char* suffix;
    };
    explicit TransformGizmoTool(const SnapSpec& snapSpec)
        : m_snapSpec(snapSpec), m_snapIncrement(snapSpec.increment) {}

    // `value` rounded to the nearest multiple of the snap increment, or
    // unchanged when snapping is off.
    double snapped(double value) const;

    // Starts a drag if the press landed on one of this tool's gizmo handles
    // (sets m_activeHandle and returns handled=true). Otherwise leaves
    // m_activeHandle at None and returns an unhandled result, which makes
    // mousePress() fall back to selecting.
    virtual ToolResult beginGizmoDrag(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                      const PXR_NS::GfRay& ray) = 0;

    // Which axes the gizmo (and its drag math) uses: the prim's own, or the
    // world's. Only MoveTool exposes this; Rotate/Scale write to the prim's
    // local rotate/scale ops, so they must stay on its own axes.
    enum class Space { Local, World };
    Space m_space = Space::Local;

    enum class Handle { None, AxisX, AxisY, AxisZ, PlaneXY, PlaneYZ, PlaneXZ, Center };

    static constexpr double kGizmoScreenSize = 0.15;
    static constexpr double kPickThresholdNdc = 0.035;
    // Within this distance of the gizmo's origin the center handle wins outright.
    // The axes all start at the origin, so without this a press a pixel or two
    // off the center box would be nearer to an axis line and grab that instead.
    static constexpr double kCenterPickNdc = 0.02;

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

    // Targeting one specific op (ToolContext::targetOpName, see
    // docs/ATTRIBUTE_EDITOR.md §7). With no target — or one naming an op that
    // isn't in the stack — a tool runs in Auto mode, exactly as before: the
    // first op of its type, the prim's own frame.
    struct Target {
        PXR_NS::UsdGeomXformOp op;  // the targeted op (valid only when `targeted`)
        int index = -1;             // its position in xformOpOrder
        bool targeted = false;      // a target this tool can drive
        bool mismatch = false;      // a target this tool can't drive: dim the gizmo, edit nothing
    };
    // Whether this tool can edit ops of `type` (Move: translate, …).
    virtual bool canDrive(PXR_NS::UsdGeomXformOp::Type type) const = 0;
    Target resolveTarget(const ToolContext& ctx, const PXR_NS::UsdGeomXformable& xformable) const;

    // The frame a targeted op is edited in: with the stack split around op k as
    // Inner · Op_k · Outer · ParentToWorld (row vectors), the gizmo sits in
    // Op_k · Outer · ParentToWorld — its origin is where the op acts (a pivot
    // in Outer moves it to the pivot) and its axes follow the op's own
    // rotation. Scale is stripped from the axes, as in gizmoFrame().
    GizmoFrame opFrame(const PXR_NS::UsdGeomXformable& xformable, int opIndex) const;
    // Outer · ParentToWorld: the space an op's own value is expressed in
    // (a translate op's value moves the prim along these axes).
    static PXR_NS::GfMatrix4d opValueSpace(const PXR_NS::UsdGeomXformable& xformable, int opIndex);

    // An op's value as plain numbers / written back from them, whatever its
    // precision (half, float or double).
    static std::vector<double> readOpNumbers(const PXR_NS::UsdGeomXformOp& op);
    static bool writeOpNumbers(const PXR_NS::UsdGeomXformOp& op, const std::vector<double>& numbers);

    // A gizmo color greyed out (a target this tool can't drive).
    static PXR_NS::GfVec4f dimmed(const PXR_NS::GfVec4f& color);

    // Targeted transform (matrix) ops: each tool edits its own part of the
    // matrix — Move the translation row, Rotate and Scale the 3x3 part — and
    // leaves the rest alone. The matrix is read at press and every move
    // writes a variation of it.
    static bool isMatrixOp(const PXR_NS::UsdGeomXformOp& op);
    static PXR_NS::GfMatrix4d readOpMatrix(const PXR_NS::UsdGeomXformOp& op);
    static bool writeOpMatrix(const PXR_NS::UsdGeomXformOp& op, const PXR_NS::GfMatrix4d& matrix);
    PXR_NS::GfMatrix4d m_dragStartMatrix{1.0};
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

private:
    SnapSpec m_snapSpec;
    bool m_snapEnabled = false;
    double m_snapIncrement;
};

}  // namespace usdcc::tools
