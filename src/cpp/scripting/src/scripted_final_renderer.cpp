// Final renderers written in Python (usdcc.render.FinalRenderer, docs/FINAL_RENDER.md §6.1):
// a FinalRenderer that forwards to the Python instance, and the task object Python reports on.
#include "python_object.h"

#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/render/scene_file_renderer.h"

#include <pybind11/stl.h>

#include <pxr/base/gf/vec3f.h>

#include <QDir>
#include <QImage>
#include <QTemporaryDir>

#include <cstring>
#include <memory>
#include <stdexcept>

namespace py = pybind11;

namespace usdcc::scripting {

namespace {

using usdcc::render::FinalRenderer;
using usdcc::render::FloatImage;
using usdcc::render::RenderSettingField;
using usdcc::render::RenderTask;
using usdcc::render::TaskCallbacks;

// One task as Python sees it: what to render, and where to say how it went.
struct TaskState {
    RenderTask task;
    TaskCallbacks callbacks;
    bool ended = false;
    std::shared_ptr<QTemporaryDir> folder;  // export_scene()'s, kept until the task goes
};

class PythonTask {
public:
    explicit PythonTask(std::shared_ptr<TaskState> state) : m(std::move(state)) {}
    const std::shared_ptr<TaskState>& state() const { return m; }

    py::tuple exportScene() {
        if (!m->folder) {
            m->folder = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/usdcc-render-XXXXXX"));
            if (!m->folder->isValid()) throw std::runtime_error("couldn't make a folder for the scene");
        }
        const usdcc::render::SceneExport scene = usdcc::render::exportRenderScene(m->task, m->folder->path().toStdString());
        return py::make_tuple(scene.file, scene.settings.GetString(), scene.product.GetString(), scene.camera.GetString());
    }
    void progress(double fraction, const std::string& state) {
        if (!m->ended && m->callbacks.progress) m->callbacks.progress(fraction, state);
    }
    void finished(const std::map<std::string, std::uintptr_t>& images) {
        if (m->ended) return;
        std::map<std::string, FloatImage> result;
        for (const auto& [name, address] : images) {
            const auto* image = reinterpret_cast<const QImage*>(address);
            if (!image || image->isNull()) continue;
            const QImage linear = image->convertToFormat(QImage::Format_RGBA32FPx4);
            FloatImage out(linear.width(), linear.height());
            for (int y = 0; y < linear.height(); ++y) {
                std::memcpy(out.row(y), linear.constScanLine(y), static_cast<size_t>(linear.width()) * 4 * sizeof(float));
            }
            // A whole frame for a tile: the tile.
            result[name] = usdcc::render::cropToRegion(out, m->task.region);
        }
        m->ended = true;
        if (result.empty()) {
            if (m->callbacks.failed) m->callbacks.failed("The renderer gave no image back");
        } else if (m->callbacks.finished) {
            m->callbacks.finished(std::move(result));
        }
    }
    void failed(const std::string& error) {
        if (m->ended) return;
        m->ended = true;
        if (m->callbacks.failed) m->callbacks.failed(error);
    }

private:
    std::shared_ptr<TaskState> m;
};

py::object pythonValue(const PXR_NS::VtValue& value) {
    if (value.IsHolding<bool>()) return py::bool_(value.UncheckedGet<bool>());
    if (value.IsHolding<int>()) return py::int_(value.UncheckedGet<int>());
    if (value.IsHolding<unsigned int>()) return py::int_(value.UncheckedGet<unsigned int>());
    if (value.IsHolding<float>()) return py::float_(value.UncheckedGet<float>());
    if (value.IsHolding<double>()) return py::float_(value.UncheckedGet<double>());
    if (value.IsHolding<std::string>()) return py::str(value.UncheckedGet<std::string>());
    if (value.IsHolding<PXR_NS::TfToken>()) return py::str(value.UncheckedGet<PXR_NS::TfToken>().GetString());
    if (value.IsHolding<PXR_NS::GfVec3f>()) {
        const PXR_NS::GfVec3f v = value.UncheckedGet<PXR_NS::GfVec3f>();
        return py::make_tuple(v[0], v[1], v[2]);
    }
    return py::none();
}

PXR_NS::VtValue usdValue(const py::handle& value, RenderSettingField::Type type) {
    using Type = RenderSettingField::Type;
    if (value.is_none()) return {};
    switch (type) {
    case Type::Bool: return PXR_NS::VtValue(value.cast<bool>());
    case Type::Int: return PXR_NS::VtValue(value.cast<int>());
    case Type::Float: return PXR_NS::VtValue(value.cast<float>());
    case Type::Color: {
        const auto rgb = value.cast<std::vector<float>>();
        if (rgb.size() != 3) throw py::value_error("a color is three numbers");
        return PXR_NS::VtValue(PXR_NS::GfVec3f(rgb[0], rgb[1], rgb[2]));
    }
    case Type::Choice: return PXR_NS::VtValue(PXR_NS::TfToken(value.cast<std::string>()));
    case Type::String:
    default: return PXR_NS::VtValue(value.cast<std::string>());
    }
}

RenderSettingField::Type fieldType(const std::string& name) {
    using Type = RenderSettingField::Type;
    if (name == "bool") return Type::Bool;
    if (name == "int") return Type::Int;
    if (name == "float") return Type::Float;
    if (name == "choice") return Type::Choice;
    if (name == "color") return Type::Color;
    if (name == "string") return Type::String;
    throw py::value_error("no setting type '" + name + "' (bool, int, float, string, choice or color)");
}

class ScriptedTaskHandle : public usdcc::render::RenderTaskHandle {
public:
    ScriptedTaskHandle(std::shared_ptr<TaskState> state, std::shared_ptr<PythonObject> renderer, std::shared_ptr<PythonObject> task)
        : m_state(std::move(state)), m_renderer(std::move(renderer)), m_task(std::move(task)) {}
    ~ScriptedTaskHandle() override { cancel(); }
    void cancel() override {
        if (!m_state || m_state->ended) return;
        m_state->ended = true;
        if (!Py_IsInitialized()) return;
        py::gil_scoped_acquire gil;
        try {
            m_renderer->object.attr("cancel")(m_task->object);
        } catch (py::error_already_set& error) {
            logPythonError(error);
        }
    }

private:
    std::shared_ptr<TaskState> m_state;
    std::shared_ptr<PythonObject> m_renderer;
    std::shared_ptr<PythonObject> m_task;
};

class ScriptedFinalRenderer : public FinalRenderer {
public:
    explicit ScriptedFinalRenderer(py::object instance) : m_instance(std::make_shared<PythonObject>(std::move(instance))) {
        const py::object& o = m_instance->object;
        m_id = py::str(o.attr("id"));
        m_name = py::str(o.attr("name"));
        for (const py::handle ns : o.attr("namespaces")) m_namespaces.push_back(py::str(ns));
        m_tiles = o.attr("tiles").cast<bool>();
        if (m_id.empty()) throw py::value_error("a final renderer needs an id");
    }

