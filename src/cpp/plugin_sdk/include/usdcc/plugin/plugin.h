#pragma once

// What a C++ plugin includes (docs/PLUGIN_SDK.md). A plugin defines:
//
//     USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) { ... }
//     USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host& host) { ... }  // optional, at quit
//
// A plugin shares the app's own classes and singletons, so it can use anything in
// their headers directly. Host is the convenient way to add things: these calls
// also tell the Plugin Manager what the plugin added. An exception thrown from
// activate marks the plugin failed; what it registered by then stays until the
// next start, as a plugin's library is never unloaded.

#include "usdcc/core/log.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/main_menu_actions.h"
#include "usdcc/usd/prim_context_menu.h"

#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define USDCC_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define USDCC_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd

namespace usdcc::plugin {

// Lives until the app quits. Null pointers mean the app has none here; the add
// functions needing one throw.
struct Host {
    // From the manifest.
    std::string id;
    std::string name;
    std::string version;
    std::filesystem::path folder;

    usdcc::usd::StageManager* stageManager = nullptr;
    usdcc::usd::UndoManager* undoManager = nullptr;
    usdcc::tools::ToolSet* toolSet = nullptr;
    usdcc::ui::MainWindow* mainWindow = nullptr;
    usdcc::ui::ViewPanelRegistry* viewPanels = nullptr;

    // Tells the Plugin Manager the plugin added something.
    std::function<void(const std::string& kind, const std::string& name)> note;

    void addViewPanel(const QString& panelId, const QString& displayName, usdcc::ui::ViewPanelRegistry::Factory create) {
        require(viewPanels, "view panel types").add(panelId, displayName, std::move(create));
        noted("View panel", displayName.toStdString());
    }

    // Takes ownership. A stable object name is its identity in the saved layout.
    void addSidePanel(usdcc::ui::SidePanel* panel, Qt::DockWidgetArea area) {
        require(mainWindow, "main window").addSidePanel(panel, area);
        noted("Side panel", panel->windowTitle().toStdString());
    }

    // `icon` is the path of an image file; empty, the rail shows the label's first letter.
    void addTool(const std::string& toolId, const std::string& label, const std::string& icon,
                 std::unique_ptr<usdcc::tools::Tool> tool) {
        if (!require(toolSet, "tool set").addTool(toolId, label, icon, std::move(tool))) {
            throw std::invalid_argument("can't add the tool '" + toolId + "'");
        }
        noted("Tool", label);
    }

    // `run` is one undo step, however many stages it edits. An exception is logged.
    void addMenuAction(const QString& path, std::function<void()> run, const QString& shortcut = {},
                       const QString& tip = {}) {
        const std::string label = path.section(QLatin1Char('/'), -1).toStdString();
        auto action = [this, label, run = std::move(run)]() { asOneStep(label, run); };
        if (!usdcc::usd::addMainMenuAction(require(mainWindow, "main window"), path, std::move(action), shortcut, tip)) {
            throw std::invalid_argument("a menu action's path is 'Menu/Action', not '" + path.toStdString() + "'");
        }
        noted("Menu action", path.toStdString());
    }

    // An entry at the end of the Outliner's prim menu ("Group/Action" makes a
    // submenu). `run` is one undo step. `applies` may be empty.
    void addPrimMenuAction(const QString& label, std::function<void(const usdcc::usd::PrimMenuContext&)> run,
                           std::function<bool(const usdcc::usd::PrimMenuContext&)> applies = {}) {
        usdcc::usd::PrimMenuEntry entry;
        entry.id = id + "." + label.toStdString();
        entry.label = label;
        const std::string step = label.section(QLatin1Char('/'), -1).toStdString();
        entry.run = [this, step, run = std::move(run)](const usdcc::usd::PrimMenuContext& context) {
            asOneStep(step, [&run, &context]() { run(context); });
        };
        entry.applies = std::move(applies);
        usdcc::usd::PrimMenuEntries::instance().add(std::move(entry));
        noted("Prim menu action", label.toStdString());
    }

    // A key of the plugin's own in usdcc::ui::appSettings().
    QString settingKey(const QString& key) const {
        return QStringLiteral("PluginSettings/%1/%2").arg(QString::fromStdString(id), key);
    }

    // A record with the source plugin:<id> in the Log panel.
    void log(usdcc::core::LogLevel level, const std::string& message) const {
        usdcc::core::LogRecord record;
        record.level = level;
        record.source = "plugin:" + id;
        record.category = "plugin." + id;
        record.message = message;
        usdcc::core::LogHub::instance().post(std::move(record));
    }

private:
    template <typename T>
    static T& require(T* object, const char* what) {
        if (!object) throw std::runtime_error(std::string("the app has no ") + what + " here");
        return *object;
    }

    void noted(const std::string& kind, const std::string& added) const {
        if (note) note(kind, added);
    }

    void asOneStep(const std::string& label, const std::function<void()>& run) const {
        usdcc::usd::runAsOneUndoStep(undoManager, stageManager, label, [&]() {
            try {
                run();
                return true;
            } catch (const std::exception& error) {
                log(usdcc::core::LogLevel::Error, label + " failed: " + error.what());
            } catch (...) {
                log(usdcc::core::LogLevel::Error, label + " failed");
            }
            return false;
        });
    }
};

using ActivateFunction = void (*)(Host&);
using DeactivateFunction = void (*)(Host&);

}  // namespace usdcc::plugin
