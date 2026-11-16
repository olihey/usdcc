# MCP Server (plan)

An MCP server inside usdcc, so an AI client (Claude Code, Claude Desktop, Cursor, …) can see and
drive the running app: read its stages, look at the viewport, and edit, with every edit undoable.

**Status:** investigated 2026-11-16. Nothing is built yet. Facts about the code were read in this
checkout; facts about MCP come from the specification's 2026-07-28 revision (§10).

## 1. Goal

- **See:** the open stages, the selection, a prim's attributes, the layer stack, the log, and a
  picture of the window or a viewport.
- **Do:** run Python in the app's interpreter, where `pxr` and `usdcc.*` already reach everything a
  script can ([SCRIPTING.md](SCRIPTING.md) §4.1). One call is one undo step, like one Run.
- **Stay in control:** off until turned on, this machine only, a token, an access level, and every
  call visible in the Log panel.
- **Also for building usdcc itself.** An agent working on this repository can start the app with a
  test profile and check its change by state and screenshot. Several plans here end with "the
  Windows session was locked, so it wasn't looked at"; a capture through the app doesn't need the
  screen (§3).

**Exit criterion:** from Claude Code, connected to a running usdcc: "open `cube_and_sphere.usda`,
make the cube twice as big, and show me". The viewport picture comes back, and one Edit > Undo in
the app reverts the change.

## 2. What MCP asks for today

The current revision, **2026-07-28**, made the protocol stateless, which makes a server much
smaller than it was a year ago:

- **One endpoint, POST only.** Each JSON-RPC request is its own POST, and the reply may be one
  plain JSON object. No session, no `initialize` handshake, no GET stream.
- **Each request names its version** in `_meta` and in the `MCP-Protocol-Version` header. The
  server checks that `Mcp-Method` and `Mcp-Name` match the body (400, error -32020, if not).
- **`server/discover` is required:** versions, capabilities, name, and `instructions` for the model.
- **Results carry `resultType`**, and lists carry `ttlMs` and `cacheScope`.
- **Security the spec requires:** an invalid `Origin` is refused with 403; bind to 127.0.0.1; it
  "should" authenticate.
- **Cancelling** is the client closing the reply's connection.
- **Deprecated or moved out,** so not built: Logging, Sampling, Roots, HTTP+SSE. Tasks, for long
  jobs, are now an extension.
- **Older clients** (2025-11-25 and before) open with `initialize`. A server may serve both; a
  server that doesn't just fails for them, with no fallback.

There is **no official C++ SDK** (TypeScript, Python, Go, Kotlin, Swift, Java, C#, Ruby, Rust and
PHP have one). The Python SDK is `mcp` 2.3.0, which needs pydantic, starlette, uvicorn, anyio,
httpx2, pyjwt, opentelemetry-api, jsonschema, sse-starlette, and pywin32 on Windows.

## 3. What usdcc already has

| Need | There today |
|---|---|
| Everything a tool does | The embedded interpreter with `usdcc.usd`, `usdcc.app`, `usdcc.tools`, `usdcc.settings`, `usdcc.physics`, and `pxr` |
| One undo step per call | `ScriptRunner`: a global edit around a run, "(failed)" when it raised |
| A run's output | `RunCapture`, a log sink attached for the run (in `script_editor_side_panel.cpp`) |
| The log | `LogHub::snapshot()`, 10 000 records with a sequence number each |
| A picture without the screen | `captureWindowPreview()` and `HydraViewportWindow::grabFrame()` ([SESSIONS.md](SESSIONS.md) §6.2) |
| HTTP | `Qt6Network.dll` is already built by vcpkg (a default feature of `qtbase`); nothing links it yet |
| JSON | `QJsonDocument`, used in nine files |
| A clean profile | `USDCC_TEST_PROFILE` moves settings, plugins and layouts |
| Calls without dialogs | The bindings (`app_module.cpp`, `usd/python`) open no message box or file dialog; `load_session(discard_unsaved=False)` refuses instead of asking. A plugin's menu action still can |