    std::string id() const override { return m_id; }
    std::string name() const override { return m_name; }
    Capabilities capabilities() const override { return {m_tiles, true}; }
    std::vector<std::string> namespaces() const override { return m_namespaces; }

    std::string setupProblem() const override {
        if (!Py_IsInitialized()) return "Python has stopped";
        py::gil_scoped_acquire gil;
        try {
            const py::object problem = m_instance->object.attr("setup_problem")();
            return problem.is_none() ? std::string() : std::string(py::str(problem));
        } catch (py::error_already_set& error) {
            logPythonError(error);
            return "Its setup_problem() raised";
        }
    }

    std::vector<RenderSettingField> settingsSchema() const override {
        std::vector<RenderSettingField> fields;
        if (!Py_IsInitialized()) return fields;
        py::gil_scoped_acquire gil;
        try {
            for (const py::handle item : m_instance->object.attr("settings_schema")()) {
                const py::dict d = py::reinterpret_borrow<py::dict>(item);
                RenderSettingField field;
                field.attribute = py::str(d["attribute"]);
                field.rendererKey = PXR_NS::TfToken(d.contains("key") && !d["key"].is_none() ? std::string(py::str(d["key"])) : field.attribute);
                field.label = d.contains("label") ? std::string(py::str(d["label"])) : field.attribute;
                if (d.contains("group") && !d["group"].is_none()) field.group = py::str(d["group"]);
                if (d.contains("help") && !d["help"].is_none()) field.help = py::str(d["help"]);
                field.type = fieldType(d.contains("type") ? std::string(py::str(d["type"])) : "string");
                if (d.contains("choices") && !d["choices"].is_none()) field.choices = d["choices"].cast<std::vector<std::string>>();
                if (d.contains("minimum") && d.contains("maximum") && !d["minimum"].is_none() && !d["maximum"].is_none()) {
                    field.hasRange = true;
                    field.minimum = d["minimum"].cast<double>();
                    field.maximum = d["maximum"].cast<double>();
                }
                const py::object defaultValue = d.contains("default") ? py::object(d["default"]) : py::object(py::none());
                field.defaultValue = usdValue(defaultValue, field.type);
                fields.push_back(std::move(field));
            }
        } catch (py::error_already_set& error) {
            logPythonError(error);
        } catch (const std::exception& error) {
            USDCC_LOG_ERROR("python.exception", "{}'s settings_schema(): {}", m_name, error.what());
        }
        return fields;
    }

