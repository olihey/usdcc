# Scripting — Plan

*Status: phase 1 done (see §12); phases 2–5 to do. Milestone M7 in [PLAN.md](PLAN.md); also covers logging phase 5
([LOGGING.md](LOGGING.md) §3.4). Last updated: 2026-11-02.*

## 1. Goal

Users can script the running application in Python:

- A **Scripting side panel**: an editor with Python syntax highlighting, load/save, Run and
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
- **Undo works per stage, by layer snapshot.** `UndoManager::beginEdit/endEdit` snapshot one
  stage's edit-target layer and nest by depth. A script may touch several stages, including
  one it opens itself.
- **OpenUSD calls into Python from its own threads** (Python notice listeners and Python
  plugins, guarded by `TfPyLock`). A main thread that keeps the GIL would deadlock them.
- **The log is ready for Python** (LOGGING.md §3.4): one hub in `usdcc_core.dll`; native
  stdout/stderr is already captured, so even `print()` reaches the log today, as source
  `stdout`.

## 2. Architecture

| Target | Contents |
|---|---|
| `usdcc_scripting` (new, static, Qt-free) | `PythonRuntime` (interpreter start/stop, paths, GIL), `ScriptRunner` (run a file or a string, output, exceptions, undo chunk), the embedded modules (§4) |
| `usdcc_usd_bindings` (new, static) | The `usdcc.usd` binding code as `bindUsd(py::module_&)`, shared by `usd.pyd` and the embedded module |
| `usdcc_scripting_ui` (new, static, Qt) | `ScriptingSidePanel`, `PythonHighlighter`, `CodeEditor` (line numbers, indentation) |
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
- **M11** replaces the absolute dev-build paths with a Python bundled next to the exe.
  Nothing else changes.

A failure here (no Python found, `import pxr` fails) is logged as an Error with the reason. The
app keeps running, and the Scripting panel says scripting is unavailable and why.

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

The exe registers embedded modules (`PYBIND11_EMBEDDED_MODULE`) and puts them in `sys.modules`
under their package names **before any user code runs**. `import usdcc.usd` then finds the
app's own module rather than `usd.pyd` on `sys.path`.

| Module | Contents |
|---|---|
| `usdcc.usd` | `StageManager`, `UndoManager` (new binding), the existing cache-id calls; plus `current_stage()` → `pxr.Usd.Stage`, `stages()`, `open_stage(path)` → `Usd.Stage`, `stage_id(stage)`, selection (`selected_paths()`, `select(paths)`) |
| `usdcc.app` | The running app's objects: `stage_manager`, `undo_manager`, `tool_set`, `version`; `is_embedded()` |
| `usdcc.core` | `log(level, category, message)`, `LogLevel`; the hub's levels (`set_level`) |
| `usdcc.tools` | `ToolKind`, `active_tool()`, `set_active_tool(kind)`, tool options by id |
| `usdcc.log` | The logging bridge (§5) |

Outside the app (a plain `python` with `build/python` on its path), `usdcc.usd` is still
`usd.pyd` with its own `StageManager`, and `usdcc.app` reports `is_embedded() == False`. Both
build from the same `bindUsd()` code, so the API matches.

Stages stay cache ids at the binding level; the `Usd.Stage` helpers live in `src/python/usdcc`
and use `UsdUtils.StageCache`. A script works with `pxr` directly:

