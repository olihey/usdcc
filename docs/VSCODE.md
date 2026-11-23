# VS Code (plan)

Write usdcc scripts and plugins in VS Code, and run and debug them in the running app: a small
server in usdcc, type stubs, `debugpy`, and a thin extension on top.

**Status:** planned 2026-11-16, updated 2026-11-23. Nothing is built yet. Facts marked *checked* were
looked up on one of those days; everything else is expected, and §9 lists what to try first.

**What the update changed:**

- A small extension comes right after the link, not last, and there are more phases (§7).
- More features: starting usdcc with F5, templates, manifest checking, Problems, a Plugins view,
  Stop (§6.2). Later ones are in §6.3.
- The part shared with [MCP.md](MCP.md) is named (§3.4), and the link has a build option (§3.1).
- Completion needs stubs of our own for `pxr` and PySide6 (§4).
- Updating usdcc while VS Code is open is a risk to test (§9 #8).

## 1. What it is, and isn't

- **You edit in VS Code, and the code runs in usdcc.** *Run in usdcc* sends the file or selection to
  the app, which runs it exactly as the Script Editor's Run does: on the main thread, as one undo
  step, with its output in the Log panel. The output also comes back to VS Code.
- **Completion and go-to-definition** for `usdcc`, `pxr` and PySide6 come from VS Code's own Python
  support, once it's told where they are (§4).
- **Breakpoints** work through `debugpy` inside the app's interpreter (§5), in scripts and in a
  plugin's callbacks, panels and tools.
- **The Script Editor stays.** It's still the quickest way to try a line, and it already follows a
  file changed on disk, so a file can be open in both.
- **The link isn't tied to VS Code.** It's a local pipe with a small JSON protocol (§3), so another
  editor, a command line or a test can use it. VS Code is the first client.

**Not an embedded editor.** VS Code inside a usdcc panel was considered and dropped:

- **It needs QtWebEngine,** a Chromium build that isn't a dependency today (`vcpkg.json` has
  `qtbase`, `qtsvg`, the docking system and `qtnodes`; *checked*), plus a VS Code web server beside
  the app.
- **It still needs everything in this plan** for Run, output and debugging, as the embedded editor's
  Python wouldn't be the app's interpreter either.
- **The user's own VS Code,** with their settings, keys and extensions, wouldn't be the one embedded.

## 2. The parts

| Part | Where | What it is |
|---|---|---|
| `EditorLink` | `usdcc_scripting_app` (C++, Qt Network) | The server: a `QLocalServer` taking requests and running them through `ScriptRunner` and `PluginManager` |
| `usdcc.debug` | `src/python/usdcc/debug.py` | `listen()`, `is_listening()`, `port()`: starts `debugpy` in the app |
| Type information | `src/python/usdcc/*.py`, `_usdcc_app.pyi`, generated `pxr` and PySide6 stubs | Type hints on the public functions, and stubs for what is compiled |
| `tools/usdcc_send.py` | Standard library only | The command-line client: `usdcc_send.py run file.py`. Also what the tests use |
| The extension | `editors/vscode/` (TypeScript) | Commands, a status bar item and an output channel, over the three above |

Qt Network is in the build already: Peers links it (`src/cpp/peers/CMakeLists.txt`; *checked*).

## 3. The link

### 3.1 Transport

**A named pipe** (`QLocalServer`; a Unix domain socket on Linux and macOS), not a TCP port:

- **Only the same user can open it** (`QLocalServer::UserAccessOption`). It isn't reachable from the
  network, and not from a web page, which a server on `localhost` would be.
- **No port to choose, and no firewall prompt.**
- **Node opens one natively** (`net.connect("\\\\.\\pipe\\…")`), so the extension needs no native
  module.

**No password or token.** A process running as the user can already put a file in the startup
scripts folder, which the app runs at its next start. The pipe gives it nothing new.

