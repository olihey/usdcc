// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#if USDCC_HAS_PYTHON
#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "python_object.h"
#include "usdcc/scripting/scripted_render_view_panel.h"
#include "usdcc/scripting/scripted_tool.h"
#include "usdcc/scripting/scripted_view_panel.h"
#include "usdcc/usd/playblast.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/python/bind_usd.h"
#endif

#include "usdcc/scripting/app_module.h"

#include "usdcc/core/settings.h"
#include "usdcc/core/build_features.h"
#include "usdcc/core/version.h"
#include "usdcc/scripting/debugger.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/python_settings.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/settings_dialog.h"
#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/toast_manager.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/ui/workspace_controller.h"
#include "usdcc/ui/workspace_library.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/main_menu_actions.h"
#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/render_view_panel.h"
#include "usdcc/usd/render/render_menu.h"
#include "usdcc/usd/render/render_queue.h"
#include "usdcc/usd/render/render_scheduler.h"
#include "usdcc/usd/renderer_settings.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>

#include <QFileInfo>
#include <QCoreApplication>
#include <QSettings>
#include <QTimer>
#include <QUndoStack>

#include <DockManager.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace usdcc::scripting {

// The Python class of a render view panel's handle (scripted_render_view_panel.cpp).
void bindRenderViewPanelHandle(pybind11::module_& m);
// Final renderers from Python (scripted_final_renderer.cpp).
void bindFinalRenderers(pybind11::module_& m);
void removeScriptedFinalRenderers();

namespace {
const AppContext* s_context = nullptr;
// What Python connected to the app, which has to end while Python is there.
std::map<int, std::shared_ptr<void>> s_eventConnections;
int s_nextEventConnection = 1;
}  // namespace

AppContextScope::AppContextScope(const AppContext& context) : m_context(context), m_outer(s_context) {
    s_context = &m_context;
}

AppContextScope::~AppContextScope() {
    s_context = m_outer;
    s_eventConnections.clear();
    usdcc::usd::PrimMenuEntries::instance().clear();
    usdcc::usd::ViewportMenuEntries::instance().clear();
#if USDCC_HAS_PYTHON
    // They call into Python, which goes next.
    if (!m_outer) removeScriptedFinalRenderers();
#endif
}

const AppContext* currentAppContext() { return s_context; }

#if USDCC_HAS_PYTHON
namespace {
namespace py = pybind11;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;

const AppContext& context() {
    if (!s_context) throw std::runtime_error("the usdcc app isn't available (it is starting up or shutting down)");
    return *s_context;
}

StageRefPtr stageFor(long cacheId) {
    StageRefPtr stage = context().stageManager->findByCacheId(cacheId);
    if (!stage) throw py::value_error("no open stage with cache id " + std::to_string(cacheId));
    return stage;
}

usdcc::ui::MainWindow& mainWindow() {
    if (!context().mainWindow) throw std::runtime_error("the usdcc app has no main window here");
    return *context().mainWindow;
}

usdcc::ui::ViewPanelRegistry& viewPanels() {
    if (!context().viewPanels) throw std::runtime_error("the usdcc app has no view panel types here");
    return *context().viewPanels;
}

// Types registered from Python, which may be registered again (replaced);
// the built-in ones may not.
std::set<std::string> s_pythonPanelTypes;

constexpr std::array<std::pair<Qt::DockWidgetArea, const char*>, 4> kAreaNames = {{
    {Qt::LeftDockWidgetArea, "left"},
    {Qt::RightDockWidgetArea, "right"},
    {Qt::TopDockWidgetArea, "top"},
    {Qt::BottomDockWidgetArea, "bottom"},
}};

QString settingsKey(const std::string& id, const std::string& key) {
    return QStringLiteral("PluginSettings/%1/%2").arg(QString::fromStdString(id), QString::fromStdString(key));
}

// One undo step, like a script run from the panel (docs/PLUGINS.md §3.7). An
// exception is logged.
void runAsOneStep(const std::string& label, const std::function<void()>& callback) {
    usdcc::usd::runAsOneUndoStep(s_context ? s_context->undoManager : nullptr, s_context ? s_context->stageManager : nullptr,
                                 label, [&callback]() {
                                     py::gil_scoped_acquire gil;
                                     try {
                                         callback();
                                         return true;
                                     } catch (py::error_already_set& error) {
                                         logPythonError(error);
                                         return false;
                                     }
                                 });
}

template <typename Signal, typename Callback>
int keepConnection(Signal& signal, Callback callback) {
    auto connection = std::make_shared<typename Signal::Connection>(signal.connect(std::move(callback)));
    const int id = s_nextEventConnection++;
    s_eventConnections[id] = std::move(connection);
    return id;
}

long cacheIdOf(const StageRefPtr& stage) { return stage ? stage->cacheId() : -1; }

std::vector<std::string> pathStrings(const std::vector<PXR_NS::SdfPath>& paths) {
    std::vector<std::string> strings;
    strings.reserve(paths.size());
    for (const PXR_NS::SdfPath& path : paths) strings.push_back(path.GetString());
    return strings;
}

// "stage_edited": every open stage's change notices as one connection. Stages
// opened later join; a closed stage's connection goes with it.
struct AllStagesEdited {
    using Callback = std::function<void(const StageRefPtr&, const usdcc::usd::StageChanges&)>;

    AllStagesEdited(usdcc::usd::StageManager& stages, Callback callback) : m_callback(std::move(callback)) {
        for (const StageRefPtr& stage : stages.stages()) add(stage);
        m_opened = stages.stageOpened.connect([this](const StageRefPtr& stage) { add(stage); });
        m_closed = stages.stageClosed.connect([this](const StageRefPtr& stage) { m_stages.erase(stage.get()); });
    }

private:
    void add(const StageRefPtr& stage) {
        // Weak: the connection must not keep a closed stage alive.
        std::weak_ptr<usdcc::usd::Stage> weak = stage;
        m_stages[stage.get()] = stage->changed.connect([this, weak](const usdcc::usd::StageChanges& changes) {
            if (const StageRefPtr locked = weak.lock()) m_callback(locked, changes);
        });
    }

    Callback m_callback;
    std::map<const usdcc::usd::Stage*, usdcc::core::Signal<const usdcc::usd::StageChanges&>::Connection> m_stages;
    usdcc::core::Signal<StageRefPtr>::Connection m_opened;
    usdcc::core::Signal<StageRefPtr>::Connection m_closed;
};

// "time_changed": every open stage's time, as AllStagesEdited does its edits.
struct AllStagesTime {
    using Callback = std::function<void(const StageRefPtr&, double)>;

    AllStagesTime(usdcc::usd::StageManager& stages, Callback callback) : m_callback(std::move(callback)) {
        for (const StageRefPtr& stage : stages.stages()) add(stage);
        m_opened = stages.stageOpened.connect([this](const StageRefPtr& stage) { add(stage); });
        m_closed = stages.stageClosed.connect([this](const StageRefPtr& stage) { m_stages.erase(stage.get()); });
    }

private:
    void add(const StageRefPtr& stage) {
        std::weak_ptr<usdcc::usd::Stage> weak = stage;
        m_stages[stage.get()] = stage->timeChanged.connect([this, weak](double time) {
            if (const StageRefPtr locked = weak.lock()) m_callback(locked, time);
        });
    }

