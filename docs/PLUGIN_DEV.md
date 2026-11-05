# Developing Plugins in the App

Write a Python plugin without leaving the app, and see a change in one click rather than a restart. §6 says what has and hasn't been tested.

```text
 Script Editor
┌───────────────────────────────────────────────────────────────────┐
│ ☰  build_usd.py   Untitled   [Notes]                        ⟳ ▶ 📁 💾 │  top tabs: files, and plugins
├──────────────┬────────────────────────────────────────────────────┤
│ ▾ python     │ __init__.py  panel.py •  usdcc-plugin.json         │  this plugin's own tabs
│   ▾ notes    ├────────────────────────────────────────────────────┤
│     __init__ │ def activate(plugin):                              │
│     panel    │     usdcc.app.register_view_panel(...)             │
│ usdcc-plugin │                                                    │
├──────────────┴────────────────────────────────────────────────────┤
│ >>> Reload Notes                                                  │  the output, shared by all tabs
│     done in 12 ms                                                 │
└───────────────────────────────────────────────────────────────────┘
```

## 1. Using it

**To start a plugin:** **New Plugin...** in View > Plugins asks for a name, writes a folder in the user's plugins folder with a manifest and an `__init__.py` to start from, runs it with no restart, and opens it in the Script Editor.

**To work on one:** right-click it in View > Plugins:

| Menu entry | What it does |
|---|---|
| **Edit in Script Editor** | Opens the plugin's tab, or goes to the one that's open |
| **Reload** | Saves its open files, and puts the changes into effect. Greyed out, saying why, for a plugin with a C++ part, with no Python, or turned off |
| **Open Folder** | The plugin's folder in Explorer |

**The tabs tell you what you're in:** a plugin's has the same `Py` chip as View > Plugins, and a Python file or untitled script has a page icon.

**In the plugin's tab:**

- **The tree** shows the plugin's folder, except `__pycache__` and `.pyc`, opened down to the code. `deps/` is there too, closed.
- **A click opens a preview,** marked `◦` on its tab, which the next click replaces. Double-clicking, or typing in it, keeps it. A binary file, or one over 5 MB, isn't opened.
- **The tree's right-click menu** has New File, New Folder, Rename, Delete (to the Recycle Bin), and Show in Explorer. Open tabs follow a rename.
- **The main toolbar button, `Ctrl+Enter` and `F5` are Reload.** Run has nothing useful to do there, as running `__init__.py` as a script fails on its relative imports. Run Selection still works, for trying a line.
- **The session keeps it:** its open files, unsaved text and the tree's width come back at the next start.

**In every tab:** a file changing on disk updates the tab, asking first if the tab has unsaved changes.

From a script, `usdcc.app.reload_plugin(id)` reloads once what's running has finished, so a plugin's own menu action can ask for it.

## 2. What a reload does

`PluginManager::reload()`, in order:

1. Calls the plugin's `deactivate(plugin)`, and those of plugins that require it, last first. **Plugins requiring it go with it,** as they may hold its classes.
2. Takes back what they registered.
3. Forgets their modules, and removes `.pyc` files under `python/`. A `.pyc` is matched to its source by size and a time in whole seconds, so a quick edit of the same length would otherwise go on running the old code.
4. Reads the manifest again, and checks its packages as at a start, so a new `requires` shows **Needs packages**.
5. Activates them, and reselects the tool that was active.
6. Rebuilds open panels of their view panel types where they stand, keeping the dock, tab and stage.

**Why not just activate again?** *Tested:* menu entries pile up, submodules stay stale with no error, and old callbacks keep firing beside the new ones.

| Extension point | Taken back by |
|---|---|
| Menu actions | `remove_menu_action()`. A menu made for them goes when empty |
| Prim menu actions | `remove_prim_menu_action()` |
| Tools | `unregister_tool()`. An active one falls back to Select, then is reselected |
| App events | `Connection.disconnect()`. A panel connecting long after `activate()` is covered, as a callback's `__module__` says whose it is |
| Create entries, relationship rules | `remove_create_entry()`, `remove_relationship_targets()` |
| View panel types | `unregister_view_panel()`. An open panel of a type that's gone says so, and comes back if the type does |
| Side panels | `MainWindow::removeSidePanel()`. A new one with the same `id` goes back where it was |
| `usdcc.settings.define()` | Nothing. A row for a removed setting stays until a restart |

