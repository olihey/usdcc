#pragma once

#include "usdcc/tools/tool.h"

namespace usdcc::tools {

// Click-to-select: casts a pick ray on mouse press and combines the prim it
// hits with the current selection according to the modifier keys held
// (ToolContext::shift / ToolContext::control):
//   click             replace the selection with the hit prim (clears it on empty space)
//   Shift+click       add the hit prim to the selection
//   Ctrl+click        remove the hit prim from the selection
//   Ctrl+Shift+click  toggle the hit prim's membership in the selection
// A modified click on empty space leaves the selection alone, so a
// slightly-missed Shift- or Ctrl-click doesn't throw it away.
class SelectTool : public Tool {
public:
    std::string name() const override { return "Select"; }

    ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
};

}  // namespace usdcc::tools