    Callback m_callback;
    std::map<const usdcc::usd::Stage*, usdcc::core::Signal<double>::Connection> m_stages;
    usdcc::core::Signal<StageRefPtr>::Connection m_opened;
    usdcc::core::Signal<StageRefPtr>::Connection m_closed;
};

// "physics_changed": every open stage's simulation (docs/PHYSICS.md): its cached range,
// its stale mark or its reports changed.
struct AllStagesPhysics {
    using Callback = std::function<void(const StageRefPtr&)>;

    AllStagesPhysics(usdcc::usd::StageManager& stages, Callback callback) : m_callback(std::move(callback)) {
        for (const StageRefPtr& stage : stages.stages()) add(stage);
        m_opened = stages.stageOpened.connect([this](const StageRefPtr& stage) { add(stage); });
        m_closed = stages.stageClosed.connect([this](const StageRefPtr& stage) { m_stages.erase(stage.get()); });
    }

private:
    void add(const StageRefPtr& stage) {
        std::weak_ptr<usdcc::usd::Stage> weak = stage;
        m_stages[stage.get()] = stage->physics().changed.connect([this, weak]() {
            if (const StageRefPtr locked = weak.lock()) m_callback(locked);
        });
    }

    Callback m_callback;
    std::map<const usdcc::usd::Stage*, usdcc::core::Signal<>::Connection> m_stages;
    usdcc::core::Signal<StageRefPtr>::Connection m_opened;
    usdcc::core::Signal<StageRefPtr>::Connection m_closed;
};

// usdcc.app.play() and the rest: the timeline's transport, or a RuntimeError.
usdcc::usd::PlaybackController& playback() {
    if (!context().playback) throw std::runtime_error("this app has no timeline");
    return *context().playback;
}

// usdcc.app.Viewport's: the open viewport with that id, or a ValueError.
usdcc::usd::ViewportViewPanel& viewportWithId(int id) {
    usdcc::usd::ViewportViewPanel* viewport = usdcc::usd::ViewportViewPanel::withId(id);
    if (!viewport) throw py::value_error("viewport " + std::to_string(id) + " has been closed");
    return *viewport;
}

bool isKnownRenderer(const PXR_NS::TfToken& pluginId) {
    PXR_NS::HfPluginDesc desc;
    return PXR_NS::HdRendererPluginRegistry::GetInstance().GetPluginDesc(pluginId, &desc);
}

// The renderer a call is about: `renderer` when given, which must be one Hydra knows, else
// the viewport's own, which it has once it has drawn (or a renderer was set for it).
PXR_NS::TfToken rendererOf(usdcc::usd::ViewportViewPanel& viewport, const std::string& renderer) {
    if (!renderer.empty()) {
        const PXR_NS::TfToken pluginId(renderer);
        if (!isKnownRenderer(pluginId)) throw py::value_error("no renderer '" + renderer + "'");
        return pluginId;
    }
    const PXR_NS::TfToken current = viewport.hydraWindow()->currentRendererPlugin();
    if (current.IsEmpty()) {
        throw py::value_error("the viewport has no renderer until it has drawn: name one with renderer=");
    }
    return current;
}

const usdcc::usd::RendererSettingInfo& rendererSettingInfo(const PXR_NS::TfToken& pluginId, const std::string& key) {
    for (const usdcc::usd::RendererSettingInfo& setting : usdcc::usd::rendererSettings(pluginId)) {
        if (setting.key == key) return setting;
    }
    throw py::key_error(pluginId.GetString() + " has no setting '" + key + "'");
}

py::object pythonValue(const usdcc::core::SettingValue& value) {
    return std::visit([](const auto& v) -> py::object { return py::cast(v); }, value);
}

py::dict pythonValues(const usdcc::usd::RendererSettingValues& values) {
    py::dict result;
    for (const auto& [key, value] : values) {
        if (const std::optional<usdcc::core::SettingValue> setting = usdcc::usd::toSettingValue(value)) {
            result[py::str(key.GetString())] = pythonValue(*setting);
        }
    }
    return result;
}

// A bool, int, float or str from Python; nothing for anything else.
std::optional<usdcc::core::SettingValue> settingValueOf(const py::handle& value) {
    if (py::isinstance<py::bool_>(value)) return value.cast<bool>();  // before int: a bool is one
    if (py::isinstance<py::int_>(value)) {
        const long long number = value.cast<long long>();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) return std::nullopt;
        return static_cast<int>(number);
    }
    if (py::isinstance<py::float_>(value)) return value.cast<double>();
    if (py::isinstance<py::str>(value)) return value.cast<std::string>();
    return std::nullopt;
}

// For a callback that a Signal holds: logged rather than thrown into C++.
template <typename... Args>
void callLogged(const std::shared_ptr<PythonObject>& callback, Args&&... args) {
    py::gil_scoped_acquire gil;
    try {
        callback->object(std::forward<Args>(args)...);
    } catch (py::error_already_set& error) {
        logPythonError(error);
    }
}
}  // namespace

