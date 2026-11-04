# Toast notifications

Short messages in a corner of the main window: "Saved", "Export failed", "3 prims could not
be resolved".
- They stack.
- They either go away after a while or stay until dismissed.
- They are colored by kind.
- They appear only inside usdcc, never as operating-system notifications.

## 1. What a toast is

| Part | Meaning |
|---|---|
| **Kind** | `Info` (the accent color), `Success` (green `#4fd671`), `Warning` (amber `#ffb340`), `Error` (red `#ff5f56`). It colors the stripe on the left, the icon (i, ✓, !, ×) and the countdown bar. `theme::toastColor()` |
| **Title** | Bold, wraps if long |
| **Body** | Any number of lines, word-wrapped. The text decides the toast's size: as wide as its longest line within 260–420 px (at 100% UI scale), as tall as the wrapped text. A body taller than 40% of the window scrolls |
| **Duration** | Timed: a thin bar along the bottom counts down, and the toast fades out at the end. Hovering the toast pauses it. Sticky: it stays until its × is clicked |
| **Actions** | Buttons that run something and dismiss the toast, such as **Show Log** on log toasts. C++ only for now |
| **×N** | Shown when the same toast (kind, title, body) is shown again while it is up. It is counted and its time restarts, instead of being stacked twice |

## 2. Where they appear

- **Corner:** the stack sits in one corner of the main window's content area: inside the
  title bar, docked toolbars and status bar, 12 px from the edges. The corner is a setting
  (§4).
- **Order:** the newest toast is nearest the corner. Older ones move along the edge, 8 px
  apart, with a short animation.
- **Dismiss All:** a "Dismiss All" button ends the stack while 2 or more toasts are up.
- **Limit:** at most `maxVisible` toasts are up at once. Others wait in a queue and appear
  as toasts go.
- **Before the window:** toasts shown before the window is visible, or while it is
  minimized, wait for it.
- **Following the window:** the stack follows the window when it moves, resizes, or its
  toolbars change.

**Each toast is a window of its own.** It is a frameless `Qt::Tool` window owned by the main
window that never takes the focus. It is not a child widget, because the viewport is a
native OpenGL window (`HydraViewportWindow` in `createWindowContainer`). On Windows a native
child draws over any plain sibling widget, so an overlay widget would vanish behind the
viewport. An owned tool window instead:
- stays above its owner only, not above other applications;
- hides with the owner when it is minimized.

## 3. Showing one

**C++ (UI code)**, through `MainWindow::toasts()`, a `ToastManager`:

```cpp
ToastManager* toasts = window.toasts();
int id = toasts->show({.kind = ToastKind::Success, .title = "Saved", .body = "scene.usda\n3 prims"});
toasts->show({.kind = ToastKind::Error, .title = "Export failed", .body = message,
              .durationMs = ToastRequest::kSticky},
             {{"Show Log", [&]() { window.logPanel()->showLatest(); }}});
toasts->dismiss(id);
toasts->dismissAll();
toasts->post(request);  // from any thread
```

`ToastRequest` (`usdcc/core/toast.h`) is Qt-free:

| Field | Default when unset |
|---|---|
| `kind` | Info |
| `title` | — |
| `body` | — |
| `durationMs` | `-1`: the kind's default from the settings. `ToastRequest::kSticky` (`0`) means until dismissed |

**From any code, through the log** ([LOGGING.md](LOGGING.md) §3.6). This works in Qt-free
libraries and on any thread:

```cpp
USDCC_LOG_TOAST(LogLevel::Warning, "usdcc.stage", "{} has unsaved changes", name);
USDCC_LOG_TOAST_STICKY(LogLevel::Error, "usdcc.stage", "Could not save {}", name);
USDCC_LOG_TOAST_AS(LogLevel::Info, "usdcc.stage",
                   (ToastRequest{.kind = ToastKind::Success, .title = "Saved"}), "Saved {}", name);
```

- The record goes into the log as usual and is also shown as a toast.
- Its kind and title come from the level unless given, and the message (plus details)
  becomes the body.
- Warnings and errors get **Show Log**.

**Python**, with `usdcc.toast` (it needs the app's window, but not PySide):

```python
import usdcc.toast as toast
tid = toast.show("Exported", "3 prims written to\nC:/out/scene.usda", kind="success")
toast.show("Baking", "This takes a while", sticky=True)
toast.show("Quick", duration=2)        # seconds
toast.dismiss(tid); toast.dismiss_all(); toast.visible()

usdcc.core.log(usdcc.core.ERROR, "my.tool", "Export failed", toast="sticky", title="Export")
logging.getLogger("my.tool").warning("Disk almost full", extra={"usdcc_toast": True})
```

Debug > **Show Test Toasts** shows one of each kind:
- a long multiline toast;
- a sticky error;
- a warning logged as a toast.

## 4. Settings

These are in Settings > Notifications ([SETTINGS.md](SETTINGS.md)) and apply immediately:

| Key | Default | Meaning |
|---|---|---|
| `Notifications/position` | Bottom right | Bottom right, Bottom left, Top right or Top left of the main window |
| `Notifications/duration` | 5 s | Info and success toasts |
| `Notifications/warningDuration` | 8 s | Warnings, and errors when they aren't sticky |
| `Notifications/errorsSticky` | on | Errors stay until dismissed |
| `Notifications/maxVisible` | 5 | Toasts up at once; more wait |

## 5. Why not a library

These were checked in November 2026:

| Option | Why not |
|---|---|
| [niklashenning/qt-toast](https://github.com/niklashenning/qt-toast), a C++ port of pyqttoast 1.2 (MIT, Qt 5/6) | The closest match. But each toast is a `QDialog` with `WindowStaysOnTopHint`, so it floats above other applications. It is placed against the **screen**, with no "relative to a widget" mode. Its queues and settings are static globals. It has no CMake or vcpkg packaging and no releases. Its colors and fonts would not follow the theme, the accent setting or the UI scale |
| [dev-protocol/qt-toast](https://github.com/dev-protocol/qt-toast) | A fork of the same port, with the same design |
| KDE's `KMessageWidget` | An inline message bar, not a stack, and it brings in KDE Frameworks |
| Qt | Has nothing for this |

The usdcc version is small (`toast.cpp`, `toast_manager.cpp`, `toast_log_sink.cpp`).

## 6. Code and tests

**Code:**
- `usdcc/core/toast.h`: `ToastKind` and `ToastRequest`.
- `LogRecord::toast` and the `USDCC_LOG_TOAST*` macros in `usdcc/core/log.h`.
- In `usdcc_ui`:
  - `ToastWidget` (`toast.h`);
  - `ToastManager` (`toast_manager.h`), which also defines the settings;
  - `ToastLogSink` (`toast_log_sink.h`);
  - `MainWindow::toasts()`.
- Python: `usdcc/toast.py` on `_usdcc_app.toast_*`, plus the toast arguments of
  `_usdcc_log.post`.

**Tests:**

| Test | Covers |
|---|---|
| `log_toast_records` (usdcc_tests) | the record field, level bypass, collapsing, the macros |
| `toast_*` (usdcc_ui_tests) | stacking in all four corners, sizes, timing and pausing, repeats, the queue, dismissing, following the window, toasts from a worker thread's log with Show Log |
| `python_toasts` (usdcc_python_tests) | the Python module and the log flag |

**Not yet:**
- Actions from Python, and change callbacks.
- Existing message boxes (open/save failures) are not turned into toasts yet.
