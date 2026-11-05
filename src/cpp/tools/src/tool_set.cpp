#include "usdcc/tools/tool_set.h"

#include "usdcc/tools/move_tool.h"
#include "usdcc/tools/rotate_tool.h"
#include "usdcc/tools/scale_tool.h"
#include "usdcc/tools/select_tool.h"
#include "usdcc/usd/xform_math.h"

#include <algorithm>

namespace usdcc::tools {

std::string toolIdForOpType(PXR_NS::UsdGeomXformOp::Type type) {
    // A matrix op can be edited with any of the three; Move is the default.
    if (type == PXR_NS::UsdGeomXformOp::TypeTranslate || type == PXR_NS::UsdGeomXformOp::TypeTransform) {
        return kMoveTool;
    }
    if (type == PXR_NS::UsdGeomXformOp::TypeScale) return kScaleTool;
    if (usdcc::usd::isRotation(type)) return kRotateTool;
    return kSelectTool;
}

bool isTransformTool(const std::string& id) { return id == kMoveTool || id == kRotateTool || id == kScaleTool; }

bool toolCanDrive(const std::string& id, PXR_NS::UsdGeomXformOp::Type type) {
    if (!isTransformTool(id)) return false;
    if (type == PXR_NS::UsdGeomXformOp::TypeTransform) return true;
    return toolIdForOpType(type) == id;
}

ToolSet::ToolSet() {
    m_tools.push_back({kSelectTool, "Select", "tool-select", std::make_unique<SelectTool>()});
    m_tools.push_back({kMoveTool, "Move", "tool-move", std::make_unique<MoveTool>()});
    m_tools.push_back({kRotateTool, "Rotate", "tool-rotate", std::make_unique<RotateTool>()});
    m_tools.push_back({kScaleTool, "Scale", "tool-scale", std::make_unique<ScaleTool>()});
}

std::vector<ToolSet::Entry>::iterator ToolSet::findEntry(const std::string& id) {
    return std::find_if(m_tools.begin(), m_tools.end(), [&id](const Entry& entry) { return entry.id == id; });
}

std::vector<ToolSet::Entry>::const_iterator ToolSet::findEntry(const std::string& id) const {
    return std::find_if(m_tools.begin(), m_tools.end(), [&id](const Entry& entry) { return entry.id == id; });
}

bool ToolSet::hasTool(const std::string& id) const { return findEntry(id) != m_tools.end(); }

bool ToolSet::isBuiltIn(const std::string& id) { return id == kSelectTool || isTransformTool(id); }

bool ToolSet::addTool(const std::string& id, const std::string& label, const std::string& icon,
                      std::unique_ptr<Tool> tool) {
    if (id.empty() || !tool || isBuiltIn(id)) return false;
    const auto existing = findEntry(id);
    if (existing != m_tools.end()) {
        if (m_active == id) existing->tool->mouseRelease();
        existing->label = label;
        existing->icon = icon;
        existing->tool = std::move(tool);
    } else {
        m_tools.push_back({id, label, icon, std::move(tool)});
    }
    toolsChanged();
    if (m_active == id) activeToolChanged();
    return true;
}

bool ToolSet::removeTool(const std::string& id) {
    if (isBuiltIn(id)) return false;
    const auto existing = findEntry(id);
    if (existing == m_tools.end()) return false;
    if (m_active == id) setActive(kSelectTool);
    m_tools.erase(findEntry(id));
    toolsChanged();
    return true;
}

Tool& ToolSet::activeTool() { return *findEntry(m_active)->tool; }

const Tool& ToolSet::activeTool() const { return *findEntry(m_active)->tool; }

Tool* ToolSet::tool(const std::string& id) const {
    const auto existing = findEntry(id);
    return existing == m_tools.end() ? nullptr : existing->tool.get();
}

bool ToolSet::setActive(const std::string& id) {
    if (id == m_active) return true;
    if (!hasTool(id)) return false;
    activeTool().mouseRelease();
    m_active = id;
    activeToolChanged();
    return true;
}

void ToolSet::setActiveOption(const std::string& id, const OptionValue& value) {
    activeTool().setOption(id, value);
    optionsChanged();
}

}  // namespace usdcc::tools
