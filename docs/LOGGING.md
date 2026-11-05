# Logging — Plan

*Status: all five phases done (see §11; phase 5 was built as part of [SCRIPTING.md](SCRIPTING.md) phase 1). Covers the logging half of milestone M8 in [PLAN.md](PLAN.md);
the `USDASidePanel` half gets its own plan. Last updated: 2026-11-02.*

## 1. Goal

One log, in one panel, that shows every message the app produces, whoever produced it:

- **usdcc's own C++ code**, through a small logging API.
- **Qt**: `qDebug`/`qWarning`/`qCritical`/`qFatal` from Qt itself, ADS, QWindowKit and our code.
- **OpenUSD**: every `TF_STATUS`, `TF_WARN`, `TF_CODING_ERROR`, `TF_RUNTIME_ERROR`,
  `TF_ERROR` and `TF_FATAL_ERROR`, from C++ and from `pxr` Python (`Tf.Warn`, …).
- **Python**: the `logging` module, `warnings.warn`, `print()`, writes to `sys.stderr`,
  and uncaught exceptions with their traceback.
- **Plugins** (M9): the same paths, labeled with the plugin's name.

"Showing up properly" means for each message: the right **level**, a **source** and
**category** you can filter on, the **file/line** or traceback where it came from, the
**thread** it came from, and **no message lost** — not the ones from before the panel
exists, not the ones from worker threads, not the last ones before a crash.

The `LogSidePanel` replaces today's stand-in and has level/source filters and search.
Everything is also written to a log file on disk.

### Where we start from

These facts constrain the plan:

- **There is no logging anywhere.** No `qWarning`, `std::cerr`, `TF_WARN` or anything else in
  `src/cpp`. `usdcc_core` is an INTERFACE library holding only `Signal`.
- **OpenUSD's messages disappear today.** With no delegate, Tf prints them to stderr. The exe
  is built with `WIN32_EXECUTABLE ON`, so launched from Explorer there is no console and that
  output goes nowhere.
- **Errors raised on worker threads are lost.** `StageOpener` loads on a `QThread`; when a
  layer fails to parse, the user gets "Could not open X." and the Tf errors explaining why
  are never shown.
- **Known noise exists.** PLAN.md §6 item 12 notes `HgiInteropOpenGL`/`HgiGLTexture` GL errors
  "at every frame". Once those reach the panel they would flood it, so repeats must collapse.
- **There is no embedded Python interpreter yet** (M7). `usdcc.usd` is a pybind11 module
  for a standalone interpreter. The Python bridge needs one inside the app (see §5.4, §10).
- **All usdcc libraries are STATIC**, and the Python modules are separate `MODULE` DLLs that
  link them again. A singleton in a static library would exist once per DLL, so a Python
  module would log into its own copy of the hub. §2.2 deals with this.
- The Log panel is a plain `QTextEdit` stand-in ([main_window.cpp](../src/cpp/ui/src/main_window.cpp)),
  registered as a `SidePanel` with objectName-keyed layout persistence already working.

## 2. Architecture

