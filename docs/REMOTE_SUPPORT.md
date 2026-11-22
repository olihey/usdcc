# Remote Support (plan)

A user who is stuck sends a **support request** to a colleague. The colleague sees usdcc's windows,
live, and, if the user allows it, works in them with mouse and keyboard.

**Status:** planned 2026-11-22. The phase 0 spike ran the same day on one Windows machine (§10):
capture and input both work. Nothing of the feature is built. "Checked" below means looked at in
this repository, "measured" means the spike ran it; everything else is an assumption until it's
tried.

## 1. Decisions

- **Off twice.** A build option leaves the feature out of the build entirely (§6). When it is built,
  a setting turns it on, and that setting is **off by default** (§5). While it is off, the app shows
  no way to create a support request anywhere: no menu entry, no button, no greyed-out item.
- **usdcc's windows only, never the desktop.** The helper sees the app's own windows (main window,
  floating panels, menus, dialogs it draws) and nothing else on the screen.
- **Input as Qt events, never as OS input.** What the helper does is replayed as Qt events into the
  app's own windows. Nothing like `SendInput` is used, so the helper can't reach another program or
  the OS, and it works the same on Windows, macOS and Linux, Wayland included (§4).
- **One helper at a time.** A second person opening the link is refused while a helper is
  connected. Decided 2026-11-22.
- **Looking first, controlling second.** A request starts as view-only. Control is a second,
  separate **Allow**, and only when the setting allows control at all (§5).
- **The user always wins.** Touching the mouse or keyboard pauses the helper's input until the user
  stops. A shortcut and a button that are always visible end the session.
- **Built on what exists.** The session is a room of [PEERS.md](PEERS.md) with the helper as a
  browser guest (§6.1 there). The pictures, the page and the pacing come from
  [VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md): the same `FrameSource`, JPEG first, H.264 later.
- **A C++ plugin,** `src/plugins/remote_support`, like Viewport Broadcast. Not built, it is not
  there (§6). Built, it opens no port and shows nothing until the setting is on and a request is
  made.

## 2. For the user

### 2.1 Asking for help

- **Help > Request Support…**, and the same in the Peers panel's menu for a person in a room. Both
  exist only while the setting is on.
- The request is either a **link** (copied, to paste into Teams, Slack or email) or a **message to a
  peer** in a room, which shows on their side as a toast with **Open**.
- Before anything is sent, a dialog says what will be shared: "Your usdcc windows will be shown to
  the person who opens this link. Nothing else on your screen is shared."
- **While a session runs:** a colored border around every usdcc window, and a bar at the top of the
  main window: "Anna is viewing" or "Anna is in control", with **Take back** and **End**.
- **The helper asks for control:** a toast, "Anna asks to control usdcc", with **Allow** and
  **Deny**. Without the control setting (§5) the helper's page doesn't offer to ask.
- **Ending:** **End** in the bar, the stop shortcut, closing the app, or turning the setting off.
  Any of them ends it at once, and the helper's page says so.

### 2.2 Helping

- Opens the link in a browser, with nothing to install, or **Open** on the toast in usdcc, which
  opens the same page.
- Sees the windows scaled to fit, the user's cursor, and a chat.
- With control: the mouse and keyboard work in the picture as if it were the app. While the user
  touches their own mouse or keyboard, the page shows "Paused: the user is working".

## 3. Capturing the windows

Two ways. In-app capture is the baseline on every platform; OS capture is an optional fast path.

### 3.1 In-app capture (the baseline)

usdcc draws every pixel it shows, so it can build the picture itself:

- **Widgets:** `QWidget::grab()` or `render()` of each top-level window.
- **The 3D views are native windows,** so a widget grab leaves them blank. They are filled in from
  their own read-back: `HydraViewportWindow::grabFrame()` (*checked*, used by Viewport Broadcast;
  about 3 ms at 1920×1080, measured there). RTX Render's Vulkan window needs the same hook.
- **Composited** at the positions Qt knows, into one picture per top-level window: the main window,
  each floating panel, and each open menu, tooltip or dialog.
