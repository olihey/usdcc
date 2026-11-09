// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#include "python_object.h"

#include "usdcc/scripting/scripted_render_view_panel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <pybind11/stl.h>

#include <QHBoxLayout>
#include <QImage>
#include <QLayoutItem>
#include <QMenu>
#include <QPointer>
#include <QWidget>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::scripting {

namespace py = pybind11;
using usdcc::usd::RenderCamera;
using usdcc::usd::StageRefPtr;

namespace {
// The bridge functions in src/python/usdcc/ui.py.
py::object bridge(const char* name) { return py::module_::import("usdcc.ui").attr(name); }

long cacheIdOf(const StageRefPtr& stage) { return stage ? stage->cacheId() : -1; }

std::vector<std::string> pathStrings(const std::vector<PXR_NS::SdfPath>& paths) {
    std::vector<std::string> strings;
    strings.reserve(paths.size());
    for (const PXR_NS::SdfPath& path : paths) strings.push_back(path.GetString());
    return strings;
}

// A RenderCamera as plain values, which ui._render_camera() makes a
// usdcc.ui.RenderCamera of: (prim path or "", camera-to-world as 16 doubles row
// by row, fov_x, fov_y, aspect, near, far, orthographic, ortho_width).
py::tuple cameraValues(const RenderCamera& camera) {
    const std::vector<double> matrix(camera.cameraToWorld.data(), camera.cameraToWorld.data() + 16);
    return py::make_tuple(camera.prim.GetString(), matrix, camera.fovX, camera.fovY, camera.aspect, camera.nearClip,
                          camera.farClip, camera.orthographic, camera.orthoWidth);
}

// What a Python renderer holds of its panel: calls into it, which raise once it
// is gone.
struct RenderViewPanelHandle {
    QPointer<ScriptedRenderViewPanel> panel;

    ScriptedRenderViewPanel& get() const {
        if (!panel) throw std::runtime_error("this render view panel has been closed");
        return *panel;
    }
};
}  // namespace

// The handle's Python class, in _usdcc_app (called from app_module.cpp).
void bindRenderViewPanelHandle(py::module_& m) {
    py::class_<RenderViewPanelHandle>(m, "RenderViewPanelHandle")
        .def("alive", [](const RenderViewPanelHandle& self) { return !self.panel.isNull(); })
        .def(
            "show_image",
            [](const RenderViewPanelHandle& self, std::uintptr_t address) {
                // A PySide QImage, by address (shiboken6.getCppPointer): PySide is built
                // against usdcc's own Qt. Copied, so Python can let go of it.
                self.get().showImage(*reinterpret_cast<const QImage*>(address));
            },
            py::arg("address"))
        .def(
            "image_size",
            [](const RenderViewPanelHandle& self) -> py::object {
                const QImage& image = self.get().image();
                if (image.isNull()) return py::none();
                return py::make_tuple(image.width(), image.height());
            })
        .def(
            "set_state", [](const RenderViewPanelHandle& self, const std::string& state) {
                self.get().setState(QString::fromStdString(state));
            },
            py::arg("state"))
        .def(
            "show_message", [](const RenderViewPanelHandle& self, const std::string& message) {
                self.get().showMessage(QString::fromStdString(message));
            },
            py::arg("message"))
        .def(
            "report_error",
            [](const RenderViewPanelHandle& self, const std::string& message, bool fatal) {
                self.get().reportError(QString::fromStdString(message), fatal);
            },
            py::arg("message"), py::arg("fatal"))
        .def(
            "set_rendering", [](const RenderViewPanelHandle& self, bool on) { self.get().setRendering(on); },
            py::arg("on"))
        .def("is_rendering", [](const RenderViewPanelHandle& self) { return self.get().isRendering(); })
        .def(
            "set_edit_delay", [](const RenderViewPanelHandle& self, int ms) { self.get().setEditDelay(ms); },
            py::arg("ms"))
        .def("edit_delay", [](const RenderViewPanelHandle& self) { return self.get().editDelay(); })
        .def("updates_on_edits", [](const RenderViewPanelHandle& self) { return self.get().updatesOnEdits(); })
        .def("camera", [](const RenderViewPanelHandle& self) { return cameraValues(self.get().currentCamera()); })
        .def("render_size", [](const RenderViewPanelHandle& self) {
            const QSize size = self.get().renderSize();
            return py::make_tuple(size.width(), size.height());
        })
        .def("content_address", [](const RenderViewPanelHandle& self) {
            return reinterpret_cast<std::uintptr_t>(self.get().contentWidget());
        });
}

ScriptedRenderViewPanel::ScriptedRenderViewPanel(usdcc::usd::StageManager* stageManager, StageRefPtr stage,
                                                 const QString& title, std::shared_ptr<PythonObject> pythonClass,
                                                 QWidget* parent)
    : RenderViewPanel(stageManager, std::move(stage), title, parent), m_class(std::move(pythonClass)) {
    createInstance();
    tellStage(currentStage());
    finishSetup();
}

ScriptedRenderViewPanel::~ScriptedRenderViewPanel() {
    closePanel();  // close(), while this is still whole
    py::gil_scoped_acquire gil;
    m_instance.reset();
}