// The running app for usdcc.app / usdcc.usd / usdcc.tools (src/python/usdcc).
// Compiled into the app (and the tests), not into _usd.pyd: a plain python
// has no app. Stages cross as UsdUtilsStageCache ids (see bind_usd.h).
PYBIND11_EMBEDDED_MODULE(_usdcc_app, m) {
    m.doc() = "The running usdcc app. Use usdcc.app, usdcc.usd and usdcc.tools, not this.";

    usdcc::usd::python::bindStageManager(m);

    py::class_<UndoManager>(m, "UndoManager")
        .def(
            "begin_edit",
            [](UndoManager& self, long cacheId, const std::string& description) {
                self.beginEdit(stageFor(cacheId), QString::fromStdString(description));
            },
            py::arg("cache_id"), py::arg("description"))
        .def(
            "end_edit", [](UndoManager& self, long cacheId) { return self.endEdit(stageFor(cacheId)); },
            py::arg("cache_id"), "True if the edit changed the stage (and so became an undo step).")
        .def(
            "cancel_edit", [](UndoManager& self, long cacheId) { self.cancelEdit(stageFor(cacheId)); },
            py::arg("cache_id"))
        .def(
            "is_editing", [](const UndoManager& self, long cacheId) { return self.isEditing(stageFor(cacheId)); },
            py::arg("cache_id"))
        .def(
            "undo",
            [](UndoManager& self, long cacheId) {
                if (QUndoStack* stack = self.stack(stageFor(cacheId)); stack->canUndo()) stack->undo();
            },
            py::arg("cache_id"))
        .def(
            "redo",
            [](UndoManager& self, long cacheId) {
                if (QUndoStack* stack = self.stack(stageFor(cacheId)); stack->canRedo()) stack->redo();
            },
            py::arg("cache_id"))
        .def(
            "can_undo", [](UndoManager& self, long cacheId) { return self.stack(stageFor(cacheId))->canUndo(); },
            py::arg("cache_id"))
        .def(
            "can_redo", [](UndoManager& self, long cacheId) { return self.stack(stageFor(cacheId))->canRedo(); },
            py::arg("cache_id"))
        .def(
            "undo_text",
            [](UndoManager& self, long cacheId) { return self.stack(stageFor(cacheId))->undoText().toStdString(); },
            py::arg("cache_id"), "The description of the step Undo would revert ('' if none).")
        .def(
            "redo_text",
            [](UndoManager& self, long cacheId) { return self.stack(stageFor(cacheId))->redoText().toStdString(); },
            py::arg("cache_id"))
        .def(
            "count", [](UndoManager& self, long cacheId) { return self.stack(stageFor(cacheId))->count(); },
            py::arg("cache_id"), "How many steps the stage's history holds.");

    m.def("is_available", []() { return s_context != nullptr; });
    m.def("version", []() { return std::string(usdcc::core::version()); }, "usdcc's version (docs/RELEASE.md).");
    m.def("is_release_build", []() { return usdcc::core::isReleaseBuild(); });
    // (name, label, included, plugin ids) per feature (docs/BUILD_OPTIONS.md §5).
    m.def("build_features", []() {
        py::list result;
        for (const usdcc::core::BuildFeature& feature : usdcc::core::buildFeatures()) {
            result.append(py::make_tuple(feature.name, feature.label, feature.included, feature.pluginIds));
        }
        return result;
    });
    m.def("user_data_folder", []() { return usdcc::ui::userDataFolder().toStdString(); });
    // usdcc.debug: debugpy's folder, the program for its helper process, and the port to use.
    m.def("debug_info", []() {
        const PythonRuntime* python = PythonRuntime::instance();
        if (!python) throw std::runtime_error("Python isn't available");
        const auto text = [](const std::filesystem::path& path) { return QString::fromStdU16String(path.generic_u16string()).toStdString(); };
        return std::make_tuple(text(debugger::folder()), text(python->executable()),
                               usdcc::core::Settings::instance().get<int>(kDebugPortKey, 5678));
    });
    m.def("debug_port", []() { return debugger::port(); });
    m.def("set_debug_port", [](int port) { debugger::setPort(port); });
    m.def(
        "stage_manager", []() { return context().stageManager; }, py::return_value_policy::reference,
        "The app's StageManager.");
    m.def(
        "undo_manager", []() { return context().undoManager; }, py::return_value_policy::reference,
        "The app's UndoManager.");

    // Qt objects cross as their C++ addresses, which usdcc/app.py turns into
    // PySide objects and back with shiboken6: this module is pybind11 and
    // knows nothing of PySide.
    m.def("main_window_address", []() {
        return reinterpret_cast<std::uintptr_t>(static_cast<QMainWindow*>(&mainWindow()));
    });
    m.def(
        "add_side_panel",
        [](std::uintptr_t address, const std::string& areaName) {
            usdcc::ui::MainWindow& window = mainWindow();
            for (const auto& [area, name] : kAreaNames) {
                if (areaName == name) {
                    // usdcc/app.py checked that it is a usdcc.ui.SidePanel.
                    window.addSidePanel(reinterpret_cast<usdcc::ui::SidePanel*>(address), area);
                    return;
                }
            }
            throw py::value_error("no dock area named '" + areaName + "' (left, right, top or bottom)");
        },
        py::arg("address"), py::arg("area"));
    m.def(
        "remove_side_panel",
        [](std::uintptr_t address) { mainWindow().removeSidePanel(reinterpret_cast<usdcc::ui::SidePanel*>(address)); },
        py::arg("address"));
    // A view panel busy (docs/PANEL_BUSY.md §6): usdcc.ui.ViewPanel.busy() wraps the handle.
    py::class_<usdcc::ui::Busy, std::shared_ptr<usdcc::ui::Busy>>(m, "Busy")
        .def("set_text", [](usdcc::ui::Busy& self, const std::string& text) { self.setText(QString::fromStdString(text)); })
        .def("set_detail", [](usdcc::ui::Busy& self, const std::string& text) { self.setDetail(QString::fromStdString(text)); })
        .def("set_progress", &usdcc::ui::Busy::setProgress)
        .def("is_cancelled", &usdcc::ui::Busy::isCancelled)
        .def("finish", &usdcc::ui::Busy::finish);
    m.def(
        "begin_busy",
        [](std::uintptr_t widgetAddress, const std::string& text, bool cancellable, bool paintNow) {
            // The panel is the widget's first ancestor that is one.
            usdcc::ui::ViewPanel* panel = nullptr;
            for (QWidget* widget = reinterpret_cast<QWidget*>(widgetAddress); widget && !panel; widget = widget->parentWidget()) {
                panel = qobject_cast<usdcc::ui::ViewPanel*>(widget);
            }
            if (!panel) throw py::value_error("the widget isn't in a view panel");
            usdcc::ui::BusyOptions options;
            options.paintNow = paintNow;
            // Cancel shows; what it does is the handle's is_cancelled(), which the script reads.
            if (cancellable) options.onCancel = []() {};
            return panel->beginBusy(QString::fromStdString(text), std::move(options));
        },
        py::arg("widget"), py::arg("text"), py::arg("cancellable"), py::arg("paint_now"));
    m.def(
        "unregister_view_panel",
        [](const std::string& id) {
            if (!s_pythonPanelTypes.erase(id)) return false;
            return viewPanels().remove(QString::fromStdString(id));
        },
        py::arg("id"));
    m.def(
        "reload_plugin",
        [](const std::string& id) {
            if (!context().plugins) throw std::runtime_error("there are no plugins here");
            PluginManager* plugins = context().plugins;
            // Not now: the caller may be the plugin's own menu action.
            QTimer::singleShot(0, QCoreApplication::instance(), [plugins, id]() {
                if (PythonRuntime* python = PythonRuntime::instance()) plugins->reload(id, *python);
            });
        },
        py::arg("id"));

    m.def(
        "register_view_panel",
        [](const std::string& id, const std::string& displayName, py::object cls) {
            usdcc::ui::ViewPanelRegistry& registry = viewPanels();
            const QString qid = QString::fromStdString(id);
            if (registry.find(qid) && !s_pythonPanelTypes.count(id)) {
                throw py::value_error("'" + id + "' is a built-in view panel type");
            }
            auto pythonClass = std::make_shared<usdcc::scripting::PythonObject>(std::move(cls));
            usdcc::usd::StageManager* stages = context().stageManager;
            registry.add(qid, QString::fromStdString(displayName),
                         [stages, pythonClass](const StageRefPtr& stage, const QString& title,
                                               QWidget* parent) -> usdcc::ui::ViewPanel* {
                             return new usdcc::scripting::ScriptedViewPanel(stages, stage, title, pythonClass, parent);
                         });
            s_pythonPanelTypes.insert(id);
        },
        py::arg("id"), py::arg("display_name"), py::arg("cls"));
    // A usdcc.ui.RenderViewPanel subclass (docs/RENDER_VIEW_PANEL.md); usdcc.app's
    // register_view_panel() picks this or the one above.
    m.def(
        "register_render_view_panel",
        [](const std::string& id, const std::string& displayName, py::object cls) {
            usdcc::ui::ViewPanelRegistry& registry = viewPanels();
            const QString qid = QString::fromStdString(id);
            if (registry.find(qid) && !s_pythonPanelTypes.count(id)) {
                throw py::value_error("'" + id + "' is a built-in view panel type");
            }
            auto pythonClass = std::make_shared<usdcc::scripting::PythonObject>(std::move(cls));
            usdcc::usd::StageManager* stages = context().stageManager;
            registry.add(qid, QString::fromStdString(displayName),
                         [stages, pythonClass](const StageRefPtr& stage, const QString& title,
                                               QWidget* parent) -> usdcc::ui::ViewPanel* {
                             return new usdcc::scripting::ScriptedRenderViewPanel(stages, stage, title, pythonClass,
                                                                                  parent);
                         });
            s_pythonPanelTypes.insert(id);
        },
        py::arg("id"), py::arg("display_name"), py::arg("cls"));
    bindRenderViewPanelHandle(m);
    bindFinalRenderers(m);
    m.def(
        "open_view_panel",
        [](const std::string& id, long cacheId) -> py::object {
            usdcc::ui::ViewPanelRegistry& registry = viewPanels();
            if (!registry.find(QString::fromStdString(id))) throw py::value_error("no view panel type '" + id + "'");
            usdcc::ui::MainWindow& window = mainWindow();
            usdcc::ui::ViewPanel* panel =
                registry.open(QString::fromStdString(id), stageFor(cacheId), window.dockManager(), &window);
            if (!panel) throw std::runtime_error("the '" + id + "' panel couldn't be created");
            // The Python instance for a Python type, None for a built-in one.
            auto* scripted = qobject_cast<usdcc::scripting::ScriptedViewPanel*>(panel);
            if (scripted && scripted->pythonPanel()) return scripted->pythonPanel()->object;
            auto* rendering = qobject_cast<usdcc::scripting::ScriptedRenderViewPanel*>(panel);
            if (rendering && rendering->pythonPanel()) return rendering->pythonPanel()->object;
            return py::none();
        },
        py::arg("id"), py::arg("cache_id"), "Opens a panel of the type for the stage, like the Stages menu.");
    // Pipelines' prim icons (docs/PRIM_ICONS.md phase 2).
    m.def(
        "set_prim_icon",
        [](const std::string& typeName, const std::string& file) {
            if (typeName.empty()) throw py::value_error("no type name");
            if (!file.empty() && !QFileInfo::exists(QString::fromStdString(file))) {
                throw py::value_error("no such file: " + file);
            }
            usdcc::usd::prim_icons::setTypeIcon(typeName, QString::fromStdString(file));
        },
        py::arg("type_name"), py::arg("file"));
    m.def(
        "prim_icon",
        [](const std::string& typeName) {
            return usdcc::usd::prim_icons::iconNameForType(PXR_NS::TfToken(typeName)).toStdString();
        },
        py::arg("type_name"));
    m.def("view_panel_types", []() {
        std::vector<std::pair<std::string, std::string>> types;
        for (const auto& entry : viewPanels().entries()) {
            types.emplace_back(entry.id.toStdString(), entry.displayName.toStdString());
        }
        return types;
    });

    m.def("tool_names", []() {
        std::vector<std::string> names;
        for (const auto& entry : context().toolSet->tools()) names.push_back(entry.id);
        return names;
    });
    m.def("active_tool", []() -> std::string { return context().toolSet->activeId(); });
    m.def(
        "set_active_tool",
        [](const std::string& toolName) {
            if (!context().toolSet->setActive(toolName)) throw py::value_error("no tool named '" + toolName + "'");
        },
        py::arg("name"));

    // usdcc.toast (docs/NOTIFICATIONS.md): the main window's toasts.
    m.def(
        "toast_show",
        [](const std::string& title, const std::string& body, const std::string& kind, int durationMs) {
            usdcc::core::ToastRequest request;
            usdcc::core::ToastKind parsed{};
            if (!usdcc::core::parseToastKind(kind, &parsed)) {
                throw py::value_error("unknown toast kind '" + kind + "' (info, success, warning or error)");
            }
            request.kind = parsed;
            request.title = title;
            request.body = body;
            request.durationMs = durationMs;
            return mainWindow().toasts()->show(request);
        },
        py::arg("title"), py::arg("body") = "", py::arg("kind") = "info", py::arg("duration_ms") = -1);
    m.def("toast_dismiss", [](int id) { mainWindow().toasts()->dismiss(id); }, py::arg("id"));
    m.def("toast_dismiss_all", []() { mainWindow().toasts()->dismissAll(); });
    m.def("toast_visible", []() {
        std::vector<int> ids;
        for (const usdcc::ui::ToastWidget* toast : mainWindow().toasts()->visibleToasts()) ids.push_back(toast->id());
        return ids;
    });

    // usdcc.settings (docs/SETTINGS.md): the process-wide registry, so these
    // need no AppContext.
    using usdcc::core::SettingDefinition;
    using usdcc::core::Settings;
    using usdcc::core::SettingValue;
    const auto definitionOf = [](const std::string& key) -> const SettingDefinition& {
        const SettingDefinition* definition = Settings::instance().definition(key);
        if (!definition) throw py::key_error("no setting '" + key + "'");
        return *definition;
    };
    m.def(
        "settings_get", [definitionOf](const std::string& key) { return Settings::instance().value(definitionOf(key).key); },
        py::arg("key"));
    m.def(
        "settings_set",
        [definitionOf](const std::string& key, const SettingValue& value) {
            const SettingDefinition& definition = definitionOf(key);
            if (!Settings::instance().setValue(key, value)) {
                throw py::value_error("not a valid value for setting '" + key + "' (a " +
                                      std::string(usdcc::core::settingTypeName(definition.type)) + ")");
            }
        },
        py::arg("key"), py::arg("value"));
    m.def(
        "settings_reset", [definitionOf](const std::string& key) { Settings::instance().reset(definitionOf(key).key); },
        py::arg("key"));
    m.def(
        "settings_is_default",
        [definitionOf](const std::string& key) { return Settings::instance().isDefault(definitionOf(key).key); },
        py::arg("key"));
    m.def(
        "settings_keys",
        [](const std::string& category) {
            std::vector<std::string> keys;
            for (const SettingDefinition* definition : Settings::instance().definitions(category)) {
                keys.push_back(definition->key);
            }
            return keys;
        },
        py::arg("category") = std::string());
    m.def("settings_categories", []() { return Settings::instance().categories(); });
    m.def(
        "settings_show",
        [](const std::string& category) {
            usdcc::ui::SettingsDialog* dialog = usdcc::ui::SettingsDialog::showDialog(&mainWindow());
            if (!category.empty()) dialog->showCategory(QString::fromStdString(category));
        },
        py::arg("category"));
    m.def(
        "settings_info",
        [definitionOf](const std::string& key) {
            const SettingDefinition& definition = definitionOf(key);
            py::dict info;
            info["key"] = definition.key;
            info["type"] = std::string(usdcc::core::settingTypeName(definition.type));
            info["category"] = definition.category;
            info["group"] = definition.group;
            info["label"] = definition.label;
            info["description"] = definition.description;
            info["default"] = definition.defaultValue;
            info["value"] = Settings::instance().value(key);
            if (definition.maximum > definition.minimum) {
                info["minimum"] = definition.minimum;
                info["maximum"] = definition.maximum;
            }
            if (!definition.choices.empty()) info["choices"] = definition.choices;
            info["applies_at_next_start"] = definition.appliesAtNextStart;
            return info;
        },
        py::arg("key"));
    m.def(
        "settings_define",
        [](const std::string& key, const std::string& type, const SettingValue& defaultValue, const std::string& label,
           const std::string& category, const std::string& group, const std::string& description,
           std::optional<double> minimum, std::optional<double> maximum, std::vector<std::string> choices,
           bool appliesAtNextStart) {
            SettingDefinition definition;
            definition.key = key;
            if (!usdcc::core::parseSettingType(type, &definition.type)) {
                throw py::value_error("unknown setting type '" + type +
                                      "' (bool, int, float, string, choice, color, shortcut, folder or folders)");
            }
            if (definition.type == usdcc::core::SettingType::Action) {
                throw py::value_error("an action setting can't be defined from Python");
            }
            definition.defaultValue = defaultValue;
            definition.label = label;
            definition.category = category;
            definition.group = group;
            definition.description = description;
            if (minimum.has_value() != maximum.has_value()) {
                throw py::value_error("give both minimum and maximum, or neither");
            }
            if (minimum) {
                if (*maximum <= *minimum) throw py::value_error("maximum must be greater than minimum");
                definition.minimum = *minimum;
                definition.maximum = *maximum;
            }
            definition.choices = std::move(choices);
            definition.appliesAtNextStart = appliesAtNextStart;
            if (!Settings::instance().define(std::move(definition))) {
                throw py::value_error("invalid setting '" + key +
                                      "': it needs a key and a category, and a default that fits its type");
            }
        },
        py::arg("key"), py::arg("type"), py::arg("default"), py::arg("label") = std::string(),
        py::arg("category") = std::string("Python"), py::arg("group") = std::string(),
        py::arg("description") = std::string(), py::arg("minimum") = py::none(), py::arg("maximum") = py::none(),
        py::arg("choices") = std::vector<std::string>(), py::arg("applies_at_next_start") = false);

    m.def(
        "register_tool",
        [](const std::string& id, const std::string& label, py::object instance, const std::string& icon) {
            if (usdcc::tools::ToolSet::isBuiltIn(id)) throw py::value_error("'" + id + "' is a built-in tool");
            if (id.empty()) throw py::value_error("a tool needs an id");
            context().toolSet->addTool(id, label, icon,
                                       std::make_unique<ScriptedTool>(
                                           label, std::make_shared<PythonObject>(std::move(instance))));
        },
        py::arg("id"), py::arg("label"), py::arg("instance"), py::arg("icon"));
    m.def(
        "unregister_tool", [](const std::string& id) { return context().toolSet->removeTool(id); }, py::arg("id"));

    // Menus (docs/PLUGINS.md phase 2). Callbacks get plain values, which
    // usdcc/app.py wraps.
    m.def(
        "add_menu_action",
        [](const std::string& path, py::object callback, const std::string& shortcut, const std::string& tip) {
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const QString qpath = QString::fromStdString(path);
            const std::string description = qpath.section(QLatin1Char('/'), -1).remove(QLatin1Char('&')).toStdString();
            const auto run = [held, description]() { runAsOneStep(description, [&held]() { held->object(); }); };
            if (!usdcc::usd::addMainMenuAction(mainWindow(), qpath, run, QString::fromStdString(shortcut),
                                               QString::fromStdString(tip))) {
                throw py::value_error("a menu action's path is 'Menu/Action', not '" + path + "'");
            }
        },
        py::arg("path"), py::arg("callback"), py::arg("shortcut"), py::arg("tip"));
    m.def(
        "remove_menu_action",
        [](const std::string& path) { return usdcc::usd::removeMainMenuAction(mainWindow(), QString::fromStdString(path)); },
        py::arg("path"));
    m.def(
        "add_prim_menu_action",
        [](const std::string& id, const std::string& label, py::object callback, py::object applies) {
            usdcc::usd::PrimMenuEntry entry;
            entry.id = id;
            entry.label = QString::fromStdString(label);
            // pathStrings, not a local lambda captured by the two below: MSVC's ARM64
            // compiler (Visual Studio 2026) crashes on that (C1001).
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const std::string description = label.substr(label.rfind('/') + 1);
            entry.run = [held, description](const usdcc::usd::PrimMenuContext& menu) {
                runAsOneStep(description,
                             [&]() { held->object(cacheIdOf(menu.stage), pathStrings(menu.paths), menu.clicked.GetString()); });
            };
            if (!applies.is_none()) {
                auto heldApplies = std::make_shared<PythonObject>(std::move(applies));
                entry.applies = [heldApplies](const usdcc::usd::PrimMenuContext& menu) {
                    py::gil_scoped_acquire gil;
                    try {
                        return heldApplies->object(cacheIdOf(menu.stage), pathStrings(menu.paths), menu.clicked.GetString())
                            .cast<bool>();
                    } catch (py::error_already_set& error) {
                        logPythonError(error);
                        return false;
                    }
                };
            }
            usdcc::usd::PrimMenuEntries::instance().add(std::move(entry));
        },
        py::arg("id"), py::arg("label"), py::arg("callback"), py::arg("applies"));
    m.def(
        "remove_prim_menu_action", [](const std::string& id) { usdcc::usd::PrimMenuEntries::instance().remove(id); },
        py::arg("id"));
    // A viewport's menus (docs/PLUGINS.md): callbacks get the viewport's id, which
    // usdcc/app.py makes a Viewport.
    m.def(
        "add_viewport_menu_action",
        [](const std::string& id, const std::string& label, py::object callback, py::object checked, py::object applies,
           py::object enabled) {
            usdcc::usd::ViewportMenuEntry entry;
            entry.id = id;
            entry.label = QString::fromStdString(label);
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const std::string description = label.substr(label.rfind('/') + 1);
            entry.run = [held, description](usdcc::usd::ViewportViewPanel* viewport) {
                runAsOneStep(description, [&]() { held->object(viewport->id()); });
            };
            const auto predicate = [](py::object function) -> std::function<bool(usdcc::usd::ViewportViewPanel*)> {
                if (function.is_none()) return {};
                auto heldFunction = std::make_shared<PythonObject>(std::move(function));
                return [heldFunction](usdcc::usd::ViewportViewPanel* viewport) {
                    py::gil_scoped_acquire gil;
                    try {
                        return heldFunction->object(viewport->id()).cast<bool>();
                    } catch (py::error_already_set& error) {
                        logPythonError(error);
                        return false;
                    }
                };
            };
            entry.checked = predicate(std::move(checked));
            entry.applies = predicate(std::move(applies));
            entry.enabled = predicate(std::move(enabled));
            usdcc::usd::ViewportMenuEntries::instance().add(std::move(entry));
        },
        py::arg("id"), py::arg("label"), py::arg("callback"), py::arg("checked"), py::arg("applies"),
        py::arg("enabled"));
    m.def(
        "remove_viewport_menu_action",
        [](const std::string& id) { usdcc::usd::ViewportMenuEntries::instance().remove(id); }, py::arg("id"));

    // The camera of the viewport the user last worked in, as plain values that
    // usdcc/app.py turns into a ViewportCamera: (stage cache id or -1,
    // camera-to-world matrix as 16 doubles, row by row, vertical field of view
    // in degrees, aspect ratio, near, far). None with no viewport open.
    m.def("active_viewport_camera", []() -> py::object {
        usdcc::usd::ViewportViewPanel* viewport = usdcc::usd::ViewportViewPanel::activeViewport();
        if (!viewport) return py::none();
        const usdcc::usd::RenderCamera camera = usdcc::usd::viewportRenderCamera(*viewport);
        std::vector<double> matrix(camera.cameraToWorld.data(), camera.cameraToWorld.data() + 16);
        return py::make_tuple(cacheIdOf(viewport->currentStage()), matrix, camera.fovY, camera.aspect, camera.nearClip,
                              camera.farClip);
    });

    // The viewports, by id (ViewportViewPanel::id()), for usdcc/app.py's Viewport, and
    // their renderers' settings (docs/VIEWPORT_DISPLAY.md §7). A `renderer` of "" is the
    // viewport's own.
    using usdcc::usd::ViewportViewPanel;
    m.def("viewports", []() {
        std::vector<int> ids;
        for (const ViewportViewPanel* viewport : ViewportViewPanel::openViewports()) ids.push_back(viewport->id());
        return ids;
    });
    m.def("active_viewport", []() -> py::object {
        const ViewportViewPanel* viewport = ViewportViewPanel::activeViewport();
        return viewport ? py::object(py::int_(viewport->id())) : py::none();
    });
    m.def("viewport_exists", [](int id) { return ViewportViewPanel::withId(id) != nullptr; }, py::arg("id"));
    m.def(
        "viewport_title", [](int id) { return viewportWithId(id).windowTitle().toStdString(); }, py::arg("id"));
    m.def(
        "viewport_stage", [](int id) { return cacheIdOf(viewportWithId(id).currentStage()); }, py::arg("id"));
    // A playblast (docs/ANIMATION.md phase 5): the files written.
    m.def(
        "viewport_playblast",
        [](int id, const std::string& pattern, double start, double end, double step) {
            ViewportViewPanel& viewport = viewportWithId(id);
            if (!viewport.currentStage() || !viewport.hydraWindow()) throw py::value_error("the viewport has no stage");
            usdcc::usd::PlayblastOptions options;
            options.frames = {start, end};
            options.step = step;
            options.pattern = QString::fromStdString(pattern);
            QString error;
            const QStringList files = usdcc::usd::playblast(*viewport.hydraWindow(), *viewport.currentStage(), options, {}, &error);
            if (!error.isEmpty()) throw std::runtime_error("the playblast stopped: " + error.toStdString());
            std::vector<std::string> result;
            for (const QString& file : files) result.push_back(file.toStdString());
            return result;
        },
        py::arg("id"), py::arg("pattern"), py::arg("start"), py::arg("end"), py::arg("step"));
    // The stage camera it looks through, "" for its own (docs/VIEWPORT_CAMERA.md §3).
    m.def(
        "viewport_camera", [](int id) { return viewportWithId(id).hydraWindow()->stageCamera().GetString(); },
        py::arg("id"));
    m.def(
        "set_viewport_camera",
        [](int id, const std::string& path) {
            ViewportViewPanel& viewport = viewportWithId(id);
            if (!path.empty() && !PXR_NS::SdfPath::IsValidPathString(path)) throw py::value_error("not a prim path: '" + path + "'");
            if (!viewport.lookThrough(path.empty() ? PXR_NS::SdfPath() : PXR_NS::SdfPath(path))) {
                throw py::value_error("the viewport's stage has no camera at '" + path + "'");
            }
        },
        py::arg("id"), py::arg("path"));
    // Every renderer Hydra knows, as {id: display name}.
    m.def("renderers", []() {
        PXR_NS::HfPluginDescVector plugins;
        PXR_NS::HdRendererPluginRegistry::GetInstance().GetPluginDescs(&plugins);
        std::map<std::string, std::string> names;
        for (const PXR_NS::HfPluginDesc& plugin : plugins) names[plugin.id.GetString()] = plugin.displayName;
        return names;
    });
    m.def(
        "viewport_renderer", [](int id) { return viewportWithId(id).hydraWindow()->currentRendererPlugin().GetString(); },
        py::arg("id"));
    // The ones it can switch to: once it has drawn, those that can render here; before, all.
    m.def(
        "viewport_renderers",
        [](int id) {
            std::vector<std::string> ids;
            for (const PXR_NS::TfToken& plugin : viewportWithId(id).hydraWindow()->rendererPlugins()) ids.push_back(plugin.GetString());
            if (ids.empty()) {
                PXR_NS::HfPluginDescVector plugins;
                PXR_NS::HdRendererPluginRegistry::GetInstance().GetPluginDescs(&plugins);
                for (const PXR_NS::HfPluginDesc& plugin : plugins) ids.push_back(plugin.id.GetString());
            }
            return ids;
        },
        py::arg("id"));
    m.def(
        "set_viewport_renderer",
        [](int id, const std::string& renderer) {
            ViewportViewPanel& viewport = viewportWithId(id);
            if (!isKnownRenderer(PXR_NS::TfToken(renderer))) throw py::value_error("no renderer '" + renderer + "'");
            viewport.setRenderer(PXR_NS::TfToken(renderer));
            return viewport.hydraWindow()->currentRendererPlugin().GetString() == renderer;
        },
        py::arg("id"), py::arg("renderer"));
    // A renderer's settings: (key, label, type name, default, Settings key) each. Defines
    // their Settings > Renderers defaults, so usdcc.settings has them.
    m.def(
        "renderer_settings_info",
        [](const std::string& renderer) {
            const PXR_NS::TfToken pluginId(renderer);
            if (!isKnownRenderer(pluginId)) throw py::value_error("no renderer '" + renderer + "'");
            usdcc::usd::defineRendererSettings(pluginId);
            py::list result;
            for (const usdcc::usd::RendererSettingInfo& setting : usdcc::usd::rendererSettings(pluginId)) {
                const std::optional<usdcc::core::SettingValue> defaultValue = usdcc::usd::toSettingValue(setting.defaultValue);
                result.append(py::make_tuple(setting.key.GetString(), setting.label,
                                             std::string(usdcc::core::settingTypeName(setting.type)),
                                             defaultValue ? pythonValue(*defaultValue) : py::none(),
                                             usdcc::usd::rendererSettingKey(pluginId, setting.key)));
            }
            return result;
        },
        py::arg("renderer"));
    // Final renders (docs/FINAL_RENDER.md §6), for usdcc.render.
    m.def("final_renderers", []() {
        py::list result;
        for (const auto& renderer : usdcc::render::FinalRenderers::instance().all()) {
            result.append(py::make_tuple(renderer->id(), renderer->name(), renderer->setupProblem(),
                                         renderer->capabilities().tiles));
        }
        return result;
    });
    m.def(
        "submit_render",
        [](long cacheId, const std::string& settings, bool animation, const std::string& renderer) {
            if (!settings.empty() && !PXR_NS::SdfPath::IsValidPathString(settings)) {
                throw py::value_error("not a prim path: " + settings);
            }
            std::string error;
            const usdcc::render::RenderJobPtr job =
                usdcc::render::submitRender(stageFor(cacheId), settings.empty() ? PXR_NS::SdfPath() : PXR_NS::SdfPath(settings),
                                            animation, nullptr, &error, renderer);
            if (!job) throw std::runtime_error(error);
            return job->id;
        },
        py::arg("stage"), py::arg("settings"), py::arg("animation"), py::arg("renderer"));
    m.def("render_jobs", []() {
        py::list result;
        for (const usdcc::render::RenderJobPtr& job : usdcc::render::RenderScheduler::instance().jobs()) {
            static const char* statuses[] = {"queued", "running", "done", "failed", "cancelled"};
            result.append(py::make_tuple(job->id, job->title, job->rendererId, statuses[static_cast<int>(job->status)],
                                         job->progress(), job->frames, job->files, job->error, std::string()));
        }
        // Jobs sent to a queue server are the queue's, not the scheduler's: by the id
        // submit_render() returned, with the queue's status.
        if (const usdcc::render::RenderQueueClient* queues = usdcc::render::RenderQueueClient::instance()) {
            for (const usdcc::render::SentJobInfo& job : queues->sentJobs()) {
                std::string status = job.status.toStdString();
                if (status == "sending" || status == "receiving") status = "queued";
                if (status == "refused") status = "failed";
                std::vector<std::string> files;
                for (const QString& file : job.files) files.push_back(file.toStdString());
                result.append(py::make_tuple(job.artistJob.toStdString(), job.title.toStdString(), job.renderer.toStdString(),
                                             status, job.progress, job.frames, files, job.error.toStdString(),
                                             job.queueName.toStdString()));
            }
        }
        return result;
    });
    m.def(
        "cancel_render",
        [](const std::string& id) {
            usdcc::render::RenderScheduler::instance().cancel(id);
            if (usdcc::render::RenderQueueClient* queues = usdcc::render::RenderQueueClient::instance()) {
                queues->cancelSent(QString::fromStdString(id));
            }
        },
        py::arg("id"));
    m.def(
        "viewport_renderer_settings",
        [](int id, const std::string& renderer) {
            ViewportViewPanel& viewport = viewportWithId(id);
            return pythonValues(viewport.rendererSettingValues(rendererOf(viewport, renderer)));
        },
        py::arg("id"), py::arg("renderer"));
    m.def(
        "viewport_renderer_overrides",
        [](int id, const std::string& renderer) {
            ViewportViewPanel& viewport = viewportWithId(id);
            const auto found = viewport.rendererOverrides().find(rendererOf(viewport, renderer));
            return found == viewport.rendererOverrides().end() ? py::dict() : pythonValues(found->second);
        },
        py::arg("id"), py::arg("renderer"));
    m.def(
        "set_viewport_renderer_setting",
        [](int id, const std::string& renderer, const std::string& key, const py::object& value) {
            ViewportViewPanel& viewport = viewportWithId(id);
            const PXR_NS::TfToken pluginId = rendererOf(viewport, renderer);
            const usdcc::usd::RendererSettingInfo& setting = rendererSettingInfo(pluginId, key);
            const std::optional<usdcc::core::SettingValue> plain = settingValueOf(value);
            const std::optional<PXR_NS::VtValue> converted =
                plain ? usdcc::usd::toRendererValue(*plain, setting.defaultValue) : std::nullopt;
            if (!converted) {
                throw py::type_error(key + " takes a " + std::string(usdcc::core::settingTypeName(setting.type)) + ", not " +
                                     py::str(py::type::of(value).attr("__name__")).cast<std::string>());
            }
            viewport.setRendererOverride(pluginId, setting.key, converted);
        },
        py::arg("id"), py::arg("renderer"), py::arg("key"), py::arg("value"));
    // One override dropped, or with a `key` of "", all of the renderer's.
    m.def(
        "reset_viewport_renderer_setting",
        [](int id, const std::string& renderer, const std::string& key) {
            ViewportViewPanel& viewport = viewportWithId(id);
            const PXR_NS::TfToken pluginId = rendererOf(viewport, renderer);
            if (!key.empty()) {
                viewport.setRendererOverride(pluginId, rendererSettingInfo(pluginId, key).key, std::nullopt);
                return;
            }
            ViewportViewPanel::RendererOverrides overrides = viewport.rendererOverrides();
            overrides.erase(pluginId);
            viewport.setRendererOverrides(overrides);
        },
        py::arg("id"), py::arg("renderer"), py::arg("key"));
    // The current renderer's commands, (name, description) each; none before it has drawn.
    m.def(
        "viewport_renderer_commands",
        [](int id) {
            std::vector<std::pair<std::string, std::string>> commands;
            for (const PXR_NS::HdCommandDescriptor& command : viewportWithId(id).hydraWindow()->rendererCommands()) {
                commands.emplace_back(command.commandName.GetString(), command.commandDescription);
            }
            return commands;
        },
        py::arg("id"));
    m.def(
        "run_viewport_renderer_command",
        [](int id, const std::string& name) { return viewportWithId(id).hydraWindow()->runRendererCommand(PXR_NS::TfToken(name)); },
        py::arg("id"), py::arg("name"));

    // App events: returns an id for disconnect_event().
    m.def(
        "connect_event",
        [](const std::string& event, py::object callback) -> int {
            usdcc::usd::StageManager& stages = *context().stageManager;
            auto held = std::make_shared<PythonObject>(std::move(callback));
            if (event == "selection_changed") {
                return keepConnection(stages.selectionChanged,
                                      [held](const StageRefPtr& stage, std::vector<PXR_NS::SdfPath> selection) {
                                          std::vector<std::string> paths;
                                          for (const PXR_NS::SdfPath& path : selection) paths.push_back(path.GetString());
                                          callLogged(held, cacheIdOf(stage), paths);
                                      });
            }
            if (event == "current_stage_changed") {
                return keepConnection(stages.currentStageChanged,
                                      [held](const StageRefPtr& stage) { callLogged(held, cacheIdOf(stage)); });
            }
            if (event == "stage_opened") {
                return keepConnection(stages.stageOpened,
                                      [held](const StageRefPtr& stage) { callLogged(held, cacheIdOf(stage)); });
            }
            if (event == "stage_closed") {
                // It has left the stage cache by now, so the file says which it was.
                return keepConnection(stages.stageClosed, [held](const StageRefPtr& stage) {
                    callLogged(held, stage->usdStage()->GetRootLayer()->GetIdentifier());
                });
            }
            if (event == "stage_edited") {
                // Synchronously, inside USD's change notice: the callback should
                // only note what changed, not edit or export the stage.
                const int id = s_nextEventConnection++;
                s_eventConnections[id] = std::make_shared<AllStagesEdited>(
                    stages, [held](const StageRefPtr& stage, const usdcc::usd::StageChanges& changes) {
                        callLogged(held, cacheIdOf(stage), pathStrings(changes.resyncedPaths),
                                   pathStrings(changes.changedInfoOnlyPaths));
                    });
                return id;
            }
            if (event == "time_changed") {
                const int id = s_nextEventConnection++;
                s_eventConnections[id] = std::make_shared<AllStagesTime>(
                    stages, [held](const StageRefPtr& stage, double time) { callLogged(held, cacheIdOf(stage), time); });
                return id;
            }
            if (event == "physics_changed") {
                const int id = s_nextEventConnection++;
                s_eventConnections[id] = std::make_shared<AllStagesPhysics>(
                    stages, [held](const StageRefPtr& stage) { callLogged(held, cacheIdOf(stage)); });
                return id;
            }
            if (event == "viewport_camera_changed") {
                // usdcc/app.py reads the camera with active_viewport_camera().
                return keepConnection(usdcc::usd::ViewportViewPanel::activeViewportChanged(),
                                      [held]() { callLogged(held); });
            }
            if (event == "viewport_renderer_settings_changed") {
                // The viewport's id: usdcc/app.py makes it a Viewport.
                return keepConnection(usdcc::usd::ViewportViewPanel::anyRendererSettingsChanged(),
                                      [held](usdcc::usd::ViewportViewPanel* viewport) { callLogged(held, viewport->id()); });
            }
            throw py::value_error("no event named '" + event + "'");
        },
        py::arg("event"), py::arg("callback"));
    m.def(
        "disconnect_event", [](int id) { s_eventConnections.erase(id); }, py::arg("id"));

    // The timeline's transport (docs/ANIMATION.md §4.2), on the current stage.
    m.def("play", []() { playback().play(); });
    m.def("stop_playing", []() { playback().stop(); });
    m.def("is_playing", []() { return playback().isPlaying(); });
    m.def("is_simulating", []() { return playback().simulates(); });
    m.def("set_simulating", [](bool on) { playback().setSimulates(on); }, py::arg("on"));

    // A plugin's own settings and what it adds (src/python/usdcc/_plugins.py).
    m.def(
        "plugin_setting",
        [](const std::string& id, const std::string& key) -> py::object {
            const QSettings settings = usdcc::ui::appSettings();
            const QString name = settingsKey(id, key);
            if (!settings.contains(name)) return py::none();
            return py::str(settings.value(name).toString().toStdString());
        },
        py::arg("id"), py::arg("key"));
    m.def(
        "set_plugin_setting",
        [](const std::string& id, const std::string& key, const std::string& value) {
            QSettings settings = usdcc::ui::appSettings();
            settings.setValue(settingsKey(id, key), QString::fromStdString(value));
        },
        py::arg("id"), py::arg("key"), py::arg("value"));
    m.def(
        "remove_plugin_setting",
        [](const std::string& id, const std::string& key) {
            QSettings settings = usdcc::ui::appSettings();
            settings.remove(settingsKey(id, key));
        },
        py::arg("id"), py::arg("key"));
    m.def(
        "plugin_note",
        [](const std::string& id, const std::string& kind, const std::string& name) {
            if (s_context && s_context->plugins) s_context->plugins->addContribution(id, kind, name);
        },
        py::arg("id"), py::arg("kind"), py::arg("name"));

    // Sessions (docs/SESSIONS.md §7); usdcc/app.py wraps these.
    const auto sessions = []() -> SessionHost& {
        if (!s_context || !s_context->sessions) throw std::runtime_error("sessions aren't available here");
        return *s_context->sessions;
    };
    const auto check = [](const std::string& error) {
        if (!error.empty()) throw std::runtime_error(error);
    };
    m.def("sessions", [sessions]() { return sessions().sessionNames(); });
    m.def("current_session", [sessions]() { return sessions().currentSessionName(); });
    m.def(
        "save_session", [sessions, check](const std::string& name) { check(sessions().saveSession(name)); },
        py::arg("name"));
    m.def(
        "load_session",
        [sessions, check](const std::string& name, bool discard) { check(sessions().loadSession(name, discard)); },
        py::arg("name"), py::arg("discard_unsaved") = false);
    m.def(
        "delete_session", [sessions, check](const std::string& name) { check(sessions().deleteSession(name)); },
        py::arg("name"));
    m.def(
        "import_session",
        [sessions, check](const std::string& path, bool replace) {
            std::string name;
            check(sessions().importSession(path, replace, &name));
            return name;
        },
        py::arg("path"), py::arg("replace") = false);
    m.def(
        "export_session",
        [sessions, check](const std::string& name, const std::string& path) { check(sessions().exportSession(name, path)); },
        py::arg("name"), py::arg("path"));

    // Workspaces (docs/WORKSPACES.md §9.1); usdcc/app.py wraps these.
    const auto workspaces = []() -> usdcc::ui::WorkspaceController& {
        if (!s_context || !s_context->workspaces) throw std::runtime_error("workspaces aren't available here");
        return *s_context->workspaces;
    };
    m.def("workspaces", [workspaces]() {
        usdcc::ui::WorkspaceController& controller = workspaces();
        std::vector<std::map<std::string, py::object>> list;
        for (const usdcc::ui::workspace::Definition& definition : controller.library()->workspaces()) {
            QStringList missing;
            usdcc::ui::workspace::pruned(
                definition.layout,
                [&controller](const QString& type) {
                    return controller.context().registry && controller.context().registry->find(type);
                },
                &missing);
            std::vector<std::string> missingTypes;
            for (const QString& type : missing) missingTypes.push_back(type.toStdString());
            list.push_back({{"id", py::str(definition.id.toStdString())},
                            {"name", py::str(definition.name.toStdString())},
                            {"group", py::str(definition.group.toStdString())},
                            {"description", py::str(definition.description.toStdString())},
                            {"file", py::str(definition.file.toStdString())},
                            {"source", py::str(definition.source.toStdString())},
                            {"missing", py::cast(missingTypes)}});
        }
        return list;
    });
    m.def("current_workspace", [workspaces]() { return workspaces().currentId().toStdString(); });
    m.def(
        "set_workspace",
        [workspaces](const std::string& id) {
            usdcc::ui::WorkspaceController& controller = workspaces();
            if (!controller.library()->find(QString::fromStdString(id))) throw py::key_error("no workspace \"" + id + "\"");
            QString error;
            return controller.switchTo(QString::fromStdString(id), &error);
        },
        py::arg("id"));
    m.def(
        "save_workspace",
        [workspaces](const std::string& name, bool sidePanels) {
            QString error;
            const QString id = workspaces().saveCurrentLayout(QString::fromStdString(name), sidePanels, &error);
            if (id.isEmpty()) throw std::runtime_error(error.toStdString());
            return id.toStdString();
        },
        py::arg("name"), py::arg("side_panels") = true);
    m.def("reload_workspaces", [workspaces]() { workspaces().library()->reload(); });
    m.def(
        "fit_all_views",
        [workspaces](bool selection) {
            workspaces().fitAllViews(selection ? QStringLiteral("selection") : QStringLiteral("stage"));
        },
        py::arg("selection") = true);
}

