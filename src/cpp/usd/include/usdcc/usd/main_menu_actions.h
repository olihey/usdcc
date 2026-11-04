#pragma once

#include <QString>

#include <functional>
#include <string>

namespace usdcc::ui {
class MainWindow;
}

namespace usdcc::usd {

class StageManager;
class UndoManager;

// Runs `run` as one undo step, however many stages it edits, named `label`
// (docs/PLUGINS.md §3.7). `run` returns whether it worked; if not, the step says
// it failed. Inside a global edit it belongs to that edit's step. Either manager
// may be null, and then it just runs.
void runAsOneUndoStep(UndoManager* undo, StageManager* stages, const std::string& label,
                      const std::function<bool()>& run);

// Adds an entry to the main menu. `path` is "Menu/Action" or "Menu/Submenu/Action";
// menus that don't exist yet are made, new top-level ones before Debug. Adding a
// path again replaces its callback. False, with nothing added, for a path with no menu.
bool addMainMenuAction(usdcc::ui::MainWindow& window, const QString& path, std::function<void()> callback,
                       const QString& shortcut = {}, const QString& tip = {});
// False if there's no such entry.
bool removeMainMenuAction(usdcc::ui::MainWindow& window, const QString& path);

}  // namespace usdcc::usd