    std::unique_ptr<usdcc::render::RenderTaskHandle> start(const RenderTask& task, TaskCallbacks callbacks) override {
        auto state = std::make_shared<TaskState>();
        state->task = task;
        state->callbacks = std::move(callbacks);
        if (!Py_IsInitialized()) {
            if (state->callbacks.failed) state->callbacks.failed("Python has stopped");
            return nullptr;
        }
        py::gil_scoped_acquire gil;
        std::shared_ptr<PythonObject> taskObject;
        try {
            taskObject = std::make_shared<PythonObject>(py::cast(PythonTask(state)));
            m_instance->object.attr("start")(taskObject->object);
        } catch (py::error_already_set& error) {
            logPythonError(error);
            if (!state->ended) {
                state->ended = true;
                if (state->callbacks.failed) state->callbacks.failed(std::string("The renderer's start() raised: ") + error.what());
            }
            return nullptr;
        }
        return std::make_unique<ScriptedTaskHandle>(state, m_instance, taskObject);
    }

private:
    std::shared_ptr<PythonObject> m_instance;
    std::string m_id;
    std::string m_name;
    std::vector<std::string> m_namespaces;
    bool m_tiles = false;
};

}  // namespace

void bindFinalRenderers(py::module_& m) {
    py::class_<PythonTask>(m, "FinalRenderTask")
        .def_property_readonly("job_id", [](const PythonTask& t) { return t.state()->task.jobId; })
        .def_property_readonly("index", [](const PythonTask& t) { return t.state()->task.index; })
        .def_property_readonly("frame", [](const PythonTask& t) { return t.state()->task.frame; })
        .def_property_readonly("resolution",
                               [](const PythonTask& t) { return py::make_tuple(t.state()->task.resolution[0], t.state()->task.resolution[1]); })
        .def_property_readonly("pixel_aspect", [](const PythonTask& t) { return t.state()->task.pixelAspect; })
        .def_property_readonly("region",
                               [](const PythonTask& t) {
                                   const auto& r = t.state()->task.region;
                                   return py::make_tuple(r.x, r.y, r.width, r.height);
                               })
        .def_property_readonly("camera", [](const PythonTask& t) { return t.state()->task.camera.GetString(); })
        .def_property_readonly("render_settings", [](const PythonTask& t) { return t.state()->task.renderSettings.GetString(); })
        .def_property_readonly("time_limit", [](const PythonTask& t) { return t.state()->task.timeLimit; })
        .def_property_readonly("aovs",
                               [](const PythonTask& t) {
                                   py::list result;
                                   for (const auto& aov : t.state()->task.aovs) {
                                       result.append(py::make_tuple(aov.name, aov.sourceName, aov.dataType.GetString()));
                                   }
                                   return result;
                               })
        .def_property_readonly("settings",
                               [](const PythonTask& t) {
                                   py::dict result;
                                   for (const auto& [key, value] : t.state()->task.settings) result[py::str(key.GetString())] = pythonValue(value);
                                   return result;
                               })
        .def_property_readonly("active", [](const PythonTask& t) { return !t.state()->ended; })
        .def("export_scene", &PythonTask::exportScene)
        .def("progress", &PythonTask::progress, py::arg("fraction"), py::arg("state"))
        .def("finished", &PythonTask::finished, py::arg("images"))
        .def("failed", &PythonTask::failed, py::arg("error"));

    m.def(
        "register_final_renderer",
        [](py::object instance) {
            auto renderer = std::make_shared<ScriptedFinalRenderer>(std::move(instance));
            const std::string id = renderer->id();
            usdcc::render::FinalRenderers::instance().add(std::move(renderer));
            return id;
        },
        py::arg("renderer"));
    m.def(
        "unregister_final_renderer", [](const std::string& id) { usdcc::render::FinalRenderers::instance().remove(id); },
        py::arg("id"));
}

// What's left when Python goes: renderers from Python can't run any more.
void removeScriptedFinalRenderers() {
    std::vector<std::string> ids;
    for (const auto& renderer : usdcc::render::FinalRenderers::instance().all()) {
        if (std::dynamic_pointer_cast<ScriptedFinalRenderer>(renderer)) ids.push_back(renderer->id());
    }
    for (const std::string& id : ids) usdcc::render::FinalRenderers::instance().remove(id);
}

}  // namespace usdcc::scripting