**Finding the app.** Each usdcc names its pipe `usdcc-editor-<pid>`. A client lists the pipes with
that prefix (`\\.\pipe\` on Windows; `$XDG_RUNTIME_DIR/usdcc/` elsewhere, where a leftover socket
that refuses a connection is deleted) and asks each one `hello`. With one app running it connects;
with several it asks which, showing each one's current stage.

- **No file says where the app is.** A file in `%LOCALAPPDATA%` goes stale after a crash, and a
  client packaged as MSIX may see a redirected copy of that folder. A pipe goes away with its
  process.
- **A test profile** (`USDCC_TEST_PROFILE`) names its pipe `usdcc-test-<profile>-editor-<pid>`, so
  an editor never connects to an app a test started.

**Turning it off:** the setting **Allow editors to connect** (`Python/editorLink`, on by default,
applies at the next start) on the Python page, and `--no-editor-link` for one start. Off, no pipe
is made.

**Leaving it out of a build:** `USDCC_BUILD_EDITOR_LINK`, on by default, from phase 1
([BUILD_OPTIONS.md](BUILD_OPTIONS.md) §6). It's code inside a library, so it's a compile definition:
off, `EditorLink` and the setting aren't compiled in. If that document's mechanism isn't there yet,
it's a plain CMake option until it is.

### 3.2 Protocol

One JSON object per line, UTF-8. A request has an `id` and a `method`; its answer carries the same
`id`; lines without an `id` are events.

```text
→ {"id": 1, "method": "run", "params": {"code": "…", "file": "C:/shots/setup.py", "line": 1}}
← {"event": "output", "run": 1, "source": "python", "category": "python.stdout", "level": "info", "text": "Created 3 prims"}
← {"id": 1, "result": {"ok": true, "ms": 12, "undoSteps": 1}}
```

| Method | Parameters | Result |
|---|---|---|
| `hello` | | `api` (this protocol's number, 1), usdcc's version, `pid`, the current stage's name, Python's version, `python.executable` and `python.paths` (§4), whether the debugger is listening and on which port |
| `run` | `code`; `file` or `name`; `line` (where `code` starts in `file`, default 1); `scope`: `fresh` or `session` | `ok`, `ms`, `undoSteps`, and on a failure `error`: the exception's type and message, and the innermost frame's file and line |
| `resetSession` | | Forgets the kept variables, as ☰ > Reset Variables does |
| `plugins` | | Each plugin's id, name, folder, state, and why it can't be reloaded if it can't |
| `reloadPlugin` | `id`, or `path` (any file inside the plugin's folder) | `ok`, `ms`, and the plugin's state afterwards |
| `debugListen` | | The port. Starts `debugpy` if it isn't listening (§5) |

**A request that can't be carried out** gets `{"id": …, "error": {"code": …, "message": …}}`
instead of a result: `busy` (a script is running), `dialog` (the app is showing a modal dialog),
`unavailable` (Python couldn't start, with the reason), `not-found`, `bad-request`.

### 3.3 Running

- **Through `ScriptRunner`,** so a run is one undo step per stage it changed, named "Run setup.py",
  and a failing one leaves "Run setup.py (failed)" ([SCRIPTING.md](SCRIPTING.md) §4.2).
- **On the main thread, one at a time.** Requests arrive through the event loop, so one sent while
  a script runs waits its turn.
- **Never inside something else.** A modal dialog, or a script showing one, runs its own event
  loop, and a request arriving then would run a script in the middle of whatever opened the dialog.
  So `run` and `reloadPlugin` are refused while a run is active (`busy`) or a modal widget is up
  (`dialog`), where the Script Editor's own Run button couldn't be clicked either.
- **`file` is the name the code is compiled under,** as the Script Editor does today
  (`runCode()`; *checked*). Tracebacks name it, and the debugger's breakpoints match it. The text
  run is what the editor sent, so an unsaved file runs as it looks.
- **`line` fixes a selection's line numbers.** `ScriptRunner::run()` gains a first line, and the
  code is compiled with that many empty lines before it. Today a traceback from Run Selection counts
  from the selection's first line, in the Script Editor too (*checked*: `runSelection()` passes the
  file's name and only the selected text). Both get the fix.
- **Output is sent while the script runs.** A log sink, attached for the run like the Script
  Editor's `RunCapture`, forwards each record as an `output` event, leaving out the same
  bookkeeping records. The main thread is busy, so the sink writes and waits for the bytes to go
  (§9 #1).
- **The app shows it happened:** the Log panel gets "Run setup.py (from an editor)" and everything
  the run printed. The Script Editor's own output pane doesn't (§8 #3).

### 3.4 What MCP shares

[MCP.md](MCP.md) §4 needs most of what a run over the link needs, and isn't built either. Whichever
comes first builds it, in `usdcc_scripting_app`, for both:

- **`ScriptRunner` returning what happened:** output, error, undo steps, time.
- **`RunCapture`, moved out of the Script Editor** (it's in `script_editor_side_panel.cpp`; *checked*).
- **Whether a script is running or a dialog is open** (§3.3). MCP's reading tools are allowed during a
  dialog, so it's a question a caller asks, not a refusal inside `ScriptRunner`.
- **Stopping a script** (§6.2).

The transports stay separate. The pipe is the user's own and needs no token; MCP is HTTP on loopback,
off by default, with a token and access levels.

## 4. Completion

What VS Code's Python support needs is the app's `sys.path`, an interpreter to call its own, and
types where the code is compiled.

**The paths come from the app.** `hello` returns them, so nothing is guessed:

| | Deployed | A build |
|---|---|---|
| `python.executable` | `python\python.exe`, whose `site-packages` has `usdcc`, `pxr` and PySide6 (*checked* in `deploy/`) | The Python OpenUSD was built with |
| `python.paths` | The shared packages environment, the additional package folders, the user's `scripts/` | Those, plus `build/python`, OpenUSD's `lib/site-packages` and `USDCC_PYSIDE_DIR` |

They go into the workspace's `.vscode/settings.json` as `python.defaultInterpreterPath` and
`python.analysis.extraPaths`, merged with what's there.

**Types, where they're missing:**

- **`usdcc`'s own functions get type hints.** The modules users import are plain Python
  (`usd.py`, `app.py`, …) with docstrings but no annotations, and each forwards to `_usdcc_app`,
  which no analyzer can read as it's compiled into the exe (*checked*). So `current_stage()` is
  annotated `-> Usd.Stage | None` and so on. About 280 functions, none annotated
  (*checked*). This is most of the work, and it improves the docs too. It can be done a module at a
  time: `usd`, `app` and `ui` first, then `tools`, `render` and the rest.
- **`_usdcc_app.pyi`,** for the classes that come straight from C++: `StageManager`, `UndoManager`,
  `Viewport`, `ViewportCamera`, `RendererSetting`. Generated with `pybind11-stubgen` run inside
  the app, kept in the repository beside the package, and deployed with it. A test in
  `usdcc_python_tests` generates it again and fails when the two differ.
- **`pxr` has no stubs in this OpenUSD build** (*checked*: no `.pyi` in it), and the third-party
  `types-usd` stops at 24.5.2, from August 2024 (*checked*; usdcc is on 26.11). So they're generated
  from the OpenUSD usdcc ships, in the job that builds OpenUSD, and can't be of another version.
  With which tool is §9 #5.
- **PySide6's `.pyi` files aren't in the deploy** (*checked*: no `Qt*.pyi`). PySide6 comes with
  a generator for them; running it in the PySide build is §9 #6.
- **All stubs ship in the deploy,** in `python\stubs`, which goes on `extraPaths` only: never on the
  app's `sys.path`. Nothing is downloaded.

**Without the extension:** **Set Up VS Code…** in the Script Editor's ☰ menu, and on a plugin's
right-click menu in View > Plugins, writes the same settings and a debugger configuration (§5) into
a folder's `.vscode/`, and **Open in VS Code** runs `code <folder>` (greyed out, saying why, when
`code` isn't on `PATH`). That alone gives completion and breakpoints, with Run still pressed in
the app.

## 5. Debugging

**`debugpy`, bundled and pinned like ruff** (`cmake/modules/UsdccDebugpy.cmake`, deploy
`tools\debugpy\`, `-DUSDCC_BUNDLE_DEBUGPY=OFF` skips it). Its folder goes on `sys.path` only when
debugging starts, at the end, so no plugin gets it by accident.

**Starting it:** `usdcc.debug.listen()`, from ☰ > **Allow Debugger to Attach** in the Script
Editor, or from the extension through `debugListen`:

```python
debugpy.configure(python=python_exe)          # PythonRuntime::executable(), a real python.exe
host, port = debugpy.listen(("127.0.0.1", port))
```

- **`configure(python=…)` is needed** because `debugpy` starts a helper process with
  `sys.executable`, which in the app is expected to be `usdcc.exe` (§9 #2). A deployed app has
  `python\python.exe` for this, as uv already uses ([PYTHON_PACKAGES.md](PYTHON_PACKAGES.md) §4).
- **The port** is `Python/debugPort` (5678, `debugpy`'s usual), or a free one if that's taken, as
  by a second usdcc. The log and `hello` say which.
- **Never at startup, only when asked.** Unlike the pipe, this is a TCP port on `127.0.0.1`, which
  any process on the machine can connect to, and a debugger can run code. Once listening it stays
  so until the app quits, as `debugpy` has no call to stop.

**What works once VS Code is attached:**

- **A script run from VS Code or from the Script Editor,** as both compile under the file's path
  (§3.3).
- **A plugin's code:** `activate()`, menu actions, panels, tools. Plugins load from their files, so
  their paths are real.
- **Stepping into `usdcc`'s own Python** with `"justMyCode": false`.

**What it costs:**

- **A breakpoint freezes the app.** Scripts run on the main thread
  ([SCRIPTING.md](SCRIPTING.md) §3.3), so while one is paused the window doesn't repaint, and
  Windows marks it "Not Responding" after a few seconds. That's usual for an app that runs scripts
  on its UI thread. Pumping
  Qt's events while paused would let the UI edit a stage in the middle of a script, so it isn't
  done.
- **Python run from OpenUSD's own threads** (notice listeners) isn't traced, as `debugpy` only
  follows threads it knows about.
- **Code that runs on every frame** (a tool's `build_gizmo()`) is slower while a debugger is
  attached. By how much is §9 #4.

## 6. The extension

### 6.1 Running, reloading, debugging

```text
┌ shot_setup.py ─────────────────────────────────────────────────────────┐
│ 1  from pxr import Sdf                                                 │
│ 2  import usdcc.usd                                                    │
│ 3  stage = usdcc.usd.current_stage()                                   │
├ OUTPUT: usdcc ─────────────────────────────────────────────────────────┤
│ >>> Run shot_setup.py                                                  │
│ Created 3 prims                                                        │
│     done in 12 ms, 1 undo step                                         │
└────────────────────────────────────────────────────────────────────────┘
  ⚡ usdcc 0.1 · shot_a.usda                              status bar
```

| Command | Key, while connected | What it does |
|---|---|---|
| **usdcc: Run File** | `Ctrl+Enter` | Sends the editor's text, saved or not. In a plugin's folder it reloads the plugin instead, as the Script Editor's plugin tab does |
| **usdcc: Run Selection** | `Ctrl+Shift+Enter` | The selected lines, or the cursor's line, with their place in the file (§3.3) |
| **usdcc: Reload Plugin** | | The plugin whose folder (the one with `usdcc-plugin.json`) holds the current file. Saves its files first |
| **usdcc: Attach Debugger** | | `debugListen`, then starts VS Code's debugger on the port it returns |
| **usdcc: Set Up Workspace** | | Writes the interpreter and paths (§4) |
| **usdcc: Start usdcc** | | Starts the app, and connects when its pipe answers (§6.2) |
| **usdcc: Stop Script** | | Interrupts the running script (§6.2) |
| **usdcc: New Plugin** | | A plugin from a template (§6.2) |
| **usdcc: Connect** | | Picks the app when several are running |
| **usdcc: Keep Variables Between Runs**, **Reset Variables** | | `scope: session`, and `resetSession` |

- **The keys are the Script Editor's,** and apply only to Python files while an app is connected,
  where they replace VS Code's "insert line below/above".
- **The status bar item** shows the app's version and current stage, or "usdcc: not running", and
  reconnects by itself when an app starts.
- **The output channel** shows each run as the Script Editor's output pane does. A failed run also
  moves the cursor to the innermost frame that's in the workspace.
- **`usdcc.reloadPluginOnSave`,** off by default: saving a file in a plugin's folder reloads it.
  This is the Reload on Save [PLUGIN_DEV.md](PLUGIN_DEV.md) §4 left out.
- **It depends on Microsoft's Python Debugger extension** (`ms-python.debugpy`) for attaching, and
  uses the Python extension's settings for completion. It bundles nothing of its own but its code.
- **`hello`'s `api` number decides compatibility.** An extension meeting a newer number says to
  update it, and does nothing else.

### 6.2 Making a plugin

- **Starting usdcc (F5).** A `usdcc` launch configuration starts the app with `--plugin-path`, waits
  for its pipe, calls `debugListen` and attaches. It uses a development profile by default, so a
  broken plugin can't disturb the user's own settings (§8 #10). Attaching comes after `activate()`
  has run, so a breakpoint there is hit by a reload.
- **New Plugin** offers templates: a panel, a tool, a menu action, a prim menu action, a final
  renderer. The app's own **New Plugin...** can use them too, so there's one set.
- **A JSON schema for `usdcc-plugin.json`:** completion and errors while typing. It's a file in the
  deploy, and a test checks every manifest in this repository against it.
- **Problems.** A failed run marks its line, and a plugin that failed to load marks the line that
  raised. Both are cleared by the next run or reload. Tracebacks in the output are links.
- **A Plugins view:** what `plugins` returns, each with its state and reason, and Reload and Open
  Folder.
- **Stop.** The pipe has to be read off the main thread, to see a `stop` request while a script runs.
  It raises `KeyboardInterrupt` in the main thread between two Python instructions
  (`PyThreadState_SetAsyncExc()`; MCP.md §4 item 5). It can't break a long call into C++. The Script
  Editor gets a Stop button from it too. Untried (§9 #9).
- **Snippets** for the common `usdcc` calls. Hover text is the docstrings, through §4.

New in the protocol for these: the `stop` method, and a `plugins-changed` event. `plugins` also
returns the file and line of a failure.

### 6.3 Later

Not designed yet, each on its own:

- **Plugin tests in the Test Explorer,** perhaps headless: `usdcc_node` starts Python and activates
  plugins (*checked*), but has no window.
- **Packaging a plugin** as a zip, with its requirements checked.
- **C++ plugins:** CMake presets for the SDK, and starting the app under a C++ debugger.
- **USD helpers:** insert the selected prim's path, select the path under the cursor, show the stage.
- **AI setup:** write the MCP client's configuration, once MCP exists.

### 6.4 Shipping

**Shipped** as `tools\vscode\usdcc.vsix` in the deploy, installed with
`code --install-extension`. **Set Up VS Code…** offers to do that. Publishing it to the
Marketplace is a decision for later (§8 #6).

## 7. Phases

Sizes are estimates: S is about one session, M a few, L several.

| Phase | Size | Needs | Builds | Verification |
|---|---|---|---|---|
| **0. Spikes** | S | | §9's items | Each has its answer in §9, and this plan is changed where it's wrong |
| **1. The link** | M | 0 | The shared run core (§3.4) except Stop, `EditorLink`, the protocol without `debugListen`, the setting, `--no-editor-link`, the build option, `ScriptRunner`'s first line, `tools/usdcc_send.py` | Tests in `usdcc_python_tests`, over a real pipe: a run is one undo step and its output arrives before its result; a failing run returns its error and leaves a "(failed)" step; a selection's traceback has the file's line numbers; `busy` and `dialog`; two clients; `reloadPlugin` by id and by path; nothing listens when turned off. A build with the option off compiles and starts. In the app: `usdcc_send.py` against a deployed usdcc |
| **2. First extension** | M | 1 | `editors/vscode`: Run File and Selection, kept variables, output with traceback links, Reload and reload on save, Connect, status bar, a CI job that builds the `.vsix` | Its protocol code against a fake server in Node. By hand: edit a plugin, save, see it change; with one app and with two |
| **3. Debugging** | M | 0; the test, `debugListen` and Attach need 1 and 2, as does F5 | Bundled `debugpy`, `usdcc.debug`, the menu entry, `debugListen`, Attach, then starting usdcc (F5) | A small debug-protocol client in the tests: attach, set a breakpoint, run a script over the link, see it stop there, continue, and the app still exits with code 0. By hand: a breakpoint in a plugin's menu action, and F5 from nothing |
| **4. Completion** | L | 0; Set Up Workspace needs 2 | Type hints in `src/python/usdcc`, `_usdcc_app.pyi` and its test, the `pxr` and PySide6 stubs, **Set Up VS Code…**, **Open in VS Code**, Set Up Workspace | Pyright from the command line over the example plugin and a sample script, with the written settings: no unresolved imports, and `usdcc.usd.current_stage()` is a `Usd.Stage`. By hand: completion in VS Code |
| **5. Plugin workflow** | M | 2 | §6.2 except F5: templates, the manifest schema, Problems, the Plugins view, Stop, snippets | Every manifest here passes the schema; a stopped endless loop leaves a "(failed)" step; the extension's tests. By hand: a working panel within five minutes of installing |
| **6. Shipping** | S | 2; best after 3 to 5 | The `.vsix` in the deploy, **Set Up VS Code…** installing it, user documentation | On a clean machine: install usdcc, set up, run and break, with nothing else done by hand |
| **7. Later** | L | 5 | §6.3, chosen one by one | Each with its own plan |

0, 1, 2 in order. 3 and 4 can go alongside 2, apart from what needs it. **The first version worth
using is 0 to 3.** 3's debugger and 4 are useful without the extension, through **Allow Debugger to
Attach** and **Set Up VS Code…**.

## 8. Decisions taken as defaults

Each can be changed before its phase starts.

| # | Question | Default used by this plan |
|---|---|---|
| 1 | Is the link on without asking? | Yes: the pipe is the user's own and adds no rights (§3.1). The debugger's port is the opposite: only when asked (§5) |
| 2 | Pipe or TCP port | Pipe. A port would let a remote or containerized VS Code connect, and that's left out (§10) |
| 3 | Where a remote run's output shows in the app | The Log panel only, not the Script Editor's output pane, which is for runs started there |
| 4 | Type information | Hints in the Python modules plus one generated stub, rather than stubs for everything |
| 5 | Where `debugpy` comes from | Bundled and pinned, rather than through **My packages**, so debugging works with nothing installed |
| 6 | How the extension reaches users | A `.vsix` in the deploy first. A Marketplace and Open VSX listing needs a publisher account, and is yours to decide |
| 7 | Run keys in VS Code | The Script Editor's, only while connected (§6) |
| 8 | Where the extension's source lives | This repository, `editors/vscode`, so the protocol and its client change together |
| 9 | `pxr` stubs | Generated from the shipped OpenUSD, rather than `types-usd`, which stopped at 24.5 (§4) |
| 10 | The profile F5 starts usdcc with | A development one, through `USDCC_TEST_PROFILE`. Its pipe isn't listed to editors (§3.1), so F5 connects to the process it started, by its pid and profile |
| 11 | C++ plugins | Later (§6.3): Python is the easy path |

## 9. To try first

Things the plan rests on and that haven't been run. Phase 0 does them; the phase named is the one
that needs the answer.

1. **Writing to the pipe while the main thread is busy** (phase 1). The sink has to push bytes out
   without the event loop (`waitForBytesWritten`). If Qt's pipe writer on Windows needs the loop,
   the writes move to a thread of their own.
2. **`debugpy.listen()` in the embedded interpreter** (phase 3): that `sys.executable` is
   `usdcc.exe` and `configure(python=…)` is enough, and that the deploy's `python.exe` can start
   the helper with `bin\` on `PATH`.
3. **Finalizing Python with `debugpy` loaded** (phase 3). `Py_FinalizeEx` already has OpenUSD's
   modules to cope with ([SCRIPTING.md](SCRIPTING.md) §3.2); `debugpy`'s threads are one more
   thing that could hold up the exit.
4. **The cost of an attached debugger** on a tool's gizmo, measured at 0.017 ms per frame without
   one ([PLUGINS.md](PLUGINS.md) §3.8), and on OpenUSD's threads taking the GIL.
5. **Generating `pxr` stubs** (phase 4). `pybind11-stubgen` may not understand OpenUSD's Python
   bindings, which aren't pybind11; mypy's `stubgen` is the other candidate. How good the result is,
   and whether it needs corrections by hand.
6. **PySide6's `.pyi` files** (phase 4): whether its generator runs on our build, in CI.
7. **The standard library opening the pipe** from `usdcc_send.py`, as a file on Windows and a
   socket elsewhere (phase 1).
8. **Updating with VS Code open** (phases 3 and 4). Its Python support and debugger may run the
   installed `python\python.exe`, which an update has to replace ([UPDATES.md](UPDATES.md)). If
   that blocks it: another interpreter for VS Code, with the app's on `extraPaths` only.
9. **Stopping a script from another thread** (phase 5), and whether the exception can land after the
   run has ended.

## 10. Left out

- **VS Code on another machine, in WSL or in a container.** The pipe is local, and the paths in
  tracebacks and breakpoints are the app's.
- **Other editors.** The link and `usdcc_send.py` are there for them, and nothing else is built.
- **Language servers other than Pylance.** Pyright-based ones read the same paths from a
  `pyrightconfig.json`, which **Set Up VS Code…** doesn't write.
- **A stage editor in VS Code:** a stage tree, or a variable viewer outside the debugger.
- **A USDA language server of our own.**

## 11. Where the code will be

- **`scripting/editor_link.h`:** `EditorLink`, in `usdcc_scripting_app`. `main.cpp` creates it
  after the `ScriptRunner` and `PluginManager`.
- **`scripting/script_runner.h`, `python_runtime.h`:** the first line of a run.
- **`scripting/python_settings.h`:** `Python/editorLink`, `Python/debugPort`.
- **`scripting/script_editor_side_panel.h`, `plugin_manager_side_panel.h`:** **Allow Debugger to
  Attach**, **Set Up VS Code…**, **Open in VS Code**.
- **`src/python/usdcc/debug.py`,** the type hints in its neighbours, and `_usdcc_app.pyi`.
- **`cmake/modules/UsdccDebugpy.cmake`, `tools/deploy.py`:** `debugpy`, the stubs, the manifest schema,
  the `.vsix`.
- **`tools/usdcc_send.py`.**
- **`editors/vscode/`:** the extension.
- **`tests/editor_link_test.cpp`,** and the debug-protocol client beside it.