- **The cursor** is drawn in by us.
- **What it can't capture:** the OS's own dialogs (§3.3).

No permission prompt on any platform, no Wayland problem, and nothing outside usdcc can end up in
the picture. The cost is a CPU read-back and compositing. For support (clicking through UI, not
orbiting at 60 frames a second) that is enough.

### 3.2 OS capture (an optional fast path)

Captures exactly what is on screen, GPU content included, without compositing. Each platform has its
own API, and they behave differently:

| | API | Only usdcc's windows? | Permission | Notes |
|---|---|---|---|---|
| **Windows** | Windows Graphics Capture (Windows 10 1903+) | Yes, per window | None. Windows draws a yellow border around a captured window; Windows 11 lets the app turn it off | GPU textures that go straight into Media Foundation's H.264 encoder. Covered windows are captured, minimized ones not |
| **macOS** | ScreenCaptureKit (12.3+) | Yes, filtered to the app's windows | **Screen Recording**, granted in System Settings; the app must restart after it. Recent macOS versions ask again from time to time | GPU surfaces, off-screen windows captured, VideoToolbox for H.264 |
| **Linux, X11** | XComposite with XShm, per window | Yes | None | Simple and reliable, a CPU copy. Only when the app runs on `xcb` |
| **Linux, Wayland** | xdg-desktop-portal ScreenCast, through PipeWire | **No: the user picks the window in a system dialog** | That dialog every time, unless a restore token is kept | GNOME and KDE work, smaller compositors vary. The app can't learn its own windows' positions, which makes mapping input hard |

So OS capture means up to four back ends, a permission flow on macOS, and a poor experience on
Wayland. It is added behind the same `FrameSource` only where the frame rate turns out to matter:
Windows Graphics Capture first, ScreenCaptureKit if macOS is a target.

### 3.3 Native dialogs

**The problem.** Most windows usdcc shows are drawn by Qt, so usdcc owns their pixels and their
input. A few are not: when the app asks for a file or a folder, Qt by default hands the job to the
OS's own dialog (on Windows the shell's `IFileOpenDialog`, on macOS `NSOpenPanel`, on Linux the
desktop's portal or GTK dialog). That dialog is a window the OS draws, not a `QWidget` or a
`QWindow`:

- **It can't be grabbed** by in-app capture: there is no widget to render, so the helper sees the
  main window with nothing on top while the user looks at a file picker.
- **It can't be controlled** by Qt events: replay (§4) has no Qt window to send them to. Only OS
  input would reach it, which this plan rules out.
- **It is modal.** The app waits for it to close, so the helper is stuck until the user finishes it.
- **It shows more than usdcc.** A file picker shows the user's folders, recent files and drives,
  which the user may not expect to share. OS capture (§3.2) could show it, but would then show all
  of that, and still couldn't control it.

*Checked:* 17 calls use the native file dialog today (`QFileDialog::getOpenFileName` 7,
`getSaveFileName` 8, `getExistingDirectory` 2, in 14 files). Message boxes (`QMessageBox`) and the
color pickers already used (`QColorDialog` with `DontUseNativeDialog`) are drawn by Qt and are fine.

**Decided 2026-11-22: not handled.** The helper asks the user, in the chat or on a call, to operate
an OS dialog, and the user does. Nothing is changed for a session: the dialogs stay native, and the
page doesn't detect or mark them. This keeps the user's file system out of the picture.

## 4. Input

- **The page sends events, not OS input:** pointer moves, buttons, wheel and keys, with the point as a
  fraction of the picture.
- **The plugin replays them through `QWindowSystemInterface`,** the way Qt's platform plugin
  delivers real input, into the window under that point (a native 3D view directly). Not
  `QCoreApplication::sendEvent()` to widgets: that skips shortcuts, popups' grabs, enter and leave,
  and double clicks. It is a private Qt API (`Qt6::GuiPrivate`), and it takes native pixels (§10).
- **Keys are sent as text and key codes,** so the helper's keyboard layout gives the characters
  they typed.
