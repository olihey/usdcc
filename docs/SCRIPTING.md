# Scripting — Plan

*Status: all five phases done (see §12). Milestone M7 in [PLAN.md](PLAN.md); also covers logging phase 5
([LOGGING.md](LOGGING.md) §3.4). Last updated: 2026-11-02.*

## 1. Goal

Users can script the running application in Python:

- A **Script Editor side panel**: an editor with Python syntax highlighting, load/save, Run and
  Run Selection, and the run's output.
- An **embedded interpreter** in which `pxr` (OpenUSD's own Python API) and `usdcc.*` work
  together on the app's live state: its open stages, selection, undo history and tools.
- **Script edits are undoable** exactly like UI edits: one Run is one undo step.
- **Errors never take the app down**: exceptions and their tracebacks go to the output pane
  and the Log panel. Only a crash in native code can still end the process.
- **Startup scripts** and a `--script` command-line option, the base M9's plugins build on.

**Exit criterion (PLAN.md):** a script run from the panel can open a stage, edit a prim, and
drive the undo stack identically to a UI-driven edit.

### Where we start from

These facts constrain the plan (checked in this checkout, 2026-11-02):

- **Two Python 3.12 builds are mixed today.** OpenUSD was built against the system Python
  (`C:\Program Files\Python312`, 3.12.10; `Python3_EXECUTABLE` in `pxrConfig.cmake`), and its
  `pxr` package is in `<USD_INSTALL>/lib/site-packages`. But vcpkg's deploy step copies
  **vcpkg's own** `python312.dll` (3.12.13, from the vcpkg `python3` port pybind11 pulls in)
  next to `usdcc.exe`, so the app already runs OpenUSD's C++ on a different Python DLL. It
  works because nothing calls into Python yet. An embedded interpreter must use one Python
  with its own standard library, and that must be the one OpenUSD's `pxr` modules were
  built for.
- **`usd.pyd` (the standalone `usdcc.usd` module) was built for Python 3.11.** Its CMake finds
  "the first `python` on PATH" at configure time, and the cache still holds a 3.11 path from
  an earlier configure. It can't be imported next to `pxr` in either interpreter.
- **PySide6/Shiboken6 is not built here**, so the Shiboken `usdcc.ui` module (only
  `SidePanel`) is skipped by the build. `ViewPanel` isn't bound at all (PLAN.md §6 item 22).
  Python access to Qt widgets therefore depends on `tools/build-pyside.ps1` and can't be part
  of M7's core.
- **Every usdcc library except `usdcc_core` is static.** `usd.pyd` links its own copy of
  `usdcc_usd`, with its own `StageManager` class, so it can't reach the app's
  `StageManager` instance. Inside the app, scripts need bindings compiled into the exe.
- **Stages cross into Python as cache ids.** Every stage is in `UsdUtilsStageCache`, and
  Python gets the real `Usd.Stage` through OpenUSD's own bindings (PLAN.md §5 item 11). That
  stays: nothing passes a `UsdStageRefPtr` through pybind11.
- **Undo works per stage, by recorded layer changes.** `UndoManager::beginEdit/endEdit` record
  the changes to one stage's edit-target layer (PLAN.md §6 item 36) and nest by depth. A script may touch several stages, including
  one it opens itself.
- **OpenUSD calls into Python from its own threads** (Python notice listeners and Python
  plugins, guarded by `TfPyLock`). A main thread that keeps the GIL would deadlock them.
- **The log is ready for Python** (LOGGING.md §3.4): one hub in `usdcc_core.dll`; native
  stdout/stderr is already captured, so even `print()` reaches the log today, as source
  `stdout`.

## 2. Architecture

| Target | Contents |
|---|---|
| `usdcc_scripting` (new, static, Qt-free) | `PythonRuntime` (interpreter start/stop, paths, GIL), the embedded `_usdcc_log` (§5) |
| `usdcc_scripting_app` (new, static, Qt) | The embedded `_usdcc_app` module (§4.1), `AppContextScope`, `ScriptRunner` (one undo step per run, §4.2) |
| `usdcc_usd_bindings` (new, static) | The `StageManager` binding code, `bindStageManager(py::module_&)`, shared by `_usd.pyd` and `_usdcc_app` |
| `usdcc_scripting_ui` (new, static, Qt) | `ScriptEditorSidePanel`, `PythonHighlighter`, `CodeEditor` (line numbers, indentation) |
| `src/python/usdcc/` | Pure-Python parts: `log.py` (logging bridge), `app.py` helpers, `usd.py` conveniences (`current_stage()`, `edit()`) |

C++ lives under `src/cpp/scripting/` (namespace `usdcc::scripting`). The Python package keeps
mirroring the C++ namespaces (`usdcc.core`, `usdcc.usd`, `usdcc.tools`, `usdcc.ui`), plus
`usdcc.app` for the running application, which has no C++ namespace of its own.

## 3. The interpreter

### 3.1 One Python

The app embeds **the Python OpenUSD was built with**: `Python3_EXECUTABLE` from
`pxrConfig.cmake`, the same one `find_package(USD)` already links into the exe.

- **Build**: after vcpkg's deploy step, copy that Python's `python3XY.dll` (and
  `python3.dll`) over vcpkg's in the exe and test folders. Its prefix becomes a
  compile-time default, `USDCC_PYTHON_HOME`.
- **`usd.pyd`**: built against the same Python, read from `pxrConfig.cmake` instead of a
  PATH lookup. This also fixes the stale 3.11 build.
- **At start-up**: `PyConfig.home` = `USDCC_PYTHON_HOME` (overridable with the environment
  variable `USDCC_PYTHONHOME`). `sys.path` gets, in order, the `usdcc` package folder
  (`build/python`), OpenUSD's `lib/site-packages`, and the user's own scripts folder (§6).
  `PYTHONPATH` is still honoured, and so is the Python's own `site-packages`, so a user can
  `import numpy`.
- **Deployed**, the app uses a Python bundled next to it instead: `..\python` from the
  executable, with usdcc, pxr and PySide6 in its `Lib\site-packages` (tools/deploy.ps1;
  PLAN.md §6 item 37).
  Nothing else changes.

A failure here (no Python found, `import pxr` fails) is logged as an Error with the reason. The
app keeps running, and the Script Editor panel says scripting is unavailable and why.

### 3.2 Life cycle and the GIL

`PythonRuntime` is created in `main()` after `QApplication` and the log, and before the window:

1. `Py_InitializeFromConfig`, then install the log bridge (`usdcc.log.install()`, §5) before
   any other Python runs, then `import pxr` as a check.
2. **Release the GIL** (`PyEval_SaveThread`). The main thread holds it only while it runs
   Python, so OpenUSD's own threads can take it with `TfPyLock`.
3. Every run takes the GIL with `py::gil_scoped_acquire` and gives it back afterwards.
4. At exit, before the window and `StageManager` go away, the runtime takes the GIL back,
   runs the `atexit` handlers and finalizes. Python's references to stages (through the stage
   cache) are released while the C++ side still exists.

