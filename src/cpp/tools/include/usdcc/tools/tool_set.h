#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool.h"

#include <pxr/usd/usdGeom/xformOp.h>

#include <memory>
#include <string>
#include <vector>

namespace usdcc::tools {

// The ids of the tools the app comes with.
inline constexpr const char* kSelectTool = "select";
inline constexpr const char* kMoveTool = "move";
inline constexpr const char* kRotateTool = "rotate";
inline constexpr const char* kScaleTool = "scale";

// Which transform tool can act on an op type (docs/ATTRIBUTE_EDITOR.md §7.2):
// Move drives translate ops, Rotate any rotate op or orient, Scale scale ops,
// and all three drive a transform (matrix) op — each its own part of it.
// toolIdForOpType() is the tool to switch to (Move for a matrix), Select for an
// op none of them drives. Any other tool drives none.
std::string toolIdForOpType(PXR_NS::UsdGeomXformOp::Type type);
bool toolCanDrive(const std::string& id, PXR_NS::UsdGeomXformOp::Type type);
// Whether it's Move, Rotate or Scale, which act on a target op.
bool isTransformTool(const std::string& id);

// The editing tools and which one is active — the state the main window's tool
// rail and options bar control. The built-in four are registered the same way a
// plugin's are (docs/PLUGINS.md §3.8). One ToolSet is shared by every viewport,
// so picking a tool or changing an option affects all of them at once and a
// viewport opened later simply joins in. Sharing the Tool instances is safe
// because a tool only holds state for an in-progress drag, and only one drag
// can happen at a time (there's one mouse).
//
// Qt-free like the rest of usdcc_tools: listeners (the toolbars, the viewports)
// subscribe through the Signal members below.
class ToolSet {
public:
    struct Entry {
        std::string id;     // stable, e.g. "move"
        std::string label;  // the tool rail's tooltip
        // A theme icon's name, or the path of an image file (see ToolRail).
        // Empty: the rail draws the label's first letter.
        std::string icon;
        std::unique_ptr<Tool> tool;
    };

    ToolSet();

    // In the rail's order.
    const std::vector<Entry>& tools() const { return m_tools; }
    bool hasTool(const std::string& id) const;
    static bool isBuiltIn(const std::string& id);

    // Adds a tool after the others, or replaces the one with this id (re-running
    // the script that registers it picks up changes). False, doing nothing, for
    // a built-in id or an empty one.
    bool addTool(const std::string& id, const std::string& label, const std::string& icon, std::unique_ptr<Tool> tool);
    // False for a built-in or unknown one. Select becomes active if this was.
    bool removeTool(const std::string& id);

    const std::string& activeId() const { return m_active; }
    Tool& activeTool();
    const Tool& activeTool() const;
    // Null for an unknown id.
    Tool* tool(const std::string& id) const;

    // Switches the active tool (a no-op if it already is). Any drag the
    // previous tool had in progress is cancelled first. False for an unknown id.
    bool setActive(const std::string& id);

    // Forwards to the active tool's setOption() and notifies listeners.
    void setActiveOption(const std::string& id, const OptionValue& value);

    // Fired after the active tool changes. The toolbars rebuild what they show.
    usdcc::core::Signal<> activeToolChanged;
    // Fired after an option of the active tool changes (it can alter the
    // gizmo, e.g. Move's Space). Viewports redraw.
    usdcc::core::Signal<> optionsChanged;
    // Fired after a tool is added, replaced or removed. The rail rebuilds.
    usdcc::core::Signal<> toolsChanged;

private:
    std::vector<Entry>::iterator findEntry(const std::string& id);
    std::vector<Entry>::const_iterator findEntry(const std::string& id) const;

    std::vector<Entry> m_tools;
    std::string m_active = kSelectTool;
};

}  // namespace usdcc::tools