```python
from pxr import Sdf
import usdcc.usd

stage = usdcc.usd.open_stage("C:/shots/a.usda")        # opens it in the app, like File > Open
cube = stage.DefinePrim("/World/Cube", "Cube")
cube.GetAttribute("size").Set(2.0)
usdcc.usd.select([cube.GetPath()])                      # the Outliner and Viewport follow
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
  the window is up, in name order, each in its own namespace. A failing one is logged and the
  rest still run. That folder's parent, `scripts/`, is on `sys.path` for shared modules.
- `usdcc.exe --script file.py [stage]` runs a script after start-up (logged, not shown in the
  panel). `--no-startup-scripts` skips the startup folder.
- **M9** builds plugins on this: a Python plugin is a package with a manifest, loaded by the
  same runner.

## 7. The Scripting panel

`ScriptingSidePanel` (a `SidePanel`, objectName "Scripting", docked like the Log panel):

```
[☰] [ shot_setup.py ▾ ]                                    [▶] [▶|] [📂] [💾]
┌──────────────────────────────────────────────────────────────────────┐
│ 1  from pxr import Sdf                                                │
│ 2  import usdcc.usd                                                   │
│ 3  stage = usdcc.usd.current_stage()                                  │
├──────────────────────────────────────────────────────────────────────┤
│ >>> Run shot_setup.py (12 ms)                                         │
│ Created 3 prims                                                       │
└──────────────────────────────────────────────────────────────────────┘
```

- **Editor**: a `QPlainTextEdit` with a line-number gutter, monospace font, 4-space tabs, keeping
  the indentation on Enter (and indenting after `:`), Tab/Shift+Tab on a selection, and
  Ctrl+/ to toggle comments.
- **Highlighting** (`PythonHighlighter`): keywords, builtins, strings (including triple-quoted
  across lines), comments, numbers, decorators, and `def`/`class` names, in theme colors.
- **Run** (Ctrl+Enter): the selection if there is one, otherwise the whole text. **Run
  Selection** runs the current line if nothing is selected. Each run gets a fresh namespace,
  with `__file__` set when the text came from a file; ☰ > *Keep Variables Between Runs* uses
  one namespace for the session instead.
- **Output pane** below the editor: each run's header with its duration, stdout, stderr
  (amber) and tracebacks (red), with ☰ > Clear Output. Everything also goes to the Log panel.
- **Files**: Open, Save, Save As, and a recent-files dropdown that also names the open file. A
  "•" marks unsaved changes. The panel remembers the open file and keeps unsaved text
  between sessions.
- **Toolbar**: the shared `PanelMenuButton` and `PanelIconButton`s (new theme icons: play,
  play-selection, open, save).
- **When scripting is unavailable** (§3.1), the panel shows the reason instead of the editor.

## 8. Python-made UI (needs PySide)

Only once `tools/build-pyside.ps1` has run and `usdcc.ui` builds:

- `usdcc.app.main_window()` returns the main window through `shiboken6.wrapInstance`.
- A Python `usdcc.ui.SidePanel` subclass can be added with `usdcc.app.add_side_panel(panel)`,
  which docks it and lists it in the View menu (`MainWindow` gains `addSidePanel()`).
- The same PySide must be built for the same Python as §3.1; `build-pyside.ps1` is changed to
  use it.

Without PySide, `main_window()` and `add_side_panel()` raise an `ImportError` saying what to
build. `ViewPanel` from Python stays out of M7 (PLAN.md §6 item 22).

## 9. Phases

| Phase | Content | Verification |
|---|---|---|
| **1. One Python, embedded** *(done)* | Build: USD's Python DLL in the exe and test folders, `usd.pyd` built for it, `USDCC_PYTHON_HOME`. `PythonRuntime` (§3), logging bridge (§5), Debug > Run Python File… | Tests in a new `usdcc_python_tests` executable: start, `import pxr`, `sys.version` equals the Python OpenUSD reports, `print`/`logging`/`warnings`/exceptions reach the hub with the right source and level, an OpenUSD worker thread can take the GIL. In the app: a script file runs from the Debug menu; a broken one logs its traceback and the app keeps going |
| **2. App API and undo** | `bindUsd()` shared; embedded `usdcc.usd`, `usdcc.app`, `usdcc.core`, `usdcc.tools`; Python helpers; `UndoManager` global edits; `ScriptRunner` with undo chunks and error handling | The exit-criterion test: open, edit, one undo step per stage, undo/redo match a UI edit; a failing script leaves one undoable step; `usd.pyd` still imports in a plain `python` |
| **3. Scripting panel** | `ScriptingSidePanel`, `CodeEditor`, `PythonHighlighter`, run/run selection, output pane, files, persistence, icons | Panel tests (offscreen): highlighting spans, indentation, run → output → log, unsaved marker. In the app: the exit criterion done by hand from the panel |
| **4. Startup scripts and CLI** | Startup folder, `--script`, `--no-startup-scripts` | A failing startup script doesn't stop the others; `--script` output is in the log |
| **5. PySide integration** *(needs PySide)* | `main_window()`, `add_side_panel()`, `MainWindow::addSidePanel()`, `build-pyside.ps1` on USD's Python | A Python `SidePanel` shows in the View menu and survives a layout restore |

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
- **Debug > Run Python File…** (until the Scripting panel). It is disabled, with the reason
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
