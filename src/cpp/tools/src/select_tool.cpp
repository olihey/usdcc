#include "usdcc/tools/select_tool.h"

namespace usdcc::tools {

ToolResult SelectTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay&) {
    ToolResult result;
    result.handled = true;
    const PXR_NS::SdfPath hit = ctx.pickPrim ? ctx.pickPrim(ndcPos) : PXR_NS::SdfPath();
    result.newSelection = hit.IsEmpty() ? std::vector<PXR_NS::SdfPath>{} : std::vector<PXR_NS::SdfPath>{hit};
    return result;
}

}  // namespace usdcc::tools
