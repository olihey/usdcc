#pragma once

#include "usdcc/tools/tool.h"

namespace usdcc::tools {

// Click-to-select: casts a pick ray on mouse press and replaces the current
// selection with whatever prim it hits (or clears it, on empty space). See
// docs/PLAN.md milestone M5.
class SelectTool : public Tool {
public:
    std::string name() const override { return "Select"; }

    ToolResult mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) override;
};

}  // namespace usdcc::tools
