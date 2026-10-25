#include "usdcc/tools/select_tool.h"

#include <algorithm>

namespace usdcc::tools {

namespace {
constexpr const char* kModeId = "mode";

const char* modeName(SelectTool::Mode mode) {
    switch (mode) {
        case SelectTool::Mode::New:
            return "new";
        case SelectTool::Mode::Add:
            return "add";
        case SelectTool::Mode::Subtract:
            return "subtract";
        case SelectTool::Mode::Invert:
            return "invert";
    }
    return "new";
}
}  // namespace

std::vector<ToolOption> SelectTool::options() const {
    return {ToolOption::choice(kModeId, "Mode", modeName(m_mode),
                               {{"new", "New"}, {"add", "Add"}, {"subtract", "Subtract"}, {"invert", "Invert"}})};
}

void SelectTool::setOption(const std::string& id, const OptionValue& value) {
    if (id != kModeId) {
        return;
    }
    const auto* chosen = std::get_if<std::string>(&value);
    if (!chosen) {
        return;
    }
    for (const Mode mode : {Mode::New, Mode::Add, Mode::Subtract, Mode::Invert}) {
        if (*chosen == modeName(mode)) {
            m_mode = mode;
            return;
        }
    }
}

ToolResult SelectTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay&) {
    ToolResult result;
    result.handled = true;

    const PXR_NS::SdfPath hit = ctx.pickPrim ? ctx.pickPrim(ndcPos) : PXR_NS::SdfPath();
    std::vector<PXR_NS::SdfPath> selection = ctx.selection;
    const auto existing = std::find(selection.begin(), selection.end(), hit);
    const bool alreadySelected = existing != selection.end();

    switch (m_mode) {
        case Mode::New:
            selection.clear();
            if (!hit.IsEmpty()) {
                selection.push_back(hit);
            }
            break;
        case Mode::Add:
            if (!hit.IsEmpty() && !alreadySelected) {
                selection.push_back(hit);
            }
            break;
        case Mode::Subtract:
            if (alreadySelected) {
                selection.erase(existing);
            }
            break;
        case Mode::Invert:
            if (!hit.IsEmpty()) {
                if (alreadySelected) {
                    selection.erase(existing);
                } else {
                    selection.push_back(hit);
                }
            }
            break;
    }

    result.newSelection = std::move(selection);
    return result;
}

}  // namespace usdcc::tools