## 4. What's missing

1. **A server:** the endpoint, the protocol, and the list of tools (§5).
2. **A way onto the main thread.** Scripts, stages and widgets are main-thread only
   ([SCRIPTING.md](SCRIPTING.md) §3.3). Requests arrive on another thread and are queued.
3. **`ScriptRunner` returning what happened:** the output, the traceback, the last expression's
   value and the undo steps, with the step's name given by the caller. `RunCapture` moves out of
   the Script Editor to be shared.
4. **Python can't yet ask for** a picture, the log's records, the menu's actions, or whether a
   dialog is open. Four small bindings in `_usdcc_app`.
5. **Stopping a script.** There is none ([SCRIPTING.md](SCRIPTING.md) §10), and an AI will write a
   loop that doesn't end. `PyErr_SetInterrupt()` can't be used: the runtime starts Python with
   `install_signal_handlers = 0`. `PyThreadState_SetAsyncExc()` on the main thread's state, from
   the server's thread, raises between two bytecodes; it can't break a long call into C++ (a stage
   open). *Not tried yet.* It would give the Script Editor its Stop button too.
6. **Settings, a command-line switch and a status-bar mark** (§7).
7. **A stdio bridge,** for clients that only start a local program (§6).

## 5. Design

### 5.1 The server is C++, the tools are Python

**`usdcc_mcp`** (new, static, in the exe like the scripting libraries; Qt Network): the endpoint,
JSON-RPC, the checks of §2, the token, the queue. It is small because of §2: a POST in, a JSON
object out.

**Not the Python SDK,** although it is the official one:

- **It would bring about fifteen packages** into the one environment all plugins share
  ([PYTHON_PACKAGES.md](PYTHON_PACKAGES.md)): pydantic's compiled core, pywin32, a web server. A
  plugin pinning another pydantic would then be refused, or refuse the server.
- **Packages are never installed at startup** and need a restart, so the feature wouldn't work
  out of the box.
- **It needs a second event loop on a Python thread** inside the embedded interpreter, plus a way
  back to the main thread that today only PySide gives, and Linux has no PySide yet.

The price: the protocol is written by hand and followed by hand when it changes. The official
conformance suite (§9) is the check.

**Tools are Python functions,** in a new `usdcc.mcp` module, on top of `pxr` and `usdcc.*`:

```python
import usdcc.mcp

@usdcc.mcp.tool(read_only=True)
def list_prims(root: str = "/", depth: int = 2, type: str = "", limit: int = 200, stage: int | None = None):
    """The prims under `root`: path, type, kind, active, child count."""
    ...
```

- **Listing and inspecting prims is a few lines of `pxr`** and would be many in C++.
- **The name, description and input schema come from the function:** its signature and docstring.
- **A plugin can add its own tools** the same way (V-Ray: `render`), attributed to it and removed
  by a reload, like `add_menu_action()` ([PLUGINS.md](PLUGINS.md) §2).
- **A tool returns** something JSON can hold, or `usdcc.mcp.Image(bytes, "image/png")`. An
  exception becomes a result with `isError` and the traceback, so the model can correct itself.

### 5.2 Threads

```
client ──POST──▶ server thread ──queued──▶ main thread: one call at a time
                 (always answers)           global edit → tool → end edit → result
```

- **The server has its own thread,** so it still answers while the main thread runs a script:
  `server/discover` and `tools/list` at once, and a second call with "busy: running <name>".
- **One call at a time.** A call that arrives inside another (a script pumping events) is refused.
- **A timeout** (`timeout_s`, 60 by default): the reply says the call is still running. It isn't
  killed; closing the connection asks for the interrupt of §4 item 5.
- **Dialogs.** Queued calls are also delivered while a modal dialog runs its own event loop, where
  a script could close the stage the dialog is about. So while one is open
  (`QApplication::activeModalWidget()`), only the reading tools and the screenshot work, and the
  rest answer "a dialog is open: <title>". A script that opens one itself waits for the user, and
  the timeout says so.

### 5.3 Undo