`Py_FinalizeEx` with OpenUSD's boost::python modules loaded is a known risk. If it proves
unreliable, the fallback (§11 #4) is to run the `atexit` handlers and skip finalization.

### 3.3 Where scripts run

On the **main thread**, blocking the UI while they run. USD stages and Qt widgets aren't safe to
change from another thread while Hydra and the panels read them, and almost all scripts are
short. The panel shows a busy cursor and "Running…". There's no Stop in M7 (§10).

## 4. The Python API

### 4.1 Modules inside the app

The modules users import are **pure Python** (`src/python/usdcc`); each picks its native part
when imported:

- **Inside the app**: the embedded module `_usdcc_app`, compiled into the exe, working on the
  app's own objects (`AppContextScope`).
- **In a plain python**: `usdcc/_usd.pyd`, with a `StageManager` of its own. It was `usd.pyd`
  before; renamed so it can't shadow `usd.py`.

No `sys.modules` patching is needed, and `import usdcc.usd` works the same in both places.

| Module | Contents |
|---|---|
| `usdcc.usd` | `StageManager` (and `UndoManager` in the app); `stages()`, `current_stage()`, `set_current_stage()`, `open_stage(path)`, `close_stage()`, all as `pxr.Usd.Stage`; `stage_id(stage)`; `selected_paths()`, `select(prims/paths)`; `edit(description)`, `undo()`, `redo()`, `can_undo()`, `can_redo()`, `undo_text()`; `EMBEDDED` |
| `usdcc.app` | `is_embedded()`, `stage_manager()`, `undo_manager()`; with PySide6, `main_window()`, `add_side_panel(panel, area)`, `register_view_panel(id, name, cls)`, `open_view_panel(id, stage=None)`, `view_panel_types()` (§8) |
| `usdcc.core` | `log(level, category, message)`, the level constants, `set_levels(spec)` (USDCC_LOG's format) |
| `usdcc.tools` | `tool_names()`, `active_tool()`, `set_active_tool(name)` |
| `usdcc.log` | The logging bridge (§5) |
| `usdcc.ui` | `SidePanel` and `ViewPanel`, the base classes for panels made in Python (§8; needs PySide6) |

- **Stage functions** default to the current stage and raise `RuntimeError` outside the app.
  The current stage is the stage of the view panel last focused (docs/CURRENT_STAGE.md), so a
  script run from the Script Editor works on the stage you were just looking at;
  `set_current_stage()` changes it until another panel is focused.
- **Prims, `Sdf.Path`s or strings** are all accepted where paths are.
- **`open_stage()` behaves like File > Open Stage**: a file that's already open is opened again
  as a second stage (`UsdStage::Open` never reuses one). To work on an open stage, use
  `stages()`/`current_stage()`.
- **In a plain python**, `usdcc.usd` imports `_usd.pyd` inside OpenUSD's own
  `Tf.WindowsImportWrapper`, which adds the folders on PATH as DLL folders.

Stages stay cache ids at the binding level; the `Usd.Stage` helpers live in `src/python/usdcc`
and use `UsdUtils.StageCache`. A script works with `pxr` directly:

```python
from pxr import Sdf
import usdcc.usd

stage = usdcc.usd.open_stage("C:/shots/a.usda")        # opens it in the app, like File > Open
cube = stage.DefinePrim("/World/Cube", "Cube")
cube.GetAttribute("size").Set(2.0)
usdcc.usd.select([cube])                                # the Outliner and Viewport follow
```

### 4.2 Undo

The rule: **one Run of a script is one undo step per stage it changed**, named after the script
("Run shot_setup.py").

- `UndoManager` gains a *global* edit: `beginGlobalEdit(description)` begins an edit on every
  open stage and on each stage opened before `endGlobalEdit()`. `endGlobalEdit()` ends them
  all, pushing a step only on the stages that changed.
- `ScriptRunner` wraps every Run in one global edit. Edits the script makes inside with
  `usdcc.usd.edit(...)` nest into it, as nested UI edits already do.
- Scripts that aren't run from the panel (startup scripts, later plugins) get no automatic
  step. They use `with usdcc.usd.edit("Description"):` (current stage, or `stage=`) or call
  `undo_manager.begin_edit/end_edit`.
- `usdcc.usd.undo()`, `redo()`, `can_undo()`, `can_redo()` drive the current stage's stack,
  the same as Edit > Undo/Redo.

Exit-criterion check: a script opens a stage, defines a prim, sets an attribute. Edit > Undo
removes the prim and restores the attribute in one step, and Redo brings both back, exactly
like the same edits made in the UI.

### 4.3 Errors

- A Python exception ends the run. The traceback goes to the output pane (red) and to the log
  as an Error, category `python.exception`, with the traceback in `details`.
- What the script changed before the exception stays, as the run's undo step ("Run x.py
  (failed)"), so one Undo reverts it (§11 #3).
- C++ exceptions thrown through pybind11 arrive as Python exceptions. OpenUSD errors in `pxr`
  calls arrive as `Tf.ErrorException`, as in any Python.
- `SystemExit` (`exit()` in a script) ends the run, not the app.

## 5. Logging (LOGGING.md phase 5)

Lands with the runtime, since every later step needs it to see what went wrong:

- An embedded module, `usdcc._log`, posts records to the hub. `usdcc/log.py` installs, at
  start-up:
  - a `logging` handler on the root logger (logger name → category);
  - `captureWarnings`;
  - `sys.stdout`/`sys.stderr` writers (source `python`, category `python.stdout`/
    `python.stderr`, line-buffered);
  - `sys.excepthook` and `threading.excepthook`.
- With these, `print()` from a script shows as source `python`, not as the native `stdout`
  capture.
- **During a run**, `ScriptRunner` also tees the script's output into the panel's output pane.

## 6. Startup scripts and the command line

- Every `*.py` in `%LOCALAPPDATA%/usdcc/scripts/startup/` (`AppLocalDataLocation`) runs once
  the window is up and its layout has settled (`MainWindow::layoutSettled()`, §12 phase 5),
  in name order (ignoring case), each in its own namespace. A failing one is
  logged and the rest still run. They get no undo step of their own (§4.2). That folder's
  parent, `scripts/`, is at the end of `sys.path` for shared modules, so they can't shadow
  `usdcc` or `pxr`. The app creates `startup/` if it's missing; ☰ > *Open Startup Scripts
  Folder* in the Script Editor panel opens it.
- `usdcc.exe [--script file.py]... [--no-startup-scripts] [stage]`: each `--script` runs after
  the startup scripts, in order, as one undo step like Debug > Run Python File. Its output is
  in the log, not in the panel. `--no-startup-scripts` skips the startup folder (it's still
  on `sys.path`). `--help` lists the options.
- **M9** builds plugins on this: a Python plugin is a package with a manifest, loaded by the
  same runner.

## 7. The Script Editor panel

`ScriptEditorSidePanel` (a `SidePanel`, objectName "Script Editor", docked on the right until moved;
added through `MainWindow::addSidePanel()`):

```
[☰] shot_setup.py • [x] │ Untitled [x] │                       [▶] [▶|] [📂] [💾]
┌──────────────────────────────────────────────────────────────────────┐
│ 1  from pxr import Sdf                                                │
│ 2  import usdcc.usd                                                   │
│ 3  stage = usdcc.usd.current_stage()                                  │
├──────────────────────────────────────────────────────────────────────┤
│ >>> Run shot_setup.py                                                 │
│ Created 3 prims                                                       │
│     done in 12 ms, 1 undo step                                        │
└──────────────────────────────────────────────────────────────────────┘
```

- **Editor**: a `QPlainTextEdit` with a line-number gutter, monospace font, 4-space tabs, keeping
  the indentation on Enter (and indenting after `:`), Tab/Shift+Tab on a selection, and
  Ctrl+/ to toggle comments.
- **Highlighting** (`PythonHighlighter`): keywords, builtins, strings (including triple-quoted
  across lines), comments, numbers, decorators, and `def`/`class` names, in theme colors.
- **Run** (Ctrl+Enter) runs the current tab's text (as it is, saved or not). **Run Selection**
  (Ctrl+Shift+Enter) runs the selected lines, or the cursor's line, without their common
  indentation.
- **Namespaces**: each run gets a fresh namespace. Tracebacks name the open file, but
  `__file__` isn't set, since the text may differ from the file. ☰ > *Keep Variables Between
  Runs* uses one namespace for the session instead, and ☰ > *Reset Variables* clears it.
- **Output pane** below the editor: each run's header, its print output, stderr and warnings
  (amber), tracebacks and errors (red), any OpenUSD messages during the run, and a footer with
  the time and undo steps. ☰ > Clear Output empties it. Everything also goes to the Log panel.
- **Tabs**: one script per tab, a file or an untitled script ("Untitled", "Untitled 2", …), in
  the toolbar row between ☰ and the buttons, styled like a ViewPanel's tab strip: the current tab is filled with the accent blue,
  solid while focus is in the panel. "•" marks unsaved changes; the tooltip is the path. Tabs
  move by dragging. New tab: ☰ > New Tab, Ctrl+T, or a double-click beside the tabs; Ctrl+W
  closes one. Run, Save and the toolbar act on the current tab.
- **A tab's context menu**: Save, Save As…, Rename… (an untitled tab's name; a file is renamed
  on disk, adding .py without a suffix), Duplicate (an untitled copy beside it), Clear (one undo
  step), Ruff Format (replaces the text with `ruff format`'s, one undo step), Ruff Check
  (`ruff check`'s findings in the output pane), Ruff Fix (replaces it with `ruff check --fix`'s,
  one undo step, and lists what it couldn't fix), Close and Close Others. Ruff is bundled at a
  pinned version (`cmake/modules/UsdccRuff.cmake` downloads PyPI's Windows wheel at configure
  time, checks its SHA-256 and unpacks `ruff.exe` and its LICENSE into `build/ruff`;
  `tools/deploy.ps1` copies them into the deploy's `tools/ruff`). The app uses the deploy's,
  then the build's, then one on PATH or in the Python's Scripts folder; without any, both
  entries are disabled and say why. Ruff gets the file's path as `--stdin-filename`, so the
  folder's `pyproject.toml`/`ruff.toml` settings apply. `-DUSDCC_BUNDLE_RUFF=OFF` skips the
  download.
- **Files** in the ☰ menu: New Tab, Open…, Open Recent ▸ (ten), Save, Save As…, plus Ctrl+O and
  Ctrl+S. Open uses a new tab (or the file's tab, if it is open; or an empty untitled current
  tab).
- **Unsaved changes**: closing a tab with any asks Save/Discard/Cancel. Closing the app asks the
  same for each *file* with unsaved changes (`SidePanel::canClose()`, from
  `MainWindow::closeEvent()`; Cancel keeps the app open, Discard goes back to the file). Untitled
  scripts are never asked about: their text is kept and restored on the next start.
- **Persistence** (`ScriptEditor/…` in the app settings): the tabs in order (files by path, with
  their text only if they still have unsaved changes; untitled scripts with their name and text),
  the current tab, recent files, the split and the variables option. The tabs are saved half a
  second after each change (typing, adding, closing, moving, renaming), not only on exit, so a
  killed or crashed app still brings them back.
- **Toolbar**: the shared `PanelMenuButton` and `PanelIconButton`s, with new theme icons for
  play, play-selection and save; Open reuses the folder icon.
- **When scripting is unavailable** (§3.1), the panel shows the reason instead of the editor.

## 8. Python-made UI (needs PySide)

Only once `tools/build-pyside.ps1` has run, and CMake then finds it in `USDCC_PYSIDE_DIR`
(default `external/pyside-install`) and builds `usdcc.ui`:

- `usdcc.app.main_window()` returns the main window as a PySide6 `QMainWindow`, through
  `shiboken6.wrapInstance`. Scripts can add menus, actions and dialogs with it.
- A Python `usdcc.ui.SidePanel` subclass is docked with `usdcc.app.add_side_panel(panel,
  area="right")`. It appears in the View menu, and the window takes ownership of it.
  `MainWindow::addSidePanel()` handles panels added after start-up:
  - the panel goes back where the saved layout last had it (`QMainWindow::restoreDockWidget`,
    by its object name, which is the title it was made with), or to `area` if the layout
    doesn't know it;
  - it's added to the View menu above the toolbars;
  - it leaves the menu when it's deleted.
- PySide is built for the same Python as §3.1 (OpenUSD's, which `build-pyside.ps1` reads from
  usdcc's CMake cache) and against vcpkg's Qt. Its folder is on the embedded `sys.path`.

```python
# A startup script: %LOCALAPPDATA%/usdcc/scripts/startup/notes_panel.py
from PySide6 import QtWidgets
import usdcc.app, usdcc.ui

class NotesPanel(usdcc.ui.SidePanel):
    def __init__(self):
        super().__init__("Notes")
        self.setWidget(QtWidgets.QPlainTextEdit())

usdcc.app.add_side_panel(NotesPanel(), "left")
```

**View panels** (milestone M1), the tabs in the centre that each show one stage:

- A `usdcc.ui.ViewPanel` subclass is registered with `usdcc.app.register_view_panel(id, name,
  cls)`. It's then a view panel type like the built-in ones: the Stages menu offers it for
  every stage, and `usdcc.app.open_view_panel(id, stage=None)` opens one (on the current stage
  by default) and returns its Python object. Registering an id again replaces the type; the
  built-in ids can't be replaced.
- Each panel is a C++ `ScriptedViewPanel` with the usual options button and Stage dropdown,
  docking, floating and Duplicate. It makes one instance of the class:
  - `create_widget()` returns the content, which the panel takes ownership of;
  - `stage_changed(stage)` gets the shown `Usd.Stage` (or `None`) at the start and on every
    change;
  - `populate_options_menu(menu)` fills the options menu.

  `self.stage` and `self.widget` hold the current stage and the content. An exception in
  this code is logged; if `create_widget()` fails, the panel says so instead.
- **Why hosted, not bound**: `ViewPanel` isn't bound through Shiboken. Its constructor takes
  the pybind11 `StageManager`/`Stage` (PLAN.md §6 item 22). Also, `_ui.pyd` carries its own
  copy of the static `usdcc_ui` code, so a bound `ViewPanel` would use a second, unconfigured
  `UiScaleController::instance()`. The host is compiled into the app, and Python only fills
  it, through three bridge functions in `ui.py` that turn addresses and cache ids into
  PySide objects and `Usd.Stage`s.
- Like the built-in panels opened from the Stages menu, Python view panels aren't reopened
  by the saved layout on the next start; a startup script can open them.

```python
from PySide6 import QtWidgets
import usdcc.app, usdcc.ui

class PrimCount(usdcc.ui.ViewPanel):
    def create_widget(self):
        self.label = QtWidgets.QLabel()
        return self.label

    def stage_changed(self, stage):
        self.label.setText("No stage" if stage is None else "%d prims" % len(list(stage.Traverse())))

usdcc.app.register_view_panel("prim_count", "Prim Count", PrimCount)
```

Without PySide, `main_window()` and `add_side_panel()` raise an `ImportError` saying what to
build. `ViewPanel` from Python stays out of M7 (PLAN.md §6 item 22).

## 9. Phases

| Phase | Content | Verification |
|---|---|---|
| **1. One Python, embedded** *(done)* | Build: USD's Python DLL in the exe and test folders, `usd.pyd` built for it, `USDCC_PYTHON_HOME`. `PythonRuntime` (§3), logging bridge (§5), Debug > Run Python File… | Tests in a new `usdcc_python_tests` executable: start, `import pxr`, `sys.version` equals the Python OpenUSD reports, `print`/`logging`/`warnings`/exceptions reach the hub with the right source and level, an OpenUSD worker thread can take the GIL. In the app: a script file runs from the Debug menu; a broken one logs its traceback and the app keeps going |
| **2. App API and undo** *(done)* | `bindUsd()` shared; embedded `usdcc.usd`, `usdcc.app`, `usdcc.core`, `usdcc.tools`; Python helpers; `UndoManager` global edits; `ScriptRunner` with undo chunks and error handling | The exit-criterion test: open, edit, one undo step per stage, undo/redo match a UI edit; a failing script leaves one undoable step; `usd.pyd` still imports in a plain `python` |
| **3. Script Editor panel** *(done)* | `ScriptEditorSidePanel`, `CodeEditor`, `PythonHighlighter`, run/run selection, output pane, files, persistence, icons | Panel tests (offscreen): highlighting spans, indentation, run → output → log, unsaved marker. In the app: the exit criterion done by hand from the panel |
| **4. Startup scripts and CLI** *(done)* | Startup folder, `--script`, `--no-startup-scripts` | A failing startup script doesn't stop the others; `--script` output is in the log |
| **5. PySide integration** *(done)* | `main_window()`, `add_side_panel()`, `MainWindow::addSidePanel()`, `build-pyside.ps1` on USD's Python | A Python `SidePanel` shows in the View menu and survives a layout restore |

Phases 1 → 2 → 3 in order; 4 needs 2; 5 only needs 1, plus a PySide build.

## 10. Out of scope for M7

- **Stopping a running script.** Scripts run on the UI thread, so a Stop button can't be
  clicked while one runs. Options for later: an interrupt hotkey checked from a Python trace
  hook, or running on a worker with marshalled calls (which USD's threading makes hard).
- Debugging (breakpoints), code completion, a REPL prompt (Run Selection covers most of it).
- Undoing a stage open/close (the UI can't either).
- Python `ViewPanel`s and Python tools (M9 with the plugin API).
- Sandboxing scripts (PLAN.md §6 item 9 only asks that errors don't crash the app).

## 11. Decisions taken as defaults

Each of these can be changed before the phase that needs it starts:

| # | Question | Default used by this plan |
|---|---|---|
| 1 | Which Python to embed | The one OpenUSD was built with (§3.1), not vcpkg's. vcpkg's stays a build-time tool for pybind11 |
| 2 | Where scripts run | The main thread, blocking (§3.3) |
| 3 | A script that fails half-way | Keep its changes as one undoable step marked "(failed)", rather than rolling them back |
| 4 | Finalize Python at exit | Yes; fall back to skipping it if OpenUSD's modules make it crash |
| 5 | Namespace per Run | Fresh each time; a ☰ option keeps one for the session |
| 6 | Undo granularity for a Run | One step per changed stage (§4.2), even if the script used `edit()` inside |
| 7 | Script output | Both in the panel's output pane and in the log |
| 8 | Python-made UI | Optional phase 5, only with a PySide build (§8) |

## 12. What was built

### Phase 1

- **One Python in the build** ([UsdccPython.cmake](../cmake/modules/UsdccPython.cmake)):
  `USDCC_PYTHON_*` comes from OpenUSD's `pxrConfig.cmake`. `usdcc_deploy_python()` copies that
  Python's `python3XY.dll`/`python3.dll` next to the exe and the three test executables, after
  vcpkg's deploy step, replacing vcpkg's copy. `usd.pyd` is now built for it as well (it was
  for a stale 3.11). The pure-Python package files are copied to `build/python/usdcc` by a
  `usdcc_python_package` target.
- **[PythonRuntime](../src/cpp/scripting/include/usdcc/scripting/python_runtime.h)** in the new
  `usdcc_scripting` library:
  - Starts Python from `PythonSettings` (home = OpenUSD's Python, `USDCC_PYTHONHOME`
    overrides; `sys.path` gets `build/python` and OpenUSD's `site-packages`). It checks for a
    standard library first, so a wrong home is a logged reason, not a fatal Python abort.
  - Installs `usdcc.log` (below), imports `pxr.Usd` as a check, then releases the GIL.
  - Provides `runString()`, `runFile()` and `evaluate()`, from any thread. Exceptions are
    logged with their traceback, and `SystemExit` ends only the script.
  - Finalizes Python at exit: the app shuts down cleanly with `pxr` loaded (exit code 0).
  - It is created in `main()` after the stage manager and before the window.
- **Logging phase 5**: the embedded module `_usdcc_log` and
  [usdcc/log.py](../src/python/usdcc/log.py) cover the `logging` root handler (logger name
  as category, filtered by the hub's levels), `captureWarnings`, line-buffered
  `sys.stdout`/`sys.stderr`, `sys.excepthook` and `threading.excepthook`. Python thread
  names are kept on the records.
- **Debug > Run Python File…** (until the Script Editor panel). It is disabled, with the reason
  as its status tip, when Python couldn't start.
- **Tests**: the new `usdcc_python_tests` executable (one interpreter per process):
  - the interpreter is OpenUSD's Python, and `pxr.Usd` works;
  - only one runtime can exist;
  - print, partial lines, logger records with their call site, `exc_info`, warnings and
    stderr arrive with the right categories and levels;
  - exceptions are logged with message, traceback and innermost frame, and `SystemExit`
    is harmless;
  - another thread runs Python between runs, and Python thread names and thread
    exceptions are kept;
  - `runFile()` sets `__file__`.

Checked in the app: the log shows "Python 3.12.10 from C:/Program Files/Python312, with pxr".
A script run from the Debug menu found the app's open stage through `UsdUtils.StageCache` and
listed its prims. Its prints, a logger warning and its final exception reached the log with
their file and line.

Found for later:

- **Standalone `usd.pyd` needs OpenUSD's DLL folders.** In a plain Python, `import usdcc.usd`
  only works after `os.add_dll_directory()` for OpenUSD's root, `bin` and `lib` folders: Python
  3.8+ ignores PATH for extension modules, and `pxr` adds those folders only while it imports
  itself. Inside the app the DLLs are already loaded. Phase 2's "still imports in a plain
  python" check will make `usdcc/__init__.py` add them.
- **`usdcc.ui` still uses the stale Python 3.11.** Its CMake keeps the PATH lookup (cached at
  3.11) for PySide; phase 5 moves it to `USDCC_PYTHON_*`, together with `build-pyside.ps1`.

### Phase 2

- **Shared binding code** ([bind_usd.h](../src/cpp/usd/python/include/usdcc/usd/python/bind_usd.h),
  `usdcc_usd_bindings`): `StageManager`, now with selection (paths as strings).
  `open_stage` releases the GIL while it loads, so OpenUSD's own threads can take it. The
  standalone module is `usdcc/_usd.pyd`, and a stale `usd.pyd` is deleted at configure.
- **`UndoManager::beginGlobalEdit/endGlobalEdit`**: a session on every open stage and on each
  stage opened during the edit. Stages closed meanwhile are skipped, and the end can rename
  the step (for "(failed)").
- **`_usdcc_app`** ([app_module.h](../src/cpp/scripting/include/usdcc/scripting/app_module.h)):
  the app's `StageManager`, `UndoManager` (begin/end/cancel edits, undo/redo, texts, count)
  and tools, through `AppContextScope`. After that scope ends, calls raise `RuntimeError`
  instead of touching freed objects.
- **[ScriptRunner](../src/cpp/scripting/include/usdcc/scripting/script_runner.h)**: one global
  edit per run, named "Run <name>" or "Run <name> (failed)". Debug > Run Python File uses it.
- **Python front modules**: `usd.py`, `app.py`, `core.py`, `tools.py` (§4.1).
- **Tests** ([python_app_test.cpp](../tests/python_app_test.cpp); `usdcc_python_tests` now sets
  up an app context like `main()`'s):
  - **The exit criterion**: a script opens a stage, defines and sizes a cube and selects it.
    That is one step "Run exit_criterion.py"; a UI-style `apply()` goes on top; two undos from
    Python revert the UI edit and then the whole run; two redos bring both back.
  - **Several stages**: a run touching an open stage and one it opens gives one step on each,
    none on an untouched stage, and no stage opened twice.
  - **A failing run** leaves one "(failed)" step, and one Undo reverts it.
  - **`edit()`** makes one named step outside a run and nests into the run's step inside one.
  - **The API**: `is_embedded`, `current_stage`/`stages`/`stage_id`, `select`/`selected_paths`
    (bad paths raise), tools, `set_levels`.
  - **Standalone**: `_usd.pyd` works in a plain Python 3.12 with its own `StageManager`, and
    the app-only functions raise there.

Not checked in the app this time: the Windows session was locked (screen capture failed), so
Debug > Run Python File plus Edit > Undo on a script's change wasn't tried by hand. The tests
drive the same `ScriptRunner`, `_usdcc_app` and `UndoManager` the app uses.

Found along the way: opening a file that's already open makes a second stage on the same
layers, from File > Open Stage as from `open_stage()`. Both stages then record undo steps for
one change. Whether opening should return the open stage instead is a decision for later.

### Phase 3

- **[ScriptEditorSidePanel](../src/cpp/scripting/include/usdcc/scripting/script_editor_side_panel.h)**
  in the new `usdcc_scripting_ui` library, as described in §7. It's docked by `main()`
  through the new `MainWindow::addSidePanel()`, which phase 5's Python panels need too. Debug
  > Run Python File stays, for running a file without opening it.
- **[CodeEditor](../src/cpp/scripting/include/usdcc/scripting/code_editor.h)**: line numbers,
  current-line highlight, 4-space Tab/Shift+Tab, Enter keeping and adding indentation, Ctrl+/,
  and `selectedCode()`.
  **[PythonHighlighter](../src/cpp/scripting/include/usdcc/scripting/python_highlighter.h)**:
  a regex pass plus a string/comment scan, which keeps `#` inside strings and carries
  triple-quoted strings across lines. The editor and output fonts come from `dark.qss`.
- **The output pane** is fed by a log sink attached for the run, so it shows exactly what the
  Log panel gets for that run.
- **`PythonRuntime::Namespace::Session`** and `resetSession()`, for Keep Variables;
  `ScriptRunner` passes it through.
- **Tests** ([script_editor_panel_test.cpp](../tests/script_editor_panel_test.cpp); `usdcc_python_tests`
  is now a `QApplication` on the offscreen platform):
  - highlighting spans;
  - the editor's indent, dedent, comments, and Enter after `:`;
  - Run, a failing run's traceback, Run Selection, and kept and reset variables;
  - Save As, the • marker, recent files, unsaved text across a restart, and a clean file
    reopening from disk;
  - the unavailable state.

Not checked by hand: the Windows session was locked, so the panel hasn't been looked at in the
running app. The app does start cleanly with it (no warnings or errors in the log).

### Phase 4

- **[runStartupScripts()](../src/cpp/scripting/include/usdcc/scripting/startup_scripts.h)** and
  `userScriptsFolder()` in `usdcc_scripting_app`, as described in §6. A run logs how many
  scripts it found, each script's result (from `PythonRuntime::runFile()`), and a warning
  counting the ones that failed.
- **`PythonRuntime::addToPath()`** appends a folder to `sys.path`.
- **The command line** in `main()` now goes through `QCommandLineParser`: `--script <file>`
  (repeatable), `--no-startup-scripts`, `--help`, and the stage as before. An unknown option
  shows Qt's error box and exits. Extra positional arguments are logged and ignored. The
  startup scripts and `--script`s run from a zero-length timer after `window.show()`, so the
  window and the command-line stage exist.
- **Tests** ([startup_scripts_test.cpp](../tests/startup_scripts_test.cpp)): three scripts run
  in name order past a failing one, its exception and the others' output are in the log, with
  a summary; a shared module next to `startup/` imports; the folder doesn't shadow `usdcc`;
  nothing runs when skipped; a missing folder is created.
- **Checked in the app**: with a working and a failing startup script, plus `--script` and a
  stage, both startup scripts run (the failure logged with its traceback). The `--script`
  sees the command-line stage as current, and its output is in the log. With
  `--no-startup-scripts` the folder is skipped and `--script` still runs.

### Phase 5

- **PySide6 for OpenUSD's Python.** `tools/build-pyside.ps1` now:
  - builds for the `Python3_EXECUTABLE` in usdcc's CMake cache (OpenUSD's 3.12), or `-Python`;
  - enters the Visual Studio developer environment itself when `cl.exe` isn't on PATH;
  - finds pip's `ninja.exe` (`ninja.BIN_DIR`; the old path guess was wrong for `--user`
    installs);
  - copies `libclang.dll` and vcpkg's Qt dependencies next to `shiboken6.exe`, which otherwise
    can't start.

  The result is in `external/pyside-install` (PySide 6.11.1, matching vcpkg's Qt).
- **CMake**: the top level sets `USDCC_PYSIDE_DIR` (default `external/pyside-install`) and
  `USDCC_PYSIDE_FOUND`. `src/cpp/ui/python` builds only with it and uses `USDCC_PYTHON_*`; the
  stale PATH lookup (Python 3.11) and the pip-PySide fallback are gone. The generator gets
  `LLVM_INSTALL_DIR`, so it finds Clang's headers. With PySide found, its folder is on the
  embedded `sys.path` (`PythonSettings::defaults()`).
- **`usdcc.ui` is now `ui.py` over `_ui.pyd`** (typesystem package `usdcc._ui`), like
  `usd.py`/`_usd.pyd`. `ui.py` imports PySide6 first (its DLL folders), imports `_ui` inside
  `Tf.WindowsImportWrapper` (OpenUSD's DLLs), and re-exports `SidePanel`. `usdcc/__init__.py`
  no longer imports anything, so `import usdcc.log` at start-up doesn't load PySide.
- **`usdcc.app.main_window()` and `add_side_panel()`** (§8), through two new `_usdcc_app`
  functions that take and return addresses (`AppContext` gains `mainWindow`).
  `add_side_panel()` checks the type and area, then parents the panel to the window from
  Python, which hands its ownership to the window.
- **`MainWindow::addSidePanel()` after start-up**:
  - it uses `restoreDockWidget()` once `restoreLayout()` has run;
  - it shows the panel when the window is already visible;
  - it inserts the panel into the View menu above the toolbars and removes it when the panel
    is destroyed;
  - it warns about two panels with the same name.

  `saveLayout()` logs the panels it saved at debug level.
- **`MainWindow::layoutSettled()`**, emitted 300 ms after the window was shown and stopped
  resizing. Found by trying it in the app: Qt keeps re-applying a restored layout while a
  window opening maximized gets its final size (`QMainWindowLayout`'s `restoredState`, dropped
  150 ms after the last resize). A dock widget added before then is taken out of the layout
  again and shown at (0, 0) when made visible. Startup scripts and `--script` now run on this
  signal instead of right after `show()`. Panels added from the Script Editor panel are never
  affected.
- **`AppContextScope`s nest**, so the tests can give a window of their own.
- **Tests** ([python_panels_test.cpp](../tests/python_panels_test.cpp), built when `usdcc.ui`
  is):
  - `main_window()`;
  - a Python subclass added on the left, after the shown window settled: it stays docked and
    visible, survives its script's namespace, and is in the View menu;
  - a plain `QDockWidget` and an unknown area are refused;
  - after a save and a new window, the panel comes back on the left although it asks for the
    bottom;
  - deleting it removes it from the menu.

  The window's layout goes to Qt's test-mode config folder.
- **Checked in the app**: a startup script adds a panel; a `--script` moves it to the right;
  the next session puts it back on the right, shown and in the View menu (screenshot taken).
  This was also the first look at the Script Editor panel in the running app.

### After M7: Python view panels (milestone M1)

- **[ScriptedViewPanel](../src/cpp/scripting/include/usdcc/scripting/scripted_view_panel.h)**
  in `usdcc_scripting_app` (built with Python only), with `PythonObject`
  (`src/python_object.h`), which releases a Python reference with the GIL held, or abandons
  it once Python is finalized (the panel registry outlives the interpreter).
- **`_usdcc_app`**: `register_view_panel`, `open_view_panel`, `view_panel_types`.
  `AppContext` gains `viewPanels` (the app's `ViewPanelRegistry`). `usdcc.app` wraps them
  (§8). **`usdcc.ui.ViewPanel`** and the bridge functions are in `ui.py`.
- **Tests** (`python_view_panel` in [python_panels_test.cpp](../tests/python_panels_test.cpp)):
  - a type is registered (listed like the built-in ones) and opened on a stage;
  - its Python widget sees the stage, and a stage change;
  - Python fills the options menu;
  - the panel floats, and Duplicate makes another instance on the same stage;
  - built-in ids and non-`ViewPanel` classes are refused;
  - a panel whose `create_widget()` raises still opens and says so.
- **Checked in the app**: a startup script registers "Prim Count", and a `--script` opens it
  on `cube_and_sphere.usda`. It's docked in the centre with the options button and Stage
  dropdown, and its label reads "4 prims in cube_and_sphere.usda". The screen was locked, so
  the window was captured with `QWidget::grab()` from the script.
