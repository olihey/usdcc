#include "usdcc/tools/tool_set.h"

#include "usdcc/tools/move_tool.h"
#include "usdcc/tools/rotate_tool.h"
#include "usdcc/tools/scale_tool.h"
#include "usdcc/tools/select_tool.h"
#include "usdcc/usd/xform_math.h"

namespace usdcc::tools {

ToolKind toolKindForOpType(PXR_NS::UsdGeomXformOp::Type type) {
    // A matrix op can be edited with any of the three; Move is the default.
    if (type == PXR_NS::UsdGeomXformOp::TypeTranslate || type == PXR_NS::UsdGeomXformOp::TypeTransform) {
        return ToolKind::Move;
    }
    if (type == PXR_NS::UsdGeomXformOp::TypeScale) return ToolKind::Scale;
    if (usdcc::usd::isRotation(type)) return ToolKind::Rotate;
    return ToolKind::Select;
}

bool toolCanDrive(ToolKind kind, PXR_NS::UsdGeomXformOp::Type type) {
    if (kind == ToolKind::Select) return false;
    if (type == PXR_NS::UsdGeomXformOp::TypeTransform) return true;
    return toolKindForOpType(type) == kind;
}

ToolSet::ToolSet() {
    m_tools[static_cast<std::size_t>(ToolKind::Select)] = std::make_unique<SelectTool>();
    m_tools[static_cast<std::size_t>(ToolKind::Move)] = std::make_unique<MoveTool>();
    m_tools[static_cast<std::size_t>(ToolKind::Rotate)] = std::make_unique<RotateTool>();
    m_tools[static_cast<std::size_t>(ToolKind::Scale)] = std::make_unique<ScaleTool>();
}

void ToolSet::setActiveKind(ToolKind kind) {
    if (kind == m_active) {
        return;
    }
    activeTool().mouseRelease();
    m_active = kind;
    activeToolChanged();
}

void ToolSet::setActiveOption(const std::string& id, const OptionValue& value) {
    activeTool().setOption(id, value);
    optionsChanged();
}

}  // namespace usdcc::tools
