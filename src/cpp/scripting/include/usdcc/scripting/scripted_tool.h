#pragma once

#include "usdcc/tools/tool.h"

#include <memory>
#include <string>

namespace usdcc::scripting {

struct PythonObject;

// A viewport tool made in Python: a usdcc.tools.Tool subclass registered with
// usdcc.tools.register_tool() (docs/PLUGINS.md §3.8). Forwards to the Python
// instance through bridge functions in src/python/usdcc/tools.py, which turn
// plain numbers and strings into Gf and Sdf values; Pixar's and pybind11's
// bindings can't share those types directly.
//
// The gizmo is asked for on every frame, so an exception is logged once and the
// tool then does nothing until it's registered again.
class ScriptedTool : public usdcc::tools::Tool {
public:
    ScriptedTool(std::string label, std::shared_ptr<PythonObject> instance);
    ~ScriptedTool() override;

    std::string name() const override { return m_label; }
    std::vector<usdcc::tools::ToolOption> options() const override;
    void setOption(const std::string& id, const usdcc::tools::OptionValue& value) override;
    usdcc::tools::GizmoGeometry buildGizmo(const usdcc::tools::ToolContext& ctx) const override;
    usdcc::tools::ToolResult mousePress(const usdcc::tools::ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                        const PXR_NS::GfRay& ray) override;
    usdcc::tools::ToolResult mouseMove(const usdcc::tools::ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                       const PXR_NS::GfRay& ray) override;
    void mouseRelease() override;

private:
    usdcc::tools::ToolResult mouseEvent(const char* method, const usdcc::tools::ToolContext& ctx,
                                        const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray);

    std::string m_label;
    std::shared_ptr<PythonObject> m_instance;
    mutable bool m_broken = false;
};

}  // namespace usdcc::scripting