- **The user always wins:** any local mouse or key input pauses replay until a short quiet time
  after the user's last input.
- **Hard parts, to try early:** a drag that starts in one window and ends in another; popups that
  open and close during a drag; text input methods; double clicks over a slow link.

## 5. The settings

Defined by the plugin, so a build without it (§6) has none of them. Settings > Peers, group
**Remote Support**:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `Support/enabled` | Bool | `false` | **Allow support requests.** Off: no Request Support entry anywhere, no session can start, and a request arriving from a peer is refused without a toast |
| `Support/allowControl` | Bool | `false` | **Let helpers ask for control.** Off: sessions are view-only and the helper's page has no control button. Shown only while `Support/enabled` is on |
| `Shortcuts/support.stop` | Shortcut | *to decide* (§9) | Ends a session from anywhere in the app |

- **Turning `Support/enabled` off** ends a running session at once and removes the entries.
  Turning it on adds them. Both without a restart.
- **Removing the entries at runtime needs a small change to the plugin API.** *Checked:*
  `Host::addViewportMenuAction()` has `applies`, which leaves an entry out when false.
  `Host::addMenuAction()` (the main menu) has no `applies` and no way to remove an entry
  ([plugin.h](../src/cpp/plugin_sdk/include/usdcc/plugin/plugin.h)), although the app has
  `removeMainMenuAction()` in `main_menu_actions.cpp`. Phase 1 adds `Host::removeMenuAction()`
  on top of it.
- **A remote peer can never change these.** They are read and written only locally. A script can set
  them through `usdcc.settings`, as it can any setting; a script already runs with the user's rights,
  so that is no new door (§8).

## 6. Leaving it out of the build

**What exists today.** *Checked* in [CMakeLists.txt](../CMakeLists.txt): a few CMake options
choose what is built, `USDCC_BUILD_RTX_PLUGIN` (off by default), `USDCC_BUILD_TESTS` and
`USDCC_USE_PCH`, plus `USDCC_BUILD_BLENDER_MESH_TOOLS`, which is forced on. Viewport Broadcast,
Peers and the other plugins are always built. There is no general convention for features a build
can leave out, and no studio-wide setting that a user can't change (§9).

**For Remote Support:** a CMake option, `USDCC_BUILD_REMOTE_SUPPORT`.

- **Off:** `src/plugins/remote_support` isn't built or deployed. The settings, the menu entries, the
  page and the input replay don't exist in the build.
- **Off also means it can't be added back by copying a plugin in.** The app is built with
  `USDCC_REMOTE_SUPPORT=0`, and the plugin manager then refuses to load a plugin with the id
  `remote_support` from any folder, and says so in the Plugin Manager. Together with the missing
  code, a studio's build can't do remote support even if someone copies the plugin from another
  build.
- **On:** built and bundled, with the setting off (§5).
- **The default is on** (decided 2026-11-22), so the feature exists in the public build but is off
  until a user turns it on. A studio that doesn't want it at all builds with
  `-DUSDCC_BUILD_REMOTE_SUPPORT=OFF`.
- **About usdcc lists the options the build was made with,** so anyone can check.

This follows the convention planned in [BUILD_OPTIONS.md](BUILD_OPTIONS.md), and is its first user.

## 7. Phases

| Phase | What | Done when |
|---|---|---|
| **0. Spike** *(done, §10)* | In-app capture of the main window with a viewport and a floating panel composited in; replaying a click and a key from a page | A picture with the 3D view in place; a button clicked and a text field typed into from the browser; milliseconds per frame at 2560×1440 |
| **1. View-only** | The CMake option, the plugin id refused when off, the settings, `Host::removeMenuAction()`, Help > Request Support (link), the session bar and border, the page, ending | The entries exist only with the setting on; turning it off ends a session; a build with the option off has no plugin and refuses a copied one |
| **2. Control** | Ask, Allow, input replay, the user always wins, the stop shortcut | A helper edits an attribute and moves a prim in the outliner; local input pauses them |
| **3. Requests to a peer** | Sending a request to a person in a room, the toast with Open | A request reaches a peer on another machine and opens the page |
| **4. Faster pictures** | H.264 from Viewport Broadcast's phase 5; Windows Graphics Capture behind `FrameSource` | A session over a relay stays usable at 1920×1080 |

