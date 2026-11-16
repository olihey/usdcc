#include "usdcc/tools/select_tool.h"

#include <algorithm>

namespace usdcc::tools {

ToolResult SelectTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay&) {
    ToolResult result;
    result.handled = true;

    const PXR_NS::SdfPath hit = ctx.pickPrim ? ctx.pickPrim(ndcPos) : PXR_NS::SdfPath();
    std::vector<PXR_NS::SdfPath> selection = ctx.selection;
    const auto existing = std::find(selection.begin(), selection.end(), hit);
    const bool alreadySelected = existing != selection.end();

    if (!ctx.shift && !ctx.control) {
        // Plain click: replace.
        selection.clear();
        if (!hit.IsEmpty()) {
            selection.push_back(hit);
        }
    } else if (hit.IsEmpty()) {
        // A modified click on empty space changes nothing.
    } else if (ctx.shift && ctx.control) {
        // Toggle.
        if (alreadySelected) {
            selection.erase(existing);
        } else {
            selection.push_back(hit);
        }
    } else if (ctx.shift) {
        // Add.
        if (!alreadySelected) {
            selection.push_back(hit);
        }
    } else if (alreadySelected) {
        // Ctrl: remove.
        selection.erase(existing);
    }

    result.newSelection = std::move(selection);
    return result;
}

}  // namespace usdcc::tools