Every tool not marked `read_only` runs in one global edit, as `ScriptRunner` does. The step is
named by the call: `run_python` takes a `description` ("Scale the cube"), shown as
"AI: Scale the cube" in Edit > Undo. A failed call leaves "… (failed)", and one Undo reverts it
([SCRIPTING.md](SCRIPTING.md) §4.3).

### 5.4 The tools

Few on purpose: every tool's description is sent to the model with every conversation.

| Tool | Does | Phase |
|---|---|---|
| `get_app_state` | Version, stages (id, file, unsaved), current stage, selection, time and range, tool, viewports and renderers, workspace, undo and redo texts, an open dialog, busy | 1 |
| `list_prims` | Prims under a path to a depth, by type or name, in pages | 1 |
| `get_prim` | Type, schemas, metadata, attributes (value shortened, authored or not, time samples), relationships, variant sets, arcs, world bounds | 1 |
| `get_layers` | The layer stack, the edit target, unsaved layers | 1 |
| `get_log` | Records since a sequence number, from a level up | 1 |
| `screenshot` | The window, a viewport or a panel as an image, scaled down to a longest side | 1 |
| `python_api` | The signatures and docstrings of a `usdcc` module, read from the running app, so it matches the build and its plugins | 1 |
| `run_python` | Runs code as one undo step. Returns the output, the last expression's value, the traceback, warnings logged meanwhile, and the undo steps | 2 |
| `undo`, `redo` | The current stage's stack | 2 |
| `list_actions`, `invoke_action` | The main menu's entries by path, with enabled and checked; triggers one. Reaches what has no Python API, such as the Mesh menu | 3 |