## 8. Security

- **Control is as strong as the user.** A helper in control can do whatever the user can in usdcc:
  run Python, open and save files, install plugins. That is why it is off twice (§5), asked for
  each session, always visible, and ended by one key.
- **Nothing outside usdcc** is shown or reachable: in-app capture (§3.1) and Qt event replay (§4).
- **The link holds a secret** and is good for one session. The connection is the room's, encrypted
  end to end as in [PEERS.md](PEERS.md) §6; a relay sees sizes and timing only.
- **Nothing is recorded** by usdcc. Start, control granted, and end are logged in the `usdcc.peers`
  category, without the link's secret ([LOGGING.md](LOGGING.md)).

## 9. Open questions

1. **The stop shortcut.** Something no panel uses and hard to hit by accident.

**Set aside:** a studio policy that forces or locks settings without a build of its own (a file next
to `usdcc.exe`, or a machine-wide `usdcc.ini` the user's can't override). A larger topic;
[SETTINGS.md](SETTINGS.md) has no such layer today. Set aside 2026-11-22.

**Decided 2026-11-22:** one helper at a time (§1); OS dialogs are operated by the user, not the
helper (§3.3); the build option on by default (§6); the build
option convention as its own plan ([BUILD_OPTIONS.md](BUILD_OPTIONS.md)).

## 10. Phase 0: the spike

### 10.1 What ran

[src/plugins/remote_support_spike](../src/plugins/remote_support_spike), a plugin for developers:
built only with `-DUSDCC_BUILD_REMOTE_SUPPORT_SPIKE=ON` (off by default) and never deployed. It
reuses Viewport Broadcast's `Broadcast` and `FrameSource` unchanged
([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md) §11.2), with a `FrameSource` of all usdcc's windows.
Its page is Broadcast's page plus the pointer, wheel and keys, sent to the host on a direct channel
open to guests. **Help > Remote Support Spike** has Start, Stop and Benchmark.
`USDCC_REMOTE_SUPPORT_SPIKE=1` starts it at launch, `USDCC_REMOTE_SUPPORT_SPIKE_BENCH=1` runs the
benchmark, and `USDCC_REMOTE_SUPPORT_SPIKE_OUT=<folder>` writes `link.txt`, `stats.txt` and
`bench.txt` there.

Run 2026-11-22 on one Windows 11 machine at 150 % scaling (a screen of 1920×1152 logical pixels),
with a test profile, `test_data/cube_and_sphere.usda` and Storm. The helper was a browser on the
same machine, over loopback. **Not tried:** a second machine, the LAN, a relay, macOS, Linux.

### 10.2 Capture

**It works.** One picture of every visible usdcc window, the native 3D view in place (filled in from
`grabFrame()`; a widget grab leaves it blank), with the user's and the helper's pointers drawn in.
Menus (their own popup windows), the Settings dialog and a floated panel were all composited in
their places.

*Measured*, 30 grabs each, the Benchmark entry:

| Picture | Grab, UI thread | of which widgets `grab()` | of which the 3D view | JPEG q60 / q85 at full size | at 1920 wide |
|---|---|---|---|---|---|
| 2560×1440 | **39.6 ms** (max 49.8) | 21.6 ms | 10.9 ms | 12.8 ms, 133 KB / 13.5 ms, 202 KB | 7.1 ms, 76 KB / 8.3 ms, 116 KB |
| 2880×1728 (maximized) | **48.3 ms** (max 50.6) | 26.5 ms | 12.6 ms | 17.4 ms, 164 KB / 17.6 ms, 244 KB | 7.9 ms, 78 KB / 8.5 ms, 118 KB |

The rest of a grab (about 7 ms) is the picture's allocation and compositing. JPEG runs on a pool
thread, as in Broadcast.

