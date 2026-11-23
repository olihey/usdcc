# VS Code (plan)

Write usdcc scripts and plugins in VS Code, and run and debug them in the running app: a small
server in usdcc, type stubs, `debugpy`, and a thin extension on top.

**Status:** planned 2026-11-16, updated 2026-11-23. Phase 0, the spikes, is done (§9), and
**phases 1 to 5, all but shipping it,
are built** (§12 to §16). Facts marked *checked* were
looked up on one of those days; everything else is expected, and §9 says what was tried.

**What the update changed:**

- A small extension comes right after the link, not last, and there are more phases (§7).
- More features: starting usdcc with F5, templates, manifest checking, Problems, a Plugins view,
  Stop (§6.2). Later ones are in §6.3.
- The part shared with [MCP.md](MCP.md) is named (§3.4), and the link has a build option (§3.1).
- Completion needs stubs of our own for `pxr` and PySide6 (§4).
- Phase 0 changed four things: VS Code must not be pointed into the installed app (§4, §9 #8),
  `pxr` stubs can't be generated with types (§4), the link gets a thread of its own (§3.3), and the
  debugger does reach native threads (§5).

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
  A pipe can be opened from another computer by someone logged on as this user, so a connection
  from one is closed.
- **No port to choose, and no firewall prompt.**
- **Node opens one natively** (`net.connect("\\\\.\\pipe\\…")`), so the extension needs no native
  module.

**No password or token.** A process running as the user can already put a file in the startup
scripts folder, which the app runs at its next start. The pipe gives it nothing new.

**Finding the app.** Each usdcc names its pipe `usdcc-editor-<pid>`. A client lists the pipes with
that prefix (`\\.\pipe\` on Windows; `$XDG_RUNTIME_DIR/usdcc/` elsewhere, or `usdcc-<uid>` in the temporary folder, where a leftover socket
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
off, `EditorLink`, the setting and `--no-editor-link` aren't compiled in.

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
| `hello` | | `api` (this protocol's number, 1), usdcc's version, `pid`, the current stage's name, Python's version, `python.executable` and `python.paths` (§4), `busy`, `debugPort` (0 when no debugger can attach) |
| `run` | `code`; `file` or `name`; `line` (where `code` starts in `file`, default 1); `scope`: `fresh` or `session` | `ok`, `ms`, `undoSteps`, and on a failure `error`: the exception's type and message, the innermost frame's file and line, and all `frames` |
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
- **On the main thread, one at a time.** The link has a thread of its own, which reads requests
  and queues each `run` and `reloadPlugin` to the main thread, so one sent while a script runs can be
  answered `busy` at once, and `stop` (§6.2) can be seen at all.
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
  bookkeeping records. The sink hands it to the link's thread and never waits for the client. What
  a client that has stopped reading is owed is capped: past 16 MB, output is dropped, with a line saying so
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
| `python.executable` | None: the user's own choice stays | The Python OpenUSD was built with |
| `python.paths` | The type folder (below), the shared packages environment, the additional package folders, the user's `scripts/` | Those, plus `build/python` |

They go into the workspace's `.vscode/settings.json` as `python.analysis.extraPaths`, and in a build
`python.defaultInterpreterPath`, merged with what's there.

**Nothing in VS Code may point into an installed usdcc.** An open file or a watched folder anywhere
in the install's `current` folder makes an update fail (§9 #8), and a language server watches the
folders it reads. So usdcc copies what's needed for completion into
`%LOCALAPPDATA%\usdcc_config\types`: its own `usdcc` package, and the stubs. It is brought up to date
after a start, file by file, and only if it's there: if **Set Up VS Code…** or Set Up Workspace has been used. Pylance has the
standard library's types itself, so no interpreter from the install is needed; that and the attach
are still to be seen in VS Code (§9).

**Types, where they're missing:**

- **`usdcc`'s own functions get type hints.** The modules users import are plain Python
  (`usd.py`, `app.py`, …) with docstrings but no annotations, and each forwards to `_usdcc_app`,
  which no analyzer can read as it's compiled into the exe (*checked*). So `current_stage()` is
  annotated `-> Usd.Stage | None` and so on. About 280 functions, none annotated
  when this was planned. This is most of the work, and it improves the docs too. It can be done a module at a
  time: `usd`, `app` and `ui` first, then `tools`, `render` and the rest.
- **`_usdcc_app.pyi`,** for the classes that come straight from C++: `StageManager`, `UndoManager`,
  `Viewport`, `ViewportCamera`, `RendererSetting`. Generated with `pybind11-stubgen` run inside
  the app, kept in the repository beside the package, and deployed with it. A test in
  `usdcc_python_tests` generates it again and fails when the two differ.
- **`pxr` has no stubs in this OpenUSD build,** and nothing to generate good ones from: of 1955
  methods in `Usd`, `Sdf` and `Gf`, 3 have a signature in their docstring (*checked*; OpenUSD 26.8).
  Generated stubs have every name and no types, so completion stops after one call:
  `Usd.Stage.CreateInMemory()` is Unknown to Pyright. The third-party `types-usd` (MIT) has the types
  (`Stage`, then `Prim`; *checked*) but stops at 24.5.2, and lacks 2066 of 7850 names.
  **So both, merged:** `types-usd` for what it knows, names without types for the rest, and names that
  are gone removed. A script does it as usdcc is built, from a pinned `types-usd`, so the stubs are always
  of the OpenUSD it's built with.
- **PySide6's `.pyi` files aren't in the deploy,** as `tools/build_pyside.py` passes `--disable-pyi`.
  Its generator works on our build (§9 #6), so it's run after the build instead.
- **All stubs ship in the deploy,** in `python\stubs`, and are never on the app's `sys.path`. What VS
  Code reads is the copy above. Nothing is downloaded.

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
  `sys.executable`, which in the app is `usdcc.exe` (*checked*). A deployed app has
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
- **Breakpoints stop native threads too:** one hit in a callback run on a thread Windows made
  stopped there (*checked*). Expected of OpenUSD's threads as well.
- **Code in a file with a breakpoint** is about 3 to 4 times slower. Elsewhere, attaching costs
  nothing measurable (§9 #4). This matters for a tool's `build_gizmo()`, which runs every frame.

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
- **A JSON schema for `usdcc-plugin.json`:** completion and errors while typing. It's part of the
  extension, and a test checks every manifest in this repository against it.
- **Problems.** A failed run marks its line, and a plugin that failed to load marks the line that
  raised. Both are cleared by the next run or reload. Tracebacks in the output are links.
- **A Plugins view:** what `plugins` returns, each with its state and reason, and Reload and Open
  Folder.
- **Stop.** The pipe has to be read off the main thread, to see a `stop` request while a script runs.
  It raises `KeyboardInterrupt` in the main thread between two Python instructions
  (`PyThreadState_SetAsyncExc()`; MCP.md §4 item 5). It can't break a long call into C++. The Script
  Editor gets a Stop button from it too. Tried, with the rule that keeps a late one from landing in
  other code (§9 #9).
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
| **0. Spikes (done 2026-11-23)** | S | | §9's items | Each has its answer in §9, and this plan is changed where it's wrong |
| **1. The link (done 2026-11-23, §12)** | M | 0 | The shared run core (§3.4) except Stop, `EditorLink`, the protocol without `debugListen`, the setting, `--no-editor-link`, the build option, `ScriptRunner`'s first line, `tools/usdcc_send.py` | Tests in `usdcc_python_tests`, over a real pipe: a run is one undo step and its output arrives before its result; a failing run returns its error and leaves a "(failed)" step; a selection's traceback has the file's line numbers; `busy` and `dialog`; two clients; `reloadPlugin` by id and by path; nothing listens when turned off. A build with the option off compiles and starts. In the app: `usdcc_send.py` against a deployed usdcc |
| **2. First extension (done 2026-11-23, §13)** | M | 1 | `editors/vscode`: Run File and Selection, kept variables, output with traceback links, Reload and reload on save, Connect, status bar, a CI job that builds the `.vsix` | Its protocol code against a fake server in Node. By hand: edit a plugin, save, see it change; with one app and with two |
| **3. Debugging (done 2026-11-23, §14)** | M | 0; the test, `debugListen` and Attach need 1 and 2, as does F5 | Bundled `debugpy`, `usdcc.debug`, the menu entry, `debugListen`, Attach, then starting usdcc (F5) | A small debug-protocol client in the tests: attach, set a breakpoint, run a script over the link, see it stop there, continue, and the app still exits with code 0. By hand: a breakpoint in a plugin's menu action, and F5 from nothing |
| **4. Completion (done 2026-11-23, §15)** | L | 0; Set Up Workspace needs 2 | Type hints in `src/python/usdcc`, `_usdcc_app.pyi` and its test, the `pxr` and PySide6 stubs, **Set Up VS Code…**, **Open in VS Code**, Set Up Workspace | Pyright from the command line over the example plugin and a sample script, with the written settings: no unresolved imports, and `usdcc.usd.current_stage()` is a `Usd.Stage`. By hand: completion in VS Code |
| **5. Plugin workflow (done 2026-11-23, §16)** | M | 2 | §6.2 except F5: templates, the manifest schema, Problems, the Plugins view, Stop, snippets | Every manifest here passes the schema; a stopped endless loop leaves a "(failed)" step; the extension's tests. By hand: a working panel within five minutes of installing |
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
| 9 | `pxr` stubs | `types-usd` 24.5.2 merged with names from the shipped OpenUSD (§4) |
| 10 | The profile F5 starts usdcc with | A development one, through `USDCC_TEST_PROFILE`. Its pipe isn't listed to editors (§3.1), so F5 connects to the process it started, by its pid and profile |
| 11 | C++ plugins | Later (§6.3): Python is the easy path |

## 9. What was tried

Phase 0, on 2026-11-23, on Windows x64: the deployed app of 2026-11-22 (`dc6012a`) with a test profile,
offscreen; `debugpy` 1.8.22; Python 3.13; OpenUSD 26.8.

| # | Question | Answer |
|---|---|---|
| 1 | Does a write reach the client while the server's thread is busy? | **Yes.** `QLocalSocket::write()` alone arrived in 0 to 2 ms, with the thread busy and no event loop; `bytesToWrite()` isn't updated meanwhile. `waitForBytesWritten()` isn't needed, and must not be used: a client that doesn't read held it for its whole timeout (506 ms of 500). Without it the bytes queue, so they're capped (§3.3) |
| 2 | Does `debugpy.listen()` work in the app? | **Yes,** in 0.6 to 1.8 s, with `configure(python=…)` naming the deploy's `python.exe`. Breakpoints in a script stopped and continued, and detaching worked. `sys.executable` is `usdcc.exe`, so without `configure` it would start another usdcc (not tried) |
| 3 | Does the app still exit cleanly? | **Yes,** code 0 when never attached to, still attached, and after a detach. No helper process was left |
| 4 | What does an attached debugger cost? | A gizmo-like function took 0.033 ms with none, 0.033 ms listening or attached, and 0.10 to 0.13 ms with a conditional breakpoint in its own file. A real tool's gizmo wasn't measured |
| 5 | Can `pxr` stubs be generated? | **Names only.** `pybind11-stubgen` and mypy's `stubgen` both run, and neither gets types. `types-usd` has them, with 26% of the names missing (§4) |
| 6 | Do PySide6's stubs generate on our build? | **Yes:** `generate_pyi.py` made QtCore, QtGui and QtWidgets, and Pyright read them (`QLabel.text()` is `str`). Not run in CI |
| 7 | Can the standard library open the pipe? | **Yes:** `open()` on the pipe's path, and `os.listdir(r"\\.\pipe")` finds it by prefix |
| 8 | Does VS Code block an update? | **It can.** With Velopack's updater on a small package: a `python.exe` of the install is **ended by the updater**, and the update goes through. A terminal whose folder is inside, an open file (even shared for deletion), or a folder watched by Node makes it **fail**, leaving the old version working. So VS Code gets copies (§4) |
| 9 | Can a script be stopped from another thread? | **Yes:** endless Python and `pxr` loops, and loops with `except Exception`, within about 0.05 s; a C call (`time.sleep(2)`) only when it returns. A late one **does land in other code:** 468 of 600 without a guard. With the run's number checked by the stopping thread, and the pending exception cleared with `PyThreadState_SetAsyncExc(main, NULL)` before the GIL is let go after the run, 131 of 131 stopped the run and none of the 600 leaked |

**#9's rule:** the run's number is read and written only with the GIL held, and nothing in Python runs
between the end of the script and the clearing.

**Not tried:**

- **VS Code itself.** The client was a small debug-protocol script, and completion was Pyright's. That
  Pylance needs no interpreter from the install, and that attaching keeps nothing open in it, are
  the first things to check in phases 3 and 4.
- **Linux and macOS,** where `QLocalSocket` may need `flush()` or the event loop (#1): the link's own
  thread covers it.
- **Windows on ARM,** where `debugpy` has only its pure-Python wheel.
- **usdcc's own update,** rather than the small package's.

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
- **`scripting/script_runner.h`, `python_runtime.h`:** the first line of a run, and stopping one.
- **`scripting/python_settings.h`:** `Python/editorLink`, `Python/debugPort`.
- **`scripting/script_editor_side_panel.h`, `plugin_manager_side_panel.h`:** **Allow Debugger to
  Attach**, **Set Up VS Code…**, **Open in VS Code**.
- **`src/python/usdcc/debug.py`,** the type hints in its neighbours, and `_usdcc_app.pyi`.
- **`cmake/modules/UsdccDebugpy.cmake`, `tools/deploy.py`:** `debugpy`, the stubs, the manifest schema,
  the `.vsix`.
- **`tools/usdcc_send.py`.**
- **`editors/vscode/`:** the extension.
- **`tests/editor_link_test.cpp`,** and the debug-protocol client beside it.

## 12. Phase 1, as built

- **The pipe's thread** answers `hello` and refuses with `busy` by itself; `run`, `reloadPlugin`,
  `resetSession` and `plugins` are queued to the main thread. Nothing waits for a client.
- **A second run is refused, not queued:** `busy` at once while one is queued or going on, whoever
  started it. An open menu counts as a dialog. `plugins` only reads, so a dialog doesn't stop it.
- **Run Selection in the Script Editor** has the file's line numbers in its tracebacks now.
- **A run from a file is named after it** in Undo ("Run setup.py"), in the Script Editor too, which had
  its whole path.
- **Identical lines in a row** all show, in the link and in the Script Editor's output. The log
  collapses them, and the output lost them.
- **`hello`'s `python.paths`** leaves out a deploy's own packages until phase 4's type folder exists.
- **`tools/usdcc_send.py`:** `list`, `run`, `reload`, `plugins`, `reset`.

**Tested** by `editor_link_test.cpp` over a real pipe (what §7 asks, a client that leaves mid-run, one that
doesn't read). With a deployed app, offscreen with a test profile: `usdcc_send.py` listing, running, a failing
script, reloading V-Ray Render, two apps at once, a script that quits the app, and `--no-editor-link`. With
the option off: the build, its Python tests, "Left out of this build", and the app starting with no pipe.

**Not tested:** Linux and macOS; a connection from another computer; the setting by hand in Settings; a run
arriving while the mouse drags in a tool, which isn't refused.

## 13. Phase 2, as built

`editors/vscode`, with no dependencies at run time: `link.ts` (the protocol), `connection.ts` (finding an
app and staying connected), `text.ts` (what's sent and how output reads) and `extension.ts` (VS Code).

- **Commands:** Run File, Run Selection, Reload Plugin, Connect, Keep Variables Between Runs, Reset
  Variables, Show Output. §6.1's Attach and Set Up Workspace come with their phases.
- **Run File in a plugin's folder** saves the plugin's files and reloads it; so does Reload Plugin.
- **The app is asked `hello` every 2 s** while VS Code's window has the focus, which is how the status bar
  follows the stage. There's no event for it.
- **Only in a trusted workspace,** as it runs the workspace's code.
- **`usdcc.testProfile`** (a machine setting) connects to a usdcc with a test profile.
- **Traceback links** need the output's language declared in `package.json`.
- **`npm run package`** makes the `.vsix`. CI (`vscode.yml`) runs the tests on Windows and Linux when the
  extension changes, and keeps the `.vsix` as an artifact.

**Tested:** 13 Node tests against a server that plays usdcc. `npm run e2e` starts the installed VS Code
(1.147, a profile of its own) and a real usdcc (offscreen, a test profile), and goes through: unsaved text
run as one undo step; a failing selection moving the cursor to the file's line; the output and its links;
kept and reset variables; reloading a plugin, a broken one, and on save; a script that quits the app.

**Not tested:** the keys, the status bar and picking an app by hand; installing the `.vsix`; the workflow on
GitHub; Linux, whose sockets only the workflow's tests will try.

**Known:** a script that opens a dialog (quitting with an unsaved stage does) keeps the run going until
the dialog is answered in usdcc. VS Code shows it as running, and says nothing about the dialog.

## 14. Phase 3, as built

- **`debugpy` 1.8.22** in `build/debugpy` and the deploy's `tools\debugpy`, 5.7 MB: without what's only for
  building it and for attaching to a process from outside. Not in a deploy with Blender mesh tools, whose
  review it isn't in.
- **`usdcc.debug`:** `listen()`, `is_listening()`, `port()`, `is_attached()`. Processes that scripts and
  plugins start aren't debugged (`subProcess=False`). The helper gets the app's folder on `PATH`, for
  `python.exe`'s DLL.
- **Allowing it:** ☰ > **Allow Debugger to Attach** in the Script Editor, `debugListen`, or a script. The port is
  the setting **Debugger port**, or a free one when that's taken.
- **`debugListen` while already listening** is answered at once, so an editor can attach to a script that
  is running.
- **The extension:** **usdcc: Attach Debugger**, **usdcc: Start usdcc**, and a `usdcc` debug type with `launch`
  and `attach`, offered by F5 with no `launch.json`. It hands over to VS Code's Python Debugger, and
  offers to install that when it's missing, rather than depending on it: running needs none of it.
- **`launch`** starts `bin\usdcc.exe` (the launcher would be another process) with the workspace's plugins
  on `--plugin-path` and the profile `vscode`. **Stopping the debugger leaves usdcc running,** as it may have unsaved
  work, and F5 again attaches to the same one. `"profile": ""` uses the user's own profile.

**Tested:** `usdcc_editor_debug`, a CTest of its own: attach over the link, stop at a breakpoint with the path
written as VS Code writes it, `hello` while stopped, a selection stopping at the file's line, detaching, and
exit code 0. In a deployed app with a bare `PATH`: the bundled `debugpy` is the one loaded, a breakpoint stops,
it quits with code 0 while attached and leaves no helper. `npm run e2e`, in VS Code with Microsoft's Python
extensions: attach, a breakpoint in a script and in a plugin's menu action, F5 starting another usdcc with the
workspace's plugin.

**Not tested:** the menu entry and the setting by hand; a taken port; Linux, macOS and Windows on ARM, where `debugpy` is the pure
Python one; an update with the debugger attached (§9 #8).

## 15. Phase 4, as built

- **Type hints** on 249 functions of `usdcc`'s modules, and the attributes of classes with `__slots__`. They aren't
  evaluated (`from __future__ import annotations`), and what only they need is imported for type checkers alone, so
  nothing more is loaded at run time.
- **`src/python/stubs/_usdcc_app.pyi`** comes from pybind11's docstrings (`tools/app_stub.py`), rather than from
  `pybind11-stubgen`, which the tests would need installed. `app_stub_is_current` compares it with the bindings;
  `USDCC_UPDATE_STUBS=1` writes it again.
- **`build/stubs`** (`tools/build_stubs.py`, 4.5 MB, under 3 s): `pxr`, merged as §4 says, PySide6's and shiboken6's,
  and `_usdcc_app`. What can't be made is left out with a warning. Of 7850 `pxr` names counted in 18 modules, 35
  are missing; `types-usd` alone lacks 2066. Names it doesn't know are `Any`.
- **The deploy** has them in `python\stubs`; not with Blender mesh tools, as `debugpy`.
- **`setUpTypes`** in the protocol brings the type folder up to date and says where to look. `hello` lists it too.
- **Set Up VS Code…** and **Open in VS Code** in the Script Editor's ☰ menu and on a plugin in View > Plugins. A
  `.vscode` file with comments isn't rewritten, as they'd be lost: the app says so. The extension's **usdcc: Set Up
  Workspace** goes through VS Code, which keeps them.
- **An installed usdcc names no interpreter,** so a workspace keeps the user's own. A build names OpenUSD's Python, unless
  one is set.

**Tested:** Pyright with a build's paths and with a deploy's type folder: no unresolved imports,
`usdcc.usd.current_stage()` is `Stage | None`, `GetPrimAtPath()` a `Prim`, and wrong names are errors. In the example
plugin it reports two uses of a global that may be None, and nothing else. `npm run e2e`: Set Up Workspace,
then Pylance completing a prim's methods. The type folder in a deploy: made in 0.9 s, 0.04 s when nothing changed, and
put right when changed or stale. The stub and folder tests.

**Not tested:** the menu entries by hand; the stubs on Linux and macOS; an update with VS Code open on a workspace set up this way,
though none of the paths written are inside the install.

**Before a release:** `types-usd` 24.5.2 says MIT in its metadata, without a licence file, and its repository says
Apache-2.0 now. The deploy ships the metadata; whether that's enough is to be decided.

## 16. Phase 5, as built

- **Stop** (`stop`, **usdcc: Stop Script**, a status bar button while something runs) raises `KeyboardInterrupt` in what
  the main thread runs: a script, whoever started it, or a plugin's `activate()`. It uses `Py_AddPendingCall()`, not the
  `PyThreadState_SetAsyncExc()` of §6.2 and §9 #9: Python calls back on the main thread between two instructions, where the
  run's number is checked. So the link needs no GIL, a script inside a long C call doesn't hold it up, and a late stop
  does nothing.
- **No Stop button in the Script Editor,** which §6.2 promised: while a script runs, the app's window takes no clicks.
- **Templates:** `src/python/usdcc/_templates`, a menu action, a prim menu action, a panel, a tool and a final renderer. The app's
  **New Plugin...** asks for one, and **usdcc: New Plugin...** uses them through the app (`templates`, `createPlugin`), so it
  needs a running usdcc.
- **`addPlugin`** takes in a plugin's folder the app doesn't know, with no restart. Reload Plugin uses it when the
  folder isn't known. It lasts until usdcc quits: at the next start it's found only in a folder usdcc looks in, as F5's
  `--plugin-path` sees to.
- **The manifest's schema** is in the extension (`schemas/`). A Node test checks the manifests in this repository, and what the
  app refuses.
- **Problems:** a failed run marks its line, until the next run or reload. A plugin that failed marks the line of its own
  that raised; `plugins` gives it as `file` and `line`.
- **The Plugins view,** an icon of its own in the activity bar, with each plugin's state and reason, Reload and its folder. Refreshed by
  `plugins-changed`, sent to every connected client.
- **Snippets,** all starting with `usdcc-`.

**Tested:** `editor_link_test.cpp`: an endless loop stops and leaves a "(failed)" step; a loop that takes `Exception` stops; of 120 stops around a
run's end, some in time and some late, none reaches other code; every template starts as it is; where a plugin failed; the event. In
a deployed app: the panel opens and counts prims, the tool is activated with a prim selected, the prim menu's function hides a
prim, and the renderer renders a frame to a file. `npm run e2e`: problems, stopping, New Plugin, taking in a folder, and the
manifest's problems as it's typed. 20 Node tests.

**Not tested:** the Plugins view's buttons, the Stop button and New Plugin's questions by hand, and the app's new dialog; a tool's drag;
stopping while a debugger is paused; "a working panel within five minutes of installing", which needs phase 6; Linux and macOS.
