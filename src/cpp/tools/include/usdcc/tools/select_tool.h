#pragma once

#include "usdcc/tools/tool.h"

namespace usdcc::tools {

// Click-to-select: casts a pick ray on mouse press and combines the prim it
// hits with the current selection according to its "Mode" option (see
// docs/PLAN.md milestone M5):
//   New       replace the selection with the hit prim (clears it on empty space)
//   Add       add the hit prim to the selection
//   Subtract  remove the hit prim from the selection
//   Invert    toggle the hit prim's membership in the selection
// A click on empty space only changes the selection in New mode; the other
// modes leave it alone, so a slightly-missed click doesn't throw it away.
class SelectTool : public Tool {
public:
    enum class Mode { New, Add, Subtract, Invert };

    std::string name() const override { return "Select"; }

    std::vector<ToolOption> options() const override;
    void setOption(const std::string& id, const OptionValue& value) override;

    ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;

    Mode mode() const { return m_mode; }

private:
    Mode m_mode = Mode::New;
};

}  // namespace usdcc::tools
