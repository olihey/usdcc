# Remote Support (plan)

A user who is stuck sends a **support request** to a colleague. The colleague sees usdcc's windows,
live, and, if the user allows it, works in them with mouse and keyboard.

**Status:** planned 2026-11-22. Nothing is built. "Checked" below means looked at in this
repository; everything else is an assumption until it's tried.

## 1. Decisions

- **Off twice.** A build option leaves the feature out of the build entirely (§6). When it is built,
  a setting turns it on, and that setting is **off by default** (§5). While it is off, the app shows
  no way to create a support request anywhere: no menu entry, no button, no greyed-out item.
- **usdcc's windows only, never the desktop.** The helper sees the app's own windows (main window,
  floating panels, menus, dialogs it draws) and nothing else on the screen.
- **Input as Qt events, never as OS input.** What the helper does is replayed as Qt events into the
  app's own windows. Nothing like `SendInput` is used, so the helper can't reach another program or
  the OS, and it works the same on Windows, macOS and Linux, Wayland included (§4).
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
- **What it can't capture:** native OS dialogs (file pickers, color pickers when native). During a
  session the app uses Qt's own dialogs instead, or the page says "A system dialog is open".

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

## 4. Input

- **The page sends events, not OS input:** pointer moves, buttons, wheel and keys, with the picture's
  coordinates and which window they belong to.
- **The plugin replays them** with `QCoreApplication::sendEvent()` (or `QWindow`'s events for the
  native 3D views) to the window under that point.
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
- **Removing the entries at runtime needs the app.** *Checked:* `Host::addViewportMenuAction()` has
  `applies`, which leaves an entry out when false, but `Host::addMenuAction()` (the main menu) has
  neither `applies` nor a way to remove an entry
  ([plugin.h](../src/cpp/plugin_sdk/include/usdcc/plugin/plugin.h)). Phase 1 adds `applies` to it.
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
- **The default is on,** so the feature exists in the public build but is off until a user turns it
  on. A studio that doesn't want it at all builds with `-DUSDCC_BUILD_REMOTE_SUPPORT=OFF`.
- **About Usdcc lists the options the build was made with,** so anyone can check.

**A convention for later.** The same pattern (an option per feature, the app refusing the plugin's
id when off, About listing it) suits other features a studio may want out: Viewport Broadcast, and
Peers as a whole for a build that never touches the network. Not part of this plan; §9.

## 7. Phases

| Phase | What | Done when |
|---|---|---|
| **0. Spike** | In-app capture of the main window with a viewport and a floating panel composited in; replaying a click and a key from a page | A picture with the 3D view in place; a button clicked and a text field typed into from the browser; milliseconds per frame at 2560×1440 |
| **1. View-only** | The CMake option, the plugin id refused when off, the settings, `applies` for main menu actions, Help > Request Support (link), the session bar and border, the page, ending | The entries exist only with the setting on; turning it off ends a session; a build with the option off has no plugin and refuses a copied one |
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
2. **A studio policy.** Should a studio be able to force a setting off (or lock it) without its own
   build, for example by a file next to `usdcc.exe` or a machine-wide `usdcc.ini` that the user's
   can't override? [SETTINGS.md](SETTINGS.md) has no such layer today.
3. **The build-option convention** (§6) for Viewport Broadcast and Peers: worth a plan of its own.
4. **Native dialogs** during a session: switch to Qt's dialogs, or show a placeholder?
5. **More than one helper** at a time, or one only.