void ScriptedRenderViewPanel::createInstance() {
    if (!m_class) return;
    py::gil_scoped_acquire gil;
    try {
        py::object instance = bridge("_create_render_view_panel")(m_class->object, RenderViewPanelHandle{this},
                                                                   reinterpret_cast<std::uintptr_t>(extraToolbarLayout()));
        m_instance = std::make_shared<PythonObject>(instance);
        const py::object name = instance.attr("renderer_name");
        setRendererName(name.is_none() ? QString() : QString::fromStdString(py::str(name)));
        const py::object category = instance.attr("settings_category");
        setSettingsCategory(category.is_none() ? QString() : QString::fromStdString(py::str(category)));
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

void ScriptedRenderViewPanel::tellStage(const StageRefPtr& stage) {
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        bridge("_view_panel_stage_changed")(m_instance->object, cacheIdOf(stage));
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

void ScriptedRenderViewPanel::closeInstance() {
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        // close(), then Python's wrappers of this panel's widgets are invalidated.
        bridge("_close_render_view_panel")(m_instance->object);
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

void ScriptedRenderViewPanel::rebuild(std::shared_ptr<PythonObject> pythonClass) {
    setRendering(false);
    closeInstance();
    {
        py::gil_scoped_acquire gil;
        m_instance.reset();
        m_class = std::move(pythonClass);
    }
    // The old renderer's own controls.
    while (QLayoutItem* item = extraToolbarLayout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    createInstance();
    tellStage(currentStage());
    finishSetup();
}

usdcc::ui::ViewPanel* ScriptedRenderViewPanel::duplicate(QWidget* parent) const {
    return new ScriptedRenderViewPanel(stageManager(), currentStage(), windowTitle(), m_class, parent);
}

void ScriptedRenderViewPanel::populateOptionsMenu(QMenu* menu) { RenderViewPanel::populateOptionsMenu(menu); }

void ScriptedRenderViewPanel::populateRendererOptions(QMenu* menu) {
    // The Python class's populate_options_menu(): between the shared choices and Settings….
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        bridge("_view_panel_options")(m_instance->object, reinterpret_cast<std::uintptr_t>(menu));
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

std::optional<QString> ScriptedRenderViewPanel::setupProblem() {
    if (!m_instance) {
        return m_class ? tr("This panel's Python code failed; the Log panel has the details.")
                       : tr("Nothing provides this panel now. If its plugin is failing, fixing it and reloading brings it back.");
    }
    py::gil_scoped_acquire gil;
    try {
        const py::object problem = m_instance->object.attr("setup_problem")();
        if (!problem.is_none()) return QString::fromStdString(py::str(problem));
    } catch (py::error_already_set& error) {
        logPythonError(error);
        return tr("This panel's Python code failed; the Log panel has the details.");
    }
    return std::nullopt;
}

namespace {
template <typename... Args>
void callHook(const std::shared_ptr<PythonObject>& instance, const char* name, Args&&... args) {
    if (!instance) return;
    py::gil_scoped_acquire gil;
    try {
        instance->object.attr(name)(std::forward<Args>(args)...);
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}
}  // namespace

void ScriptedRenderViewPanel::startRender() { callHook(m_instance, "start"); }

void ScriptedRenderViewPanel::stopRender() { callHook(m_instance, "stop"); }

void ScriptedRenderViewPanel::reload() { callHook(m_instance, "reload"); }

void ScriptedRenderViewPanel::cameraChanged(const RenderCamera& camera) {
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        m_instance->object.attr("camera_changed")(bridge("_render_camera")(cameraValues(camera)));
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

void ScriptedRenderViewPanel::sizeChanged(const QSize& size) {
    callHook(m_instance, "size_changed", size.width(), size.height());
}

void ScriptedRenderViewPanel::stageEdited(const usdcc::usd::StageChanges& changes) {
    if (!m_instance) return;
    py::gil_scoped_acquire gil;
    try {
        bridge("_render_stage_edited")(m_instance->object, pathStrings(changes.resyncedPaths),
                                       pathStrings(changes.changedInfoOnlyPaths));
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

void ScriptedRenderViewPanel::closing() { closeInstance(); }

void ScriptedRenderViewPanel::onStageChanged(const StageRefPtr& stage) {
    tellStage(stage);
    RenderViewPanel::onStageChanged(stage);
}

QJsonObject ScriptedRenderViewPanel::saveSessionState() const {
    if (!m_instance) return {};
    std::string text;
    {
        py::gil_scoped_acquire gil;
        try {
            text = bridge("_view_panel_save_state")(m_instance->object).cast<std::string>();
        } catch (py::error_already_set& error) {
            logPythonError(error);
        }
    }
    if (text.empty()) return {};
    QJsonObject state;
    // Kept as the JSON value save_state() returned, whatever its kind.
    state.insert(QStringLiteral("python"), QJsonDocument::fromJson("[" + QByteArray::fromStdString(text) + "]").array().at(0));
    return state;
}

void ScriptedRenderViewPanel::restoreSessionState(const QJsonObject& state) {
    if (!m_instance || !state.contains(QStringLiteral("python"))) return;
    const QByteArray array = QJsonDocument(QJsonArray{state.value(QStringLiteral("python"))}).toJson(QJsonDocument::Compact);
    const std::string text = array.mid(1, array.size() - 2).toStdString();
    py::gil_scoped_acquire gil;
    try {
        bridge("_view_panel_restore_state")(m_instance->object, text);
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}

}  // namespace usdcc::scripting