void rebuildScriptedPanels(const std::vector<std::string>& types) {
    if (types.empty() || !s_context || !s_context->mainWindow) return;
    for (ads::CDockWidget* dock : s_context->mainWindow->dockManager()->dockWidgetsMap()) {
        auto* panel = qobject_cast<ScriptedViewPanel*>(dock);
        auto* rendering = qobject_cast<ScriptedRenderViewPanel*>(dock);
        if (!panel && !rendering) continue;
        const std::string type = dock->property(usdcc::ui::ViewPanelRegistry::kTypeProperty).toString().toStdString();
        if (std::find(types.begin(), types.end(), type) == types.end()) continue;
        std::shared_ptr<PythonObject> cls;
        {
            py::gil_scoped_acquire gil;
            try {
                py::object found = py::module_::import("usdcc._plugins").attr("panel_class")(type);
                if (!found.is_none()) cls = std::make_shared<PythonObject>(std::move(found));
            } catch (py::error_already_set& error) {
                logPythonError(error);
            }
        }
        if (panel) {
            panel->rebuild(std::move(cls));
        } else {
            rendering->rebuild(std::move(cls));
        }
    }
}
#else
void rebuildScriptedPanels(const std::vector<std::string>&) {}
#endif  // USDCC_HAS_PYTHON

}  // namespace usdcc::scripting