**What it means:**

- **The grab is the cost, not the JPEG,** and it is on the UI thread: about 40 ms a frame at
  2560×1440 caps a fully changing picture at about 20 a second and stalls the app that long each
  time. For support (clicks and forms) it is enough. Broadcast's pacing already grabs at most once
  per encode and nothing while nobody watches.
- **Phase 1 should grab only what changed:** keep each window's last picture and grab again only
  the windows that painted since (the spike already sees every `Paint` and `UpdateRequest`), and
  the 3D view only after `frameRendered()`. A click in a form then costs one window, not all.
- **The OS's title bars are missing.** `grab()` covers a window's client area. The main window
  draws its own title bar (qwindowkit), so it is complete, but dialogs and floating panels show
  without theirs, and the helper can't move them by it. Phase 1 draws a plain strip with the title
  in their place; moving them stays with the user.
- **Stacking is guessed** from the kind of window (main, tool, dialog, popup, tooltip), since Qt
  doesn't know the OS's order. Right for everything tried; two overlapping floating panels could
  be drawn in the wrong order.
- **OS capture (§3.2) isn't needed** for phases 1 to 3.

### 10.3 Input

**It works**, through `QWindowSystemInterface` (§4). From the browser:

- a click selects a prim in the Outliner; File opens its menu, and Escape closes it;
- typing into the Outliner's search filters it;
- shortcuts: **W** (Move tool), **Ctrl+Z** (undid a move), **Ctrl+,** (opened Settings);
- in the native 3D view: a click picks the sphere, a drag of the gizmo moves it, the wheel zooms;
- in other windows: **Close** in the Settings dialog, a panel's float button, a click in the floated
  panel.

**Found on the way:**

1. **Native pixels.** `QWindowSystemInterface` takes the platform's pixels. The first try passed
   logical ones: at 150 % every event landed at two thirds of its point and was ignored. The spike
   converts with `QHighDpi::toNativeLocalPosition()` and `toNativeGlobalPosition()`, as `QTest`
   does.
2. **A private Qt API.** `Qt6::GuiPrivate` needs `find_package(Qt6 COMPONENTS GuiPrivate)` since
   Qt 6.10, and private headers are tied to the exact Qt version. Fine for a plugin built and
   shipped with the app, which is the plan (§1); a plugin built outside against another Qt would
   break.
3. **Replaying is cheap,** a few milliseconds, except that delivery is synchronous: a click that
   opens a dialog takes as long as the dialog takes to build (648 ms for Settings).
4. **The wheel has no synchronous variant;** it is queued and the event loop delivers it.
5. **Text that isn't keys doesn't arrive.** The page only sends key events, so a paste, an input
   method or a browser's inserted text sends nothing. Phase 2 also sends the page's `input` and
   `compositionend` text.
6. **While a button is held,** everything goes to the window it was pressed in, as the OS's capture
   would. That is what makes the gizmo drag work.

**Not tried:** dragging between windows (docking a panel by its tab, where Qt Advanced Docking
follows the real cursor through `QCursor::pos()` and probably won't follow the helper's), double
clicks, right-click menus, input methods, the user always winning, latency over a real network.

### 10.4 Found in passing, not this plan's

- **This checkout didn't build without precompiled headers.**
  [peers_side_panel.h](../src/cpp/ui/include/usdcc/ui/peers_side_panel.h) declared a member as
  `class QVBoxLayout*` inside `usdcc::ui`, which declares a new `usdcc::ui::QVBoxLayout`; with the
  precompiled Qt headers the global one was already visible. Fixed with a forward declaration at
  global scope.
- **Configuring needs the Blender mesh backend** in `external/blender-mesh-install` (`tools/
  build_blender_mesh.py`), also for work that has nothing to do with it. The spike configured with
  `USDCC_BLENDER_MESH_ROOT` pointed at another checkout's.
  [BUILD_OPTIONS.md](BUILD_OPTIONS.md)'s `USDCC_BUILD_BLENDER_MESH_TOOLS` would let such a build
  leave it out.
