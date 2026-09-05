#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/ray.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::tools {

// World-space gizmo geometry a Tool wants drawn. Kept free of any GL/Qt
// types so usdcc_tools stays plain C++/USD, reusable from a headless context
// and, eventually, from Python (see docs/PLAN.md milestone M9) — the
// viewport, which owns the GL context, turns this into actual draw calls.
struct GizmoGeometry {
    struct Line {
        PXR_NS::GfVec3d a;
        PXR_NS::GfVec3d b;
        PXR_NS::GfVec4f color;
    };
    struct Triangle {
        PXR_NS::GfVec3d a;
        PXR_NS::GfVec3d b;
        PXR_NS::GfVec3d c;
        PXR_NS::GfVec4f color;
    };
    std::vector<Line> lines;
    std::vector<Triangle> triangles;
};

// Everything a Tool needs to draw its gizmo and interpret a click/drag,
// supplied fresh by the viewport on every call rather than cached by the
// tool itself — the viewport remains the source of truth for camera state
// and the current selection (see docs/PLAN.md milestone M4).
struct ToolContext {
    PXR_NS::UsdStageRefPtr stage;
    std::vector<PXR_NS::SdfPath> selection;
    PXR_NS::GfMatrix4d viewMatrix;
    PXR_NS::GfMatrix4d projMatrix;
    PXR_NS::GfVec3d cameraPos;

    // Casts a pick ray through the given normalized device coordinates
    // ([-1,1] on both axes) and returns the closest prim's path, or an empty
    // path if nothing was hit. Backed by
    // UsdImagingGLEngine::TestIntersection in HydraViewportWindow.
    std::function<PXR_NS::SdfPath(const PXR_NS::GfVec2d& ndcPos)> pickPrim;
};

// Outcome of a mouse event handled by a Tool.
struct ToolResult {
    // If true, the viewport suppresses its own camera-orbit/pan handling for
    // this event (see HydraViewportWindow::mousePressEvent).
    bool handled = false;
    // Set to replace the current selection (SelectTool only).
    std::optional<std::vector<PXR_NS::SdfPath>> newSelection;
};

// Base class for viewport-driven editing tools (see docs/PLAN.md section 4
// and milestone M5): SelectTool, MoveTool, RotateTool, ScaleTool. A Tool
// holds only whatever state describes an in-progress drag, reset once
// mouseRelease() is called.
class Tool {
public:
    virtual ~Tool() = default;

    virtual std::string name() const = 0;

    // World-space geometry to draw for the current selection; empty if this
    // tool has no visible gizmo (SelectTool) or nothing is selected.
    virtual GizmoGeometry buildGizmo(const ToolContext& ctx) const {
        (void)ctx;
        return {};
    }

    virtual ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
        (void)ctx;
        (void)ndcPos;
        (void)ray;
        return {};
    }
    virtual ToolResult mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
        (void)ctx;
        (void)ndcPos;
        (void)ray;
        return {};
    }
    virtual void mouseRelease() {}
};

}  // namespace usdcc::tools
