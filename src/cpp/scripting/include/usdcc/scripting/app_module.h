#pragma once

namespace usdcc::usd {
class StageManager;
class UndoManager;
}  // namespace usdcc::usd

namespace usdcc::tools {
class ToolSet;
}

namespace usdcc::scripting {

// The running app's objects scripts work on (docs/SCRIPTING.md §4.1).
struct AppContext {
    usdcc::usd::StageManager* stageManager = nullptr;
    usdcc::usd::UndoManager* undoManager = nullptr;
    usdcc::tools::ToolSet* toolSet = nullptr;
};

// Makes `context` what the embedded module _usdcc_app — and through it
// usdcc.app, usdcc.usd and usdcc.tools — works on, for as long as this
// exists. Create it once the objects exist and destroy it before they go
// away; afterwards those calls raise RuntimeError instead of touching freed
// objects. One at a time.
class AppContextScope {
public:
    explicit AppContextScope(const AppContext& context);
    ~AppContextScope();
    AppContextScope(const AppContextScope&) = delete;
    AppContextScope& operator=(const AppContextScope&) = delete;
};

// The context in effect, or null.
const AppContext* currentAppContext();

}  // namespace usdcc::scripting
