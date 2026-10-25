#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/tools/tool.h"

#include <array>
#include <memory>
#include <string>

namespace usdcc::tools {

enum class ToolKind { Select, Move, Rotate, Scale };

// The editing tools (Select/Move/Rotate/Scale) and which one is active — the
// state the main window's tool rail and options bar control. One ToolSet is
// shared by every viewport, so picking a tool or changing an option affects
// all of them at once and a viewport opened later simply joins in. Sharing the
// Tool instances is safe because a tool only holds state for an in-progress
// drag, and only one drag can happen at a time (there's one mouse).
//
// Qt-free like the rest of usdcc_tools: listeners (the toolbars, the viewports)
// subscribe through the two usdcc::core::Signal members below.
class ToolSet {
public:
    ToolSet();

    ToolKind activeKind() const { return m_active; }
    Tool& tool(ToolKind kind) { return *m_tools[static_cast<std::size_t>(kind)]; }
    const Tool& tool(ToolKind kind) const { return *m_tools[static_cast<std::size_t>(kind)]; }
    Tool& activeTool() { return tool(m_active); }
    const Tool& activeTool() const { return tool(m_active); }

    // Switches the active tool (a no-op if it already is). Any drag the
    // previous tool had in progress is cancelled first.
    void setActiveKind(ToolKind kind);

    // Forwards to the active tool's setOption() and notifies listeners.
    void setActiveOption(const std::string& id, const OptionValue& value);

    // Fired after the active tool changes. The toolbars rebuild what they show.
    usdcc::core::Signal<> activeToolChanged;
    // Fired after an option of the active tool changes (it can alter the
    // gizmo, e.g. Move's Space). Viewports redraw.
    usdcc::core::Signal<> optionsChanged;

private:
    std::array<std::unique_ptr<Tool>, 4> m_tools;
    ToolKind m_active = ToolKind::Select;
};

}  // namespace usdcc::tools
