#pragma once

namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd

namespace usdcc::tools {
class ToolSet;
}

namespace usdcc::ui {
class MainWindow;
}

namespace usdcc::scripting {

// The running app's objects scripts work on (docs/SCRIPTING.md §4.1).
struct AppContext {
    usdcc::usd::StageManager* stageManager = nullptr;
    usdcc::usd::UndoManager* undoManager = nullptr;
    usdcc::tools::ToolSet* toolSet = nullptr;
    // For usdcc.app.main_window() and add_side_panel(); null without a
    // window, and then those raise.
    usdcc::ui::MainWindow* mainWindow = nullptr;
};

// Makes `context` what the embedded module _usdcc_app — and through it
// usdcc.app, usdcc.usd and usdcc.tools — works on, for as long as this
// exists. Create it once the objects exist and destroy it before they go
// away; afterwards those calls raise RuntimeError instead of touching freed
// objects. A scope made inside another one (the tests' window) is in effect
// until it ends, and then the outer one is again.
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

}  // namespace usdcc::scripting