**No `set_attribute`, `create_prim`, `select`, `open_stage` tools at first.** Each is one line in
`run_python`. They come later if transcripts show the model getting them wrong (§11 #7).

`server/discover`'s `instructions` tell the model the few things it can't guess: stages are
`pxr.Usd.Stage`s from `usdcc.usd.current_stage()`, edits are undoable, look with `screenshot`
after changing something, ask `python_api` before guessing a function.

## 6. Reaching it

- **`http://127.0.0.1:47811/mcp`** (the port is a setting). A second usdcc finds it taken and uses
  the next free one.
- **Each running app writes** `%LOCALAPPDATA%/usdcc/mcp/<pid>.json`: port, pid, version, started.
  A test profile has its own folder, so test runs and the real app don't find each other.
- **Both eras.** The 2026-07-28 form, and the older `initialize` opening without a session, which
  is a few more replies. Which form today's clients send is to be checked (§11 #3).
- **Clients that speak HTTP,** such as Claude Code:

  ```bash
  claude mcp add --transport http usdcc http://127.0.0.1:47811/mcp --header "Authorization: Bearer <token>"
  ```

- **Clients that only start a program** (Claude Desktop's local servers are stdio): a small
  console program, **`usdcc-mcp.exe`**, since `usdcc.exe` is a GUI program without standard
  streams. It passes lines between stdin/stdout and the app's endpoint, finds the app through the
  files above, reads the token itself, and can start usdcc when none runs. It also outlives a
  crash of the app, so the client is told "usdcc isn't running" instead of losing its server.

## 7. Safety

**`run_python` is the user's full rights:** files, network, processes. That is what a script has
too, but here the text comes from a model, and the model reads stages. A USD file from elsewhere
can hold text written to steer it. Nothing in the app can rule that out, so:

- **Off by default.** Edit > Settings > AI: **Let AI clients connect** (`Mcp/enabled`), the port,
  and the access level. `usdcc --mcp[=read|full]` turns it on for one run without changing the
  setting, for test runs.
- **Two access levels:** **Read only**, the default (the phase 1 tools), and **Full** (everything).
  Tools are marked read-only or not in `tools/list`, so a client can ask before the others.
- **Loopback only, and a token.** Another user of the same machine can reach 127.0.0.1, and so can
  a web page in a browser. So: `Origin` checked, `Content-Type: application/json` required, and a
  bearer token, made once per profile, with **Copy** and **New Token** in the settings, and
  **Copy Client Setup** for the line in §6.
- **Seen:** every call is a record with the source `mcp` in the Log panel (tool, client name,
  time taken); a mark in the status bar while a client has called in the last minute; a toast
  the first time a client connects in a run.
- **Never from a stage or a plugin:** only the setting or the switch turns it on.

## 8. Phases

| Phase | Builds | Exit |
|---|---|---|
| **1. Server, reading** | `usdcc_mcp`, both eras, token and `Origin`, the queue, settings and `--mcp`, `usdcc.mcp`, the bindings of §4 item 4, the phase 1 tools | Claude Code describes an open stage and shows its viewport, with the screen locked |
| **2. Editing** | `ScriptRunner` results, `run_python`, undo and redo, access levels, the dialog rule, timeout, the interrupt, the status-bar mark | §1's exit criterion |
| **3. Menu and plugins** | `list_actions`, `invoke_action`; tools from plugins, shown in View > Plugins | A plugin's tool is called; Mesh > Triangulate runs from a client |
| **4. stdio bridge** | `usdcc-mcp.exe`, the instance files, starting the app, the deploy | Claude Desktop drives a deployed usdcc |

1 → 2 in order; 3 and 4 need only 1.

**Later:** mouse and keys sent to a widget, for checking a tool's drag (the "not checked" of
[PLUGINS.md](PLUGINS.md) §5); the tasks extension for bakes and renders; stages as resources with
change notices; structured edit tools.

## 9. Build and testing

- **`src/cpp/mcp/`** (namespace `usdcc::mcp`), `src/python/usdcc/mcp.py`, and later
  `src/cpp/mcp_bridge/`. `tools/deploy.py` adds Qt's network DLLs.
- **Tests,** in `usdcc_python_tests`, posting JSON to the server in the same process:
  - both openings; a header that doesn't match (400, -32020); an unknown method (404); a wrong
    `Origin` (403); no token (401); GET (405);
  - Read only doesn't list or run `run_python`;
  - `run_python` is one named undo step, and a raising one is a "(failed)" step with its traceback
    in the result;
  - `screenshot` returns a PNG of the offscreen window;
  - a call during another is refused, and one during a dialog follows §5.2;
  - a plugin's tool appears, and leaves on reload.
- **The official conformance suite** against `usdcc --mcp`, and the MCP Inspector by hand.
- **In the app,** with a test profile: §1's exit criterion from Claude Code.

## 10. Sources

- [Changes in 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28/changelog),
  [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http),
  [versions and both eras](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)
- [The official SDKs](https://modelcontextprotocol.io/docs/sdk); [`mcp` on PyPI](https://pypi.org/project/mcp/)
- [Local servers in Claude Desktop](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop)

## 11. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | The server in C++, or the Python SDK in the interpreter? | C++ (§5.1). The stateless revision made it small, and the SDK's packages would sit in the plugins' environment |
| 2 | Qt's `QHttpServer` (the vcpkg port `qthttpserver`, a new module to build), or HTTP by hand on `QTcpServer`? | `QHttpServer`, if it builds at the pinned baseline. By hand is about 200 lines, as only POST with a JSON body is needed |
| 3 | Serve the older `initialize` opening too? | Yes, without sessions. First check with the Inspector what Claude Code and Claude Desktop send today; drop it if both are on 2026-07-28 |
| 4 | A token on loopback, or none for ease? | A token. Other users and web pages can reach loopback |
| 5 | The access level when first turned on | Read only. Full is chosen by the user, or by `--mcp=full` |
| 6 | `run_python`'s variables | Fresh for each call; `keep_variables` uses a namespace of the server's own, not the Script Editor's |
| 7 | Structured edit tools (`set_attribute`, …)? | Not at first (§5.4) |
| 8 | Built in, or a plugin? | Built in. The queue, the access level and the status bar belong to the app, and a plugin shouldn't be able to open this door |
| 9 | The default port | 47811 here is a placeholder; any fixed number not in common use |
