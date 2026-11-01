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
#include <variant>
#include <vector>

namespace usdcc::tools {

// The value of one tool option: a bool (Toggle), a double (Number), or the id
// of the selected entry (Choice).
using OptionValue = std::variant<bool, double, std::string>;

// A user-adjustable setting a Tool exposes (e.g. "Snap", "Space"). Pure data —
// the tool describes what it offers and the UI (the viewport's options bar)
// decides how to show it, so tools stay free of any Qt types, like the rest of
// usdcc_tools. Build instances with the choice()/toggle()/number() helpers.
struct ToolOption {
    enum class Type {
        Choice,  // pick one of `choices` — shown as a segmented control
        Toggle,  // on/off — a checkbox
        Number,  // a double within [min, max] — a spin box
    };
    struct Choice {
        std::string id;
        std::string label;
    };

    Type type = Type::Toggle;
    std::string id;     // stable key handed back to Tool::setOption()
    std::string label;  // shown beside the control; may be empty
    OptionValue value;  // current value; for Choice, the selected choice's id

    std::vector<Choice> choices;  // Choice only

    // Number only.
    double min = 0.0;
    double max = 1.0;
    double step = 0.1;
    int decimals = 2;
    std::string suffix;

    // Id of a Toggle option that must be on for this one to be usable; empty
    // means always usable. The UI greys this option out otherwise.
    std::string enabledBy;

    static ToolOption choice(std::string id, std::string label, std::string selectedId,
                             std::vector<Choice> choices) {
        ToolOption o;
        o.type = Type::Choice;
        o.id = std::move(id);
        o.label = std::move(label);
        o.value = std::move(selectedId);
        o.choices = std::move(choices);
        return o;
    }
    static ToolOption toggle(std::string id, std::string label, bool on) {
        ToolOption o;
        o.type = Type::Toggle;
        o.id = std::move(id);
        o.label = std::move(label);
        o.value = on;
        return o;
    }
    static ToolOption number(std::string id, std::string label, double value, double min, double max, double step,
                             int decimals, std::string suffix = {}, std::string enabledBy = {}) {
        ToolOption o;
        o.type = Type::Number;
        o.id = std::move(id);
        o.label = std::move(label);
        o.value = value;
        o.min = min;
        o.max = max;
        o.step = step;
        o.decimals = decimals;
        o.suffix = std::move(suffix);
        o.enabledBy = std::move(enabledBy);
        return o;
    }
};

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
    // Set to replace the current selection (SelectTool and its subclasses).
    std::optional<std::vector<PXR_NS::SdfPath>> newSelection;
    // Set by a mousePress() that started a drag which will edit the stage (a
    // gizmo handle was grabbed). The viewport then opens an undo session that
    // lasts until mouseRelease(), so the whole drag becomes one undo step. A
    // press that sets this must not have edited anything itself yet.
    bool startsEdit = false;
};

// Base class for viewport-driven editing tools (see docs/PLAN.md section 4
// and milestone M5): SelectTool, MoveTool, RotateTool, ScaleTool. A Tool
// holds only whatever state describes an in-progress drag, reset once
// mouseRelease() is called.
class Tool {
public:
    virtual ~Tool() = default;

    virtual std::string name() const = 0;

    // The settings this tool exposes, with their current values, in display
    // order. Empty if it has none. The viewport's options bar rebuilds itself
    // from this whenever the active tool changes.
    virtual std::vector<ToolOption> options() const { return {}; }

    // Applies a new value for the option with the given id. An unknown id, or
    // a value of the wrong type for that option, is ignored.
    virtual void setOption(const std::string& id, const OptionValue& value) {
        (void)id;
        (void)value;
    }

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