`plugin.settings` values are kept, and what an open panel held in memory isn't.

**When it goes wrong:** a syntax error or a raise in `activate()` shows **Failed** with the reason, and the traceback is in the output. What a half-finished `activate()` registered goes at the next reload, so fixing the mistake and reloading works from any state but a crash. A crash during a reload is caught by the same marker as at a start, so the plugin is off at the next one.

## 3. What a reload can't do

**It can't see what a plugin did directly, not through `usdcc`:** PySide signal connections and `QTimer`s, threads, `Tf.Notice` listeners, patches to other modules, and widgets it added by hand. These keep running the old code, so **`deactivate(plugin)` is the place to stop them.** It now runs before every reload as well as at quit. The output says when a plugin has none. A restart remains the answer if things get odd.

**Python only.** A plugin with a C++ part can't reload, and a C++ plugin requiring a reloaded one goes on using what it had. A change to `usd` in the manifest takes effect at the next start, and the output says so.

**Third-party packages aren't reloaded.** Only modules from the plugin's own `python/` folder are forgotten. `deps/` and the shared packages folder stay, as a compiled package can't load twice.

## 4. Left out

- **Completion and go-to-definition.** External editors already do this, and **Open Folder** gets you there.
- **Clicking a traceback line to go to it, and Reload on Save.** Both were offered, and neither is built.
- **Keeping a panel's state across a reload.**

## 5. Where the code is

- **`src/python/usdcc/_plugins.py`:** `track()`, `unload()`, forgetting modules, and who a callback belongs to.
- **`usdcc/app.py`, `tools.py`, `usd.py`:** each registering call records its undo.
- **`scripting/plugin_manager.h`:** `reload()`, `whyNotReloadable()`, `createPlugin()`.
- **`scripting/scripted_view_panel.h`:** `rebuild()`. **`scripting/app_module.h`:** `rebuildScriptedPanels()`.
- **`ui/main_window.h`:** `removeSidePanel()`. **`ui/view_panel_registry.h`:** `remove()`.
- **`scripting/script_editor_side_panel.h`:** plugin tabs, `openPlugin()`, `reloadPlugin()`, watching files.
- **`scripting/plugin_manager_side_panel.h`:** the right-click menu and New Plugin. `main.cpp` connects the two panels.
- **`tests/plugin_reload_test.cpp`, `tests/plugin_tab_test.cpp`.**

## 6. Tested, and not

**Run in a deployed app, by a script driving the real widgets:** the right-click menu and its Edit and Reload; the plugin tab, looked at in screenshots; real mouse clicks in the tree making and replacing a preview; the Reload button saving first, then the new code, menu action and open panel; a syntax error showing Failed with its traceback, and fixing it; **New Plugin** through its button and dialog, running with no restart. Nothing has been clicked by hand.

**By the tests:** menu actions, tools, app events, view panel types and side panels from §2's table, though not prim menu actions, Create entries or relationship rules, which take the same path; submodules, same-length edits, broken and half-finished plugins, dependents, a changed manifest; the tree, previews, saving before a reload, the session, tree file actions, disk changes, and New Plugin.

**Not tested:**

- **Reloading a real plugin of any size.** A plugin importing something heavy for the first time pays for that once.
- **Reload with a tool's drag in progress.**
- **The undo stack across a reload.** I expect steps to survive, as they're layer edits, but no test has undone a plugin's edit after reloading it.
- **A rebuilt panel whose old code still holds things.** The tests' panels are labels and a connection.
- **`QFileSystemModel` on a very large `deps/`,** though it reads only what's expanded.
- **A side panel floating, or tabbed with another panel,** when it's put back.
- **Two usdcc instances editing the same plugin.** They'd overwrite each other's files, as two editors would, and the Script Editor's session is already last-one-wins.