| Qt-free (`usdcc_core`, `usdcc_usd`: testable headlessly) | Qt (`usdcc_ui`) |
|---|---|
| `LogRecord`, `LogHub` (ring buffer, sinks, repeat collapsing) | `QtMessageBridge` (`qInstallMessageHandler`) |
| The C++ logging API (`USDCC_LOG_*`) | `LogModel` (a `QAbstractTableModel` over the hub's records) |
| `FileLogSink` (rotating log file) | `LogSidePanel` (view, filters, search, details) |
| `TfDiagnosticBridge` (in `usdcc_usd`: a `TfDiagnosticMgr::Delegate`) | Status-bar / tab badge for new errors |
| `NativeStreamCapture` (fd 1/2 redirected into the hub) | |
| `PythonLogBridge` (embedded module `_usdcc_log` + handler and stream objects) | `ToastLogSink`: records marked as toasts (§3.6) |

Messages flow one way: every bridge turns its message into a `LogRecord` and calls
`LogHub::post()`. Sinks — the file and the panel's model — subscribe to the hub. No bridge
knows about any sink.

### 2.1 The record

```cpp
enum class LogLevel { Trace, Debug, Info, Warning, Error, Fatal };

struct LogRecord {
    std::uint64_t sequence;        // assigned by the hub, strictly increasing
    std::chrono::system_clock::time_point time;
    LogLevel level;
    std::string source;            // "usdcc", "qt", "usd", "python", "stdout", "stderr", "plugin:<name>"
    std::string category;          // "usdcc.viewport", "TF_DIAGNOSTIC_WARNING_TYPE",
                                   // a Python logger name, a QLoggingCategory, …
    std::string message;
    std::string details;           // optional: Python traceback, Tf commentary beyond the first line
    std::string file;              // optional source location
    int line = 0;
    std::string function;
    std::uint64_t threadId;
    std::string threadName;        // "main", "stage-loader", "render", or empty
    std::uint32_t repeatCount = 1; // > 1 when identical records were collapsed (§2.3)
    std::optional<ToastRequest> toast; // set: also shown as a toast notification (§3.6)
};
```

### 2.2 The hub, and where it lives

`LogHub` holds the last **10 000** records in a ring buffer (configurable later, M10) and a
list of sinks. `post()` is **thread-safe and never blocks for long**: it takes a mutex,
stamps the sequence number, appends to the buffer and hands the record to each sink. Sinks
must be quick: the file sink queues to its own writer thread, and the panel's model only
queues for the UI thread (§4).

A sink that subscribes late can ask for the backlog (`snapshot()`), so the panel shows every
message logged since process start — including plugin-load messages from OpenUSD that arrive
before `MainWindow` exists.

**One instance per process.** `usdcc_core` becomes a **SHARED** library holding the hub
(`usdcc_core.dll`). The exe, the Python extension modules and later C++ plugins (M9) all
link it, so they share one hub. The rest of the C++ tree stays static. The alternative —
the hub living only in the exe and everything else reaching it through a function pointer
handed over at startup — works but makes every new module a special case; see §10 #2.

### 2.3 Repeat collapsing

A record identical to the **previous** one from the same source (same level, category,
message, file and line) within 2 seconds bumps that record's `repeatCount` instead of adding
a new one. The panel shows it as "×240". The file sink writes the first occurrence and then a
"(repeated 239 times)" line when the run ends. This keeps the GL-error spam from pushing
everything else out of the ring buffer while still showing that it happens.

### 2.4 Re-entrancy

A sink or bridge that logs while handling a message would deadlock on the hub's lock, or loop
(Tf can post a diagnostic from inside our delegate if, say, a `TfToken` lookup fails). The hub
keeps a thread-local "inside post()" flag: a nested `post()` on the same thread is queued (up to
64) and delivered once the outer post is done. Posts made while delivering those are dropped,
so a sink that logs on every record can't loop.

## 3. Sources

### 3.1 usdcc C++ code

```cpp
USDCC_LOG_INFO("usdcc.stage", "Opened {} ({} prims)", path, primCount);
USDCC_LOG_WARNING("usdcc.viewport", "Renderer {} unavailable, using Storm", id);
```

Macros over `std::format` (C++20, no new dependency), capturing `std::source_location`.
Debug and Trace calls compile in always but are filtered by a per-category minimum level
(defaults: Info for everything). Source is `"usdcc"`.

**First real users** (part of phase 1, so the panel has content from day one):
stage open/close/failure in `StageManager`/`StageOpener`, renderer plugin selection in
`HydraViewportWindow`, layout restore in `MainWindow`, and undo/redo in `UndoManager`.

### 3.2 Qt

`qInstallMessageHandler` in `usdcc_ui`, installed first thing in `main()` (before
`QApplication`, so Qt's own startup warnings are caught). `QtDebugMsg`→Debug, `QtInfoMsg`→Info,
`QtWarningMsg`→Warning, `QtCriticalMsg`→Error, `QtFatalMsg`→Fatal. Category = the
`QMessageLogContext` category (`"default"` when none). File/line come from the context in
Debug builds. The previous handler is not chained: `ConsoleLogSink` prints every record to
stderr and the debugger's output window in Debug builds (and with `USDCC_LOG_STDERR=1`), so
chaining would print Qt's messages twice.

### 3.3 OpenUSD

`TfDiagnosticBridge` in `usdcc_usd` derives from `TfDiagnosticMgr::Delegate`, registered with
`AddDelegate()` at the very top of `main()` and removed at shutdown.

| Tf call | `Issue*` | Level | Category |
|---|---|---|---|
| `TF_STATUS` | `IssueStatus` | Info | `GetDiagnosticCodeAsString()` |
| `TF_WARN` | `IssueWarning` | Warning | same |
| `TF_CODING_ERROR`, `TF_RUNTIME_ERROR`, `TF_ERROR(code, …)` | `IssueError` | Error | same; user error codes name their enum |
| `TF_FATAL_ERROR`, `TF_AXIOM` failure | `IssueFatalError` | Fatal | `"TF_FATAL_ERROR"` |

File, line and function come from the diagnostic's `TfCallContext`. `pxr` Python code
(`Tf.Warn`, `Tf.RaiseRuntimeError`) goes through the same manager, so it needs nothing extra.

Behavior to handle:

- **Delegates run on any thread** (stage loading, Hydra, TBB workers). The bridge only
  builds a record and calls `post()`.
- **With a delegate registered, Tf stops printing to stderr.** The file sink takes over that
  role, and `ConsoleLogSink` prints everything to stderr and the debugger in Debug builds, or
  when `USDCC_LOG_STDERR` is set.
- **Quiet diagnostics** (ones Tf itself would not have printed) arrive at Debug level.
- **Errors inside a `TfErrorMark` that the code clears never reach the delegate**
  (e.g. [property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp) parsing
  user input). That is intended: the code handled them. Errors left in a mark reach the
  delegate when the outermost mark goes away.
- **Worker-thread errors the UI needs.** `StageManager::loadUsdStage()` opens inside a
  `TfErrorMark`, copies the errors' text out for its caller, and lets the mark report them to
  the log; its own "Could not open stage X: <first error>" record follows them. `StageOpener`'s
  failure dialog shows the first error, and all of them under Show Details. A "Show Log" button
  comes with the panel (phase 3).
- **Fatal errors.** `IssueFatalError` posts a Fatal record, after which the hub flushes every
  sink before returning. Tf then writes its crash report and aborts by itself. It does not
  touch the UI.
- **`TF_DEBUG` output** does not go through the diagnostic manager (it writes straight to
  stdout). It is caught by native stream capture (§3.5).

### 3.4 Python

Python lands with M7's embedded interpreter. Logging adds one embedded module, built into the
exe with `PYBIND11_EMBEDDED_MODULE(_usdcc_log, m)` so it always talks to the exe's hub:

- `_usdcc_log.post(level, source, category, message, details, file, line, function, thread_name,
  toast, toast_title, toast_kind)`. The last three mark a record as a toast (§3.6).

A small `usdcc/log.py` installs, at interpreter start-up:

| Python path | How | Level | Category |
|---|---|---|---|
| `logging` | A `logging.Handler` on the root logger, level DEBUG; the root's level stays at the hub's filter | Python's level mapped (CRITICAL → Error) | logger name |
| `warnings.warn` | `logging.captureWarnings(True)` | Warning | `"py.warnings"` |
| `print()` / `sys.stdout` | Replace `sys.stdout` with a line-buffered writer object | Info | `"python.stdout"` |
| `sys.stderr` | Same, separate object | Warning | `"python.stderr"` |
| Uncaught exceptions | `sys.excepthook` and `threading.excepthook`; the script runner (M7) also calls it for exceptions it catches | Error, traceback in `details` | `"python.exception"` |

File/line come from the `LogRecord`'s `pathname`/`lineno`, or from the last traceback frame.

Rules:

- **Never block while holding the GIL.** `post()` is already quick (§2.2); the panel only
  queues. A sink must never wait for the UI thread, which itself may be waiting for the GIL.
- **Line buffering**: the stream objects emit one record per complete line and flush leftovers
  on `flush()`, so a traceback written in pieces does not become 30 records. A record written
  within one `excepthook` call is one record with the traceback in `details`.
- **Plugins** (M9) get a logger named `usdcc.plugin.<name>`; the bridge maps that prefix to
  source `plugin:<name>`.
- **Standalone Python** (`import usdcc.usd` outside the app) installs nothing; the user's own
  `logging` setup applies.

### 3.5 Native stdout/stderr

Some output never passes through any of the above: `TF_DEBUG`, `printf` from render delegates
or third-party libraries, and Python C extensions writing to the C-level streams.
`NativeStreamCapture` replaces file descriptors 1 and 2 with pipes (`_dup2` on Windows,
`dup2` on POSIX) and starts one reader thread per pipe that splits lines and posts them as
source `"stdout"`/`"stderr"` (Info/Warning). The original stderr is kept: `ConsoleLogSink`
and the hub's own error notes write there through `writeToOriginalStderr()`, since writing to
the captured stderr would feed every line back in (and could block the reader on its own full
pipe). The Debug-build "tee" first planned here is therefore not needed: `ConsoleLogSink`
already prints every record, captured lines included, to the original stderr.

This is the riskiest bridge (CRT stream buffering, crashing with a full pipe, MSVC's separate
CRT per DLL), so it comes last and can be turned off with `USDCC_NO_STREAM_CAPTURE=1`.

### 3.6 Records shown as toasts

A record can ask to also be shown as a toast notification ([NOTIFICATIONS.md](NOTIFICATIONS.md)):
`LogRecord::toast`, a `ToastRequest` (kind, title, body, duration; each optional).

```cpp
USDCC_LOG_TOAST(LogLevel::Warning, "usdcc.stage", "{} has unsaved changes", name);    // timed
USDCC_LOG_TOAST_STICKY(LogLevel::Error, "usdcc.stage", "Could not save {}", name);    // until dismissed
USDCC_LOG_TOAST_AS(LogLevel::Info, "usdcc.stage",
                   (ToastRequest{.kind = ToastKind::Success, .title = "Saved"}), "Saved {}", name);
```

```python
usdcc.core.log(usdcc.core.ERROR, "my.tool", "Export failed", toast="sticky", title="Export")
logging.getLogger("my.tool").warning("Disk almost full", extra={"usdcc_toast": True})
```

- **Always kept:** a marked record is posted and kept whatever levels are enabled. The user is
  meant to see it, so a Debug toast still shows. The toast macros therefore always format
  their arguments.
- **Repeats:** it only collapses (§2.3) with another marked record. A repeat counts on the
  toast that is up ("×2").
- **What it shows:**
  - its kind follows the level (Error/Fatal: Error, Warning: Warning, else Info) unless the
    request names one, such as Success;
  - its title is the level's name ("Warning") unless given;
  - its body is the message, plus the details.
- **Who shows it:** `ToastLogSink` (in `usdcc_ui`, added by `MainWindow`, without backlog
  replay) queues it to the UI thread. Warning and error toasts get a **Show Log** button,
  which calls `LogSidePanel::showLatest()`.
- **Before the window:** records logged before the window exists show no toast.

## 4. The panel

`LogSidePanel` (in `usdcc_ui`) replaces the stand-in, keeping objectName `"Log"` so saved
layouts still find it.

```
[☰]  [ search…                                  ] [Aa] [.*]   [🗑] [📁]
──────────────────────────────────────────────────────────────────────────────────────
12:04:31.204  ✕  usd     TF_RUNTIME_ERROR   Failed to open layer @a.usda@      ×1
12:04:31.205  ⚠  qt      qt.qpa.window      Window position outside any screen
12:04:33.910  ⓘ  python  my_tool            Created 12 prims
12:04:34.002  ⚠  usd     TF_DIAGNOSTIC_…    GL error: invalid operation       ×240
──────────────────────────────────────────────────────────────────────────────────────
(hover a row: tooltip with full message / traceback, file, line, function, thread)
```

- **Model/view**: `LogModel` reads the hub. The hub's sink callback, on any thread, appends to
  a pending list under a small lock and, if the list was empty, schedules one queued call to
  the UI thread. The UI thread drains the list in one `beginInsertRows`, at most every 50 ms.
  No per-message signals; 10 000 messages a second stays smooth.
- **Filters**: a menu behind the same three-lines button ViewPanels start their toolbar with
  (`PanelMenuButton`): checkable level entries with counts (Errors, Warnings, Info, Debug —
  Debug off by default), one checkable entry per source (and per plugin), a checkable
  category tree, and Show All Categories. Categories are split into levels at each '.'
  (`usdcc` > `viewport` > `picking`); unchecking a node hides that level and everything under
  it, a parent shows partly checked when only some of its children are hidden, and the tree
  stays open in the menu so several can be toggled at once. Hiding is stored as the fewest
  levels that cover what is unchecked, so a category first seen later under a hidden level is
  hidden too. Next to it a search field (plain text, case and regex toggles) matching message,
  category and details. Filters are a
  `QSortFilterProxyModel`; settings persist with the panel.
- **Follow mode**: the view sticks to the bottom until the user scrolls up, and resumes at the
  bottom.
- **Details tooltip** on every cell of a row: full message, details/traceback, time, level,
  source, category, `file:line`, function, thread, repeat count. (First built as a pane below
  the table; replaced by the tooltip to give the table the panel's full height. The location is
  no longer a clickable link, since a tooltip can't be clicked.)
- **Context menu**: Copy, Copy with details, Filter to this category, Hide this category.
- **Toolbar**: Clear (clears the view, not the file), Open log file / Reveal log folder.
- **Attention**: a new Error, while the panel is hidden or scrolled away, is counted in the
  panel's title ("Log — 3 new errors"), which the View menu entry shows too, with a red dot. It
  clears once the panel is shown at the bottom. It never pops the panel open by itself. (The
  status-bar message first planned here was left out. The window has a status bar now, for the
  edit target, but a message the user must see is a toast: mark the record, §3.6.)
- **Theme**: level colors and icons come from the existing theme tokens (`theme.h`).

## 5. The log file

`FileLogSink` writes plain text, one line per record (details indented beneath it), to
`<AppLocalData>/usdcc/logs/usdcc-<yyyyMMdd-HHmmss>-<pid>.log`. The sink is Qt-free, so
`main()` resolves that folder with `QStandardPaths` and passes it in. It keeps the last 10 runs.
Writes go through a queue to a writer thread, flushed at least every second and immediately
for Error and Fatal. On Fatal, and from a crash handler (`SetUnhandledExceptionFilter` /
`SIGSEGV`) where safe, it flushes synchronously — the last messages before a crash are the
ones most worth keeping. The `USDCC_LOG` environment variable sets levels (§8); a
`--log-file` option is a possible later addition.

## 6. Start-up order

In `main()`:

1. `LogHub` exists (static in `usdcc_core.dll`), with `FileLogSink` attached.
2. `NativeStreamCapture` (phase 4), `QtMessageBridge`, `TfDiagnosticBridge`.
3. `QApplication`, `StageManager`, the command-line stage, the window.
4. `LogSidePanel` subscribes and pulls the backlog.
5. The Python interpreter (M7) starts and runs `usdcc.log.install()` before any user script.

Shutdown runs it in reverse: Python, panel, bridges, then the file sink is flushed last.

## 7. Verification

The M8 exit criterion is that messages from C++, Python and plugins all appear in one place.
That is checked three ways:

- **Unit tests** (in `usdcc_tests`, headless): hub ordering, ring-buffer overflow, collapsing,
  backlog snapshot, re-entrancy; Tf bridge mapping for each `TF_*` macro, including from a
  worker thread and inside a `TfErrorMark` that is / isn't cleared; Qt handler mapping; file
  sink rotation and flush-on-error; the Python bridge in an embedded interpreter (logging,
  warnings, print, partial lines, excepthook, `Tf.Warn` from Python).
- **Debug > Emit Test Messages**: emits one message of every level from every source —
  usdcc, Qt, a `TF_*` of each kind (also from a worker thread), Python `logging`/`print`/
  an uncaught exception, a native `printf`, and a burst of 500 identical warnings. Every one
  must show in the panel with the right level, source, category and location, the burst as
  one "×500" row, and all of them in the log file.
- **Real-world checks**: open a stage with a syntax error (the dialog names the error, the
  panel has it); launch from Explorer without a console (nothing lost); kill the process
  right after an error (the error is in the file); open the viewport and confirm the GL
  spam collapses.

## 8. Out of scope for this plan

- Structured fields beyond the record (key/value payloads), and JSON log output.
- Sending logs anywhere off the machine.
- A per-stage or per-prim log ("show messages about this prim").
- A UI to edit per-category levels; until M10's settings dialog, `--log-level` and an
  environment variable (`USDCC_LOG=usdcc.viewport=debug,usd=warning`) set them.
- Investigating the GL-error spam itself (PLAN.md §6 item 12). The panel only makes it visible.
  (Since done: PLAN.md §6 item 38. `USDCC_GL_DEBUG=1` asks for a debug GL context, and the
  viewport then logs each GL error under `usdcc.viewport.gl` as the driver reports it, with
  the stack of the call that caused it.)

## 9. Phases

Each phase ends with something usable and verified.

| Phase | Content | Verification |
|---|---|---|
| **1. Hub and C++ API** *(done)* | `usdcc_core` as a shared library; `LogRecord`, `LogHub`, collapsing, re-entrancy guard; `USDCC_LOG_*`; `FileLogSink`; first log calls in `StageManager`/`StageOpener`/`HydraViewportWindow`/`UndoManager`/`MainWindow` | Hub and file-sink unit tests. A run writes a readable log file with the stage open/close lines |
| **2. Qt and OpenUSD bridges** *(done)* | `QtMessageBridge`, `TfDiagnosticBridge`, start-up order (§6); the `StageOpener` error mark and better failure dialog | Tf and Qt mapping tests, including worker threads. Opening a broken `.usda` names the error. No console needed to see Tf messages |
| **3. Log panel** *(done)* | `LogModel`, `LogSidePanel` replacing the stand-in, filters, search, details tooltip, follow mode, error badge; Debug > Emit Test Messages (C++/Qt/Tf part) | Every test message shows correctly. A 10 000-message burst does not freeze the UI. Saved layouts still restore the panel |
| **4. Native stream capture** *(done)* | `NativeStreamCapture`, off-switch, Debug-build tee | `printf` and `TF_DEBUG` output show in the panel. No hang with a flood of output or at shutdown |
| **5. Python** *(done, as SCRIPTING.md phase 1)* | `PYBIND11_EMBEDDED_MODULE(_usdcc_log)`, `usdcc/log.py`, install at interpreter start; Python part of Emit Test Messages | Python bridge tests. `logging`, `warnings`, `print`, `sys.stderr`, uncaught exceptions and `Tf.Warn` all show up correctly. Needs M7's interpreter (§10 #1) |

Phases 1–3 make the panel useful on their own. Phase 4 is independent of 3. Phase 5 needs the
embedded interpreter.

## 10. Decisions taken as defaults

Each of these can be changed before the phase that needs it starts:

| # | Question | Default used by this plan |
|---|---|---|
| 1 | Python needs an embedded interpreter, which is M7 | Do phases 1–4 now. Start M7 with the bare interpreter (start, `sys.path`, `usdcc.log.install()`, Debug > Run Python File…), then do phase 5, then the Script Editor panel. Doing logging first means M7's script errors have somewhere to go |
| 2 | How modules reach the one hub | `usdcc_core` becomes a shared library (§2.2) rather than handing a function pointer to each module |
| 3 | spdlog (suggested in PLAN.md §6 item 7) or our own hub | Our own: the record, ring buffer, collapsing and backlog are the bulk of the work and spdlog provides none of them; `std::format` covers formatting. Revisit if the file sink grows (compression, size-based rotation) |
| 4 | Ring-buffer size | 10 000 records; the file keeps everything |
| 5 | Default minimum level | Info for all sources; Debug shown only when enabled per category |
| 6 | Does Clear in the panel clear the hub? | No: it hides records up to the current sequence number; the backlog and the file are untouched |
| 7 | Should a new error open the panel? | No: badge and status-bar message only (§4) |

## 11. What was built

### Phase 1

- **`usdcc_core` is a shared library** (`usdcc_core.dll`), exporting with `USDCC_CORE_API`
  ([export.h](../src/cpp/core/include/usdcc/core/export.h)). `usdcc_deploy_core_library()` copies
  the DLL next to the exe, both test executables and the `.pyd` modules after each build.
- **[log.h](../src/cpp/core/include/usdcc/core/log.h)**: `LogLevel`, `LogRecord` (with a
  `lastTime` field for collapsed runs, beyond §2.1), `LogSink`, `LogHub` and the `USDCC_LOG_*`
  macros. Levels: a default plus keys matching a source or a dotted category prefix, longest
  match first; `configureLevels()` parses specs such as `"info,usdcc.viewport=debug,usd=warning"`.
  Debug and Trace are rejected through one atomic compare, without locking.
- **[file_log_sink.h](../src/cpp/core/include/usdcc/core/file_log_sink.h)**: the writer thread,
  flush rules, repeat summaries and rotation from §5.
- **`main()`** names the main thread, applies `USDCC_LOG`, and attaches the file sink right after
  `QApplication` (it needs the application's data folder). Records posted before that are
  replayed from the ring buffer. The sink is detached last, after the window and the stage
  manager are gone.
- **First log calls**: stage open/close/failure (`usdcc.stage`; the loader thread is named
  `stage-loader`), the viewport renderer and failed renderer switches (`usdcc.viewport`),
  undo/redo and failed layer restores (`usdcc.undo`), and layout restore (`usdcc.layout`).
- **Tests** ([log_test.cpp](../tests/log_test.cpp)): levels, ring buffer and backlog,
  collapsing, a sink that logs, 4 threads × 2000 posts, the macros, file format, the file sink
  and rotation.

Checked by hand: a run with a stage writes start-up, "Opened stage", layout and renderer lines;
a run with a missing file writes the "Could not open stage" error. Nothing from Qt or OpenUSD
reaches the log yet; that is phase 2.

### Phase 2

- **[TfDiagnosticBridge](../src/cpp/usd/include/usdcc/usd/tf_diagnostic_bridge.h)** (in
  `usdcc_usd`) and **[QtMessageBridge](../src/cpp/ui/include/usdcc/ui/qt_message_bridge.h)** (in
  `usdcc_ui`), both RAII objects created at the top of `main()`, before `QApplication`.
- **[ConsoleLogSink](../src/cpp/core/include/usdcc/core/console_log_sink.h)** (stderr plus
  `OutputDebugString`), since the bridges stop Tf and Qt printing anything themselves.
- **`LogSink::flush()` and `LogHub::flush()`**; the hub flushes every sink after a Fatal record.
- **The hub survives a sink that throws**: each sink call is guarded, and the exception is
  reported on stderr instead of leaving the thread unable to log.
- **`loadUsdStage(identifier, &errors)`** returns OpenUSD's error messages; `StageOpener`'s
  failure dialog shows them.
- **Tests** ([log_bridges_test.cpp](../tests/log_bridges_test.cpp)): each `TF_*` macro's level,
  category and call site; cleared and uncleared error marks; diagnostics from a worker thread;
  a broken `.usda` (the caller and the log get the same errors, plus usdcc's summary); Qt
  messages by level and category, and nothing after the bridge is gone.

Checked by hand: opening a broken `.usda` from the command line logs the parser error
(`textParserHelpers.h:48`), "Failed to open layer" (`stage.cpp:1287`) and usdcc's summary. Not
yet checked: the failure dialog from File > Open Stage, and how the viewport's GL errors
(PLAN.md §6 item 12) look in the log — the saved layout had the Attributes tab in front, so
the viewport never drew during the test runs. Both are on the phase 3 checklist (§7).

### Phase 3

- **[LogModel and LogFilterModel](../src/cpp/ui/include/usdcc/ui/log_model.h)**: the sink
  only queues; the UI thread takes records in at most every 50 ms, in one insertion. Repeats
  update their row ("×500"). Levels, hidden sources and categories, "only this category", and
  plain or regular-expression search in either case.
- **[LogSidePanel](../src/cpp/ui/include/usdcc/ui/log_side_panel.h)** replaces the stand-in in
  `MainWindow` (objectName still "Log"). Toolbar: the three-lines filter menu (levels with
  counts, sources, a checkable category tree split at '.', Show All Categories), search with Aa and .* toggles, and Clear (bin icon) and Log Folder (folder icon) as square icon buttons (`PanelIconButton`). The
  button is [PanelMenuButton](../src/cpp/ui/include/usdcc/ui/panel_menu_button.h), now shared
  with every ViewPanel's options button. Table with fixed row
  heights, follow mode, the row's details as a tooltip (`LogModel::detailsHtml()`: full
  message, details, time, level, source, category, location, function, thread, repeats), Copy
  and Copy with Details (Ctrl+C), and a context menu to show only, or hide, a category or
  source. Filters and column widths persist in `usdcc.ini` under `LogPanel/`.
- **Follow mode survives row churn**: when the selected row drops off the front, Qt makes
  another row current and scrolls to it. Model changes are bracketed, so that scroll doesn't
  switch following off, and the view returns to the bottom afterwards.
- **Show Log** in the open-stage failure dialog (`StageOpener::showLogRequested()` →
  `LogSidePanel::showLatest()`).
- **Fixed in passing (`StageOpener`)**: the "Opening…" progress dialog was only hidden and
  `deleteLater`'d. The failure dialog's event loop doesn't run deferred deletes, and
  `QProgressDialog`'s force-show timer brought it back over the failure dialog after about 4 s,
  blocking its buttons. It is now deleted at once.
- **Debug > Emit Test Log Messages / Emit 10 000 Log Messages**
  ([log_test_messages.h](../src/cpp/usd/include/usdcc/usd/log_test_messages.h)).
- **Tests** ([log_panel_test.cpp](../tests/log_panel_test.cpp), in `usdcc_ui_tests`): rows,
  batching, repeats, counts, sources, capacity, every filter, 10 000 messages from a worker
  thread (about 80 ms, one batch, in an offscreen table), the panel's unseen-error title and
  dot, follow mode through a dropped selected row (fails without the fix), and filter
  persistence.

Checked in the app: Emit Test Log Messages gives Errors 5, Warnings 8 and Info 6 as expected,
the worker-thread messages, and one "×500" row. The details show for the hovered row,
the 10 000 burst shows at once, and a broken file opened through File > Open Stage shows the
dialog, which stays usable past 4 s. Show Log brings the panel to the bottom and clears the
error count. The viewport's GL errors (PLAN.md §6 item 12) did not appear in any of these runs.

### Phase 4

- **[NativeStreamCapture](../src/cpp/core/include/usdcc/core/native_stream_capture.h)** in
  `usdcc_core`, created at the top of `main()` before the bridges and restored last.
  Descriptors 1 and 2 become pipes (not inherited by child processes); on Windows the
  `STD_OUTPUT`/`STD_ERROR` handles follow, for DLLs with a CRT of their own. A GUI process
  started without a console has no stdout/stderr at all, so they (and stdin) are first opened
  on `NUL` to get descriptors 0-2. stdout and stderr are unbuffered while captured. Readers
  post one record per line (`\r` stripped, empty lines dropped, a last line without a newline
  posted when capture ends, 64 KB at most per record), from threads named "stdout capture" and
  "stderr capture". On shutdown a reader that hasn't seen end-of-file within a second has its
  read cancelled (`CancelSynchronousIo`; POSIX readers poll a stop flag). One capture at a
  time; `USDCC_NO_STREAM_CAPTURE` turns it off.
- **`writeToOriginalStderr()`**, used by `ConsoleLogSink` and the hub's sink-exception note.
- **Debug > Emit Test Log Messages** also writes with `printf`, `fprintf(stderr)` and
  `std::cout`, and from a worker thread.
- **Tests** ([log_capture_test.cpp](../tests/log_capture_test.cpp)): line splitting across
  calls, `\r\n`, a raw descriptor write, a worker thread, the unterminated last line;
  20 000 lines from two threads (about 180 ms, none lost); the console sink doesn't loop; one
  capture at a time; the off switch; output goes to the real stdout again afterwards.

Checked in the app, started without a console: `TF_DEBUG=PLUG_INFO_SEARCH` output (138 lines)
and the test messages' native lines reach the panel and the log file, and the app exits
normally in a fraction of a second.
