// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#include "python_object.h"

#include <pybind11/stl.h>

#include "usdcc/scripting/scripted_tool.h"

#include "usdcc/core/log.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/stageCache.h>

namespace usdcc::scripting {

namespace py = pybind11;
using usdcc::tools::GizmoGeometry;
using usdcc::tools::OptionValue;
using usdcc::tools::ToolContext;
using usdcc::tools::ToolOption;
using usdcc::tools::ToolResult;

namespace {
py::object bridge(const char* name) { return py::module_::import("usdcc.tools").attr(name); }

py::tuple matrix(const PXR_NS::GfMatrix4d& m) {
    py::tuple values(16);
    const double* data = m.GetArray();
    for (std::size_t i = 0; i < 16; ++i) values[i] = data[i];
    return values;
}

py::tuple vec3(const PXR_NS::GfVec3d& v) { return py::make_tuple(v[0], v[1], v[2]); }

// What usdcc/tools.py's _context() takes.
py::tuple contextArguments(const ToolContext& ctx) {
    long cacheId = -1;
    if (ctx.stage) {
        const PXR_NS::UsdStageCache::Id id = PXR_NS::UsdUtilsStageCache::Get().GetId(ctx.stage);
        if (id.IsValid()) cacheId = id.ToLongInt();
    }
    std::vector<std::string> selection;
    selection.reserve(ctx.selection.size());
    for (const PXR_NS::SdfPath& path : ctx.selection) selection.push_back(path.GetString());
    py::cpp_function pick([pickPrim = ctx.pickPrim](double x, double y) -> std::string {
        if (!pickPrim) return {};
        return pickPrim(PXR_NS::GfVec2d(x, y)).GetString();
    });
    return py::make_tuple(cacheId, selection, matrix(ctx.viewMatrix), matrix(ctx.projMatrix), vec3(ctx.cameraPos),
                          ctx.targetOpName.GetString(), pick);
}

PXR_NS::GfVec3d toVec3(const py::handle& values, std::size_t first) {
    return PXR_NS::GfVec3d(values[py::int_(first)].cast<double>(), values[py::int_(first + 1)].cast<double>(),
                           values[py::int_(first + 2)].cast<double>());
}

PXR_NS::GfVec4f toColor(const py::handle& values, std::size_t first) {
    return PXR_NS::GfVec4f(values[py::int_(first)].cast<float>(), values[py::int_(first + 1)].cast<float>(),
                           values[py::int_(first + 2)].cast<float>(), values[py::int_(first + 3)].cast<float>());
}
}  // namespace

ScriptedTool::ScriptedTool(std::string label, std::shared_ptr<PythonObject> instance)
    : m_label(std::move(label)), m_instance(std::move(instance)) {}

ScriptedTool::~ScriptedTool() = default;

std::vector<ToolOption> ScriptedTool::options() const {
    std::vector<ToolOption> options;
    if (m_broken) return options;
    py::gil_scoped_acquire gil;
    try {
        for (const py::handle& item : m_instance->object.attr("options")()) {
            const py::dict option = py::reinterpret_borrow<py::dict>(item);
            const std::string type = option["type"].cast<std::string>();
            const std::string id = option["id"].cast<std::string>();
            const std::string label = option["label"].cast<std::string>();
            if (type == "toggle") {
                options.push_back(ToolOption::toggle(id, label, option["value"].cast<bool>()));
            } else if (type == "number") {
                options.push_back(ToolOption::number(id, label, option["value"].cast<double>(),
                                                     option["min"].cast<double>(), option["max"].cast<double>(),
                                                     option["step"].cast<double>(), option["decimals"].cast<int>(),
                                                     option["suffix"].cast<std::string>(),
                                                     option["enabled_by"].cast<std::string>()));
            } else {
                std::vector<ToolOption::Choice> choices;
                for (const py::handle& choice : option["choices"]) {
                    choices.push_back({choice[py::int_(0)].cast<std::string>(), choice[py::int_(1)].cast<std::string>()});
                }
                options.push_back(ToolOption::choice(id, label, option["value"].cast<std::string>(), std::move(choices)));
            }
        }
    } catch (py::error_already_set& error) {
        logPythonError(error);
        m_broken = true;
    } catch (py::cast_error& error) {
        USDCC_LOG_ERROR("python.exception", "The tool {}'s options() returned something unusable: {}", m_label, error.what());
        m_broken = true;
    }
    return options;
}

void ScriptedTool::setOption(const std::string& id, const OptionValue& value) {
    if (m_broken) return;
    py::gil_scoped_acquire gil;
    try {
        py::object pythonValue = std::visit([](const auto& held) -> py::object { return py::cast(held); }, value);
        m_instance->object.attr("set_option")(id, pythonValue);
    } catch (py::error_already_set& error) {
        logPythonError(error);
        m_broken = true;
    }
}

GizmoGeometry ScriptedTool::buildGizmo(const ToolContext& ctx) const {
    GizmoGeometry geometry;
    if (m_broken) return geometry;
    py::gil_scoped_acquire gil;
    try {
        const py::tuple result = bridge("_gizmo")(m_instance->object, contextArguments(ctx));
        // Flat: a, b, colour; a, b, c, colour.
        for (const py::handle& line : result[0]) {
            geometry.lines.push_back({toVec3(line, 0), toVec3(line, 3), toColor(line, 6)});
        }
        for (const py::handle& triangle : result[1]) {
            geometry.triangles.push_back({toVec3(triangle, 0), toVec3(triangle, 3), toVec3(triangle, 6), toColor(triangle, 9)});
        }
    } catch (py::error_already_set& error) {
        logPythonError(error);
        m_broken = true;
    } catch (py::cast_error& error) {
        USDCC_LOG_ERROR("python.exception", "The tool {}'s gizmo has something unusable in it: {}", m_label, error.what());
        m_broken = true;
    }
    return geometry;
}

ToolResult ScriptedTool::mouseEvent(const char* method, const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos,
                                    const PXR_NS::GfRay& ray) {
    ToolResult result;
    if (m_broken) return result;
    py::gil_scoped_acquire gil;
    try {
        const py::tuple answer =
            bridge("_mouse")(m_instance->object, method, contextArguments(ctx), py::make_tuple(ndcPos[0], ndcPos[1]),
                             vec3(ray.GetPoint(0.0)), vec3(ray.GetDirection()));
        result.handled = answer[0].cast<bool>();
        result.startsEdit = answer[1].cast<bool>();
        if (!answer[2].is_none()) {
            std::vector<PXR_NS::SdfPath> selection;
            for (const py::handle& path : answer[2]) selection.emplace_back(path.cast<std::string>());
            result.newSelection = std::move(selection);
        }
    } catch (py::error_already_set& error) {
        logPythonError(error);
        m_broken = true;
    } catch (py::cast_error& error) {
        USDCC_LOG_ERROR("python.exception", "The tool {}'s {}() returned something unusable: {}", m_label, method, error.what());
        m_broken = true;
    }
    return result;
}

ToolResult ScriptedTool::mousePress(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
    return mouseEvent("mouse_press", ctx, ndcPos, ray);
}

ToolResult ScriptedTool::mouseMove(const ToolContext& ctx, const PXR_NS::GfVec2d& ndcPos, const PXR_NS::GfRay& ray) {
    return mouseEvent("mouse_move", ctx, ndcPos, ray);
}

void ScriptedTool::mouseRelease() {
    if (m_broken) return;
    py::gil_scoped_acquire gil;
    try {
        m_instance->object.attr("mouse_release")();
    } catch (py::error_already_set& error) {
        logPythonError(error);
        m_broken = true;
    }
}

}  // namespace usdcc::scripting
