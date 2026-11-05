// pybind11 (and with it Python.h) first: Python.h must precede the standard
// headers.
#if USDCC_HAS_PYTHON
#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "python_object.h"
#include "usdcc/scripting/scripted_tool.h"
#include "usdcc/scripting/scripted_view_panel.h"
#include "usdcc/usd/python/bind_usd.h"
#endif

#include "usdcc/scripting/app_module.h"

#include "usdcc/core/settings.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/toast_manager.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/main_menu_actions.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QSettings>
#include <QUndoStack>

#include <DockManager.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::scripting {

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
            return py::none();
        },
        py::arg("id"), py::arg("cache_id"), "Opens a panel of the type for the stage, like the Stages menu.");
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
            return info;
        },
        py::arg("key"));
    m.def(
        "settings_define",
        [](const std::string& key, const std::string& type, const SettingValue& defaultValue, const std::string& label,
           const std::string& category, const std::string& group, const std::string& description,
           std::optional<double> minimum, std::optional<double> maximum, std::vector<std::string> choices) {
            SettingDefinition definition;
            definition.key = key;
            if (!usdcc::core::parseSettingType(type, &definition.type)) {
                throw py::value_error("unknown setting type '" + type +
                                      "' (bool, int, float, string, choice, color or shortcut)");
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
            if (!Settings::instance().define(std::move(definition))) {
                throw py::value_error("invalid setting '" + key +
                                      "': it needs a key and a category, and a default that fits its type");
            }
        },
        py::arg("key"), py::arg("type"), py::arg("default"), py::arg("label") = std::string(),
        py::arg("category") = std::string("Python"), py::arg("group") = std::string(),
        py::arg("description") = std::string(), py::arg("minimum") = py::none(), py::arg("maximum") = py::none(),
        py::arg("choices") = std::vector<std::string>());

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
            const auto paths = [](const usdcc::usd::PrimMenuContext& menu) {
                std::vector<std::string> list;
                for (const PXR_NS::SdfPath& path : menu.paths) list.push_back(path.GetString());
                return list;
            };
            auto held = std::make_shared<PythonObject>(std::move(callback));
            const std::string description = label.substr(label.rfind('/') + 1);
            entry.run = [held, paths, description](const usdcc::usd::PrimMenuContext& menu) {
                runAsOneStep(description, [&]() { held->object(cacheIdOf(menu.stage), paths(menu), menu.clicked.GetString()); });
            };
            if (!applies.is_none()) {
                auto heldApplies = std::make_shared<PythonObject>(std::move(applies));
                entry.applies = [heldApplies, paths](const usdcc::usd::PrimMenuContext& menu) {
                    py::gil_scoped_acquire gil;
                    try {
                        return heldApplies->object(cacheIdOf(menu.stage), paths(menu), menu.clicked.GetString()).cast<bool>();
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
            throw py::value_error("no event named '" + event + "'");
        },
        py::arg("event"), py::arg("callback"));
    m.def(
        "disconnect_event", [](int id) { s_eventConnections.erase(id); }, py::arg("id"));

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
}
#endif  // USDCC_HAS_PYTHON

}  // namespace usdcc::scripting
