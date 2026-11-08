#pragma once

#include <string>
#include <vector>

namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd

namespace usdcc::tools {
class ToolSet;
}

namespace usdcc::ui {
class MainWindow;
class ViewPanelRegistry;
}  // namespace usdcc::ui

namespace usdcc::scripting {

class PluginManager;

// Sessions, for usdcc.app.save_session() and the rest (docs/SESSIONS.md §7).
// SessionController implements it. Each returns an error, empty when it went well.
class SessionHost {
public:
    virtual ~SessionHost() = default;
    virtual std::vector<std::string> sessionNames() const = 0;
    virtual std::string currentSessionName() const = 0;
    virtual std::string saveSession(const std::string& name) = 0;
    // Without asking: refuses while layers have unsaved edits unless `discardUnsaved`,
    // and when the session needs other plugins (a restart).
    virtual std::string loadSession(const std::string& nameOrPath, bool discardUnsaved) = 0;
    virtual std::string deleteSession(const std::string& name) = 0;
    virtual std::string importSession(const std::string& path, bool replace, std::string* name) = 0;
    virtual std::string exportSession(const std::string& name, const std::string& path) = 0;
};

// The running app's objects scripts work on (docs/SCRIPTING.md §4.1).
struct AppContext {
    usdcc::usd::StageManager* stageManager = nullptr;
    usdcc::usd::UndoManager* undoManager = nullptr;
    usdcc::tools::ToolSet* toolSet = nullptr;
    // For usdcc.app.main_window() and add_side_panel(); null without a
    // window, and then those raise.
    usdcc::ui::MainWindow* mainWindow = nullptr;
    // The view panel types (the Stages menu's), for register_view_panel();
    // open_view_panel() needs it and the window.
    usdcc::ui::ViewPanelRegistry* viewPanels = nullptr;
    // Told what each plugin adds; may be null.
    PluginManager* plugins = nullptr;
    // Saves and loads sessions; null without, and then those raise.
    SessionHost* sessions = nullptr;
};

// Makes `context` what the embedded module _usdcc_app — and through it
// usdcc.app, usdcc.usd and usdcc.tools — works on, for as long as this
// exists. Create it once the objects exist and destroy it before they go
// away; afterwards those calls raise RuntimeError instead of touching freed
// objects. A scope made inside another one (the tests' window) is in effect
// until it ends, and then the outer one is again. Event connections and prim
// menu entries made from Python end with a scope, while Python is still there.
class AppContextScope {
public:
    explicit AppContextScope(const AppContext& context);
    ~AppContextScope();
    AppContextScope(const AppContextScope&) = delete;
    AppContextScope& operator=(const AppContextScope&) = delete;

private:
    AppContext m_context;
    const AppContext* m_outer = nullptr;
};

// The context in effect, or null.
const AppContext* currentAppContext();

// Rebuilds the open panels of these Python view panel types from what's registered
// now, after their plugin reloaded (docs/PLUGIN_DEV.md).
void rebuildScriptedPanels(const std::vector<std::string>& types);

}  // namespace usdcc::scripting
