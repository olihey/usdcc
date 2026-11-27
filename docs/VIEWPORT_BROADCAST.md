# Viewport Broadcast (plan)

**Broadcast** on a viewport gives a web link. Whoever opens it in a browser sees that viewport,
live, with nothing to install. One watcher at a time may be allowed to navigate, and everyone can
chat.

**Status:** planned 2026-11-17. The phase 0 spike ran the same day on this machine; its numbers are
in §11.1. The LAN, a second machine and a tunnel weren't tried yet. "Checked" below means read in
this checkout or in the vcpkg checkout beside it, "measured" means run in the spike; everything
else is an assumption. The room it builds on, [PEERS.md](PEERS.md) phase 1a, is built, and with it
§8 items 5 and 6 (the ports, `find_package` and the deploy); [PEERS.md](PEERS.md) §14.1 says how.
**Phase 1 is built** (2026-11-17, §11.2): Broadcast in a viewport's menus, the page, the watchers,
Stop, and the room's chat. Checked on this machine; a browser on a second machine isn't tried yet.
**Phase 2 is built** (2026-11-23, §11.3): the Broadcast side panel. **Phase 3 is built** (2026-11-23,
§11.4): a watcher navigates once the host allows it. **Phase 4a is built** (2026-11-23, §11.5): the
Public address for a tunnel; 4b waits for the Peers relay. **Phase 5 is half built** (2026-11-23,
§11.6): Render Views as sources; H.264 is not built, and §11.6 says what it needs first.
[REMOTE_SUPPORT.md](REMOTE_SUPPORT.md) plans the same for all of usdcc's windows, with control.

## 1. Decisions

- **Pixels, not the scene.** The host renders and sends pictures. The watcher sees exactly what the
  host sees, with any renderer and any stage size, and no asset leaves the machine. Sending the
  stage to render in the browser is a different product (§13).
- **One port: a web page and a WebSocket.** The host serves the viewer page over HTTP and the
  stream over a WebSocket on the same port. One link, one firewall rule, and any HTTP tunnel or
  reverse proxy can carry it.
- **JPEG frames first, video later.** JPEG needs no new codec, works in every browser and on a
  plain `http://` link. Video through the browser's WebCodecs is the upgrade for links over the
  internet: H.264 as the floor, AV1 where both ends have it (§4).
- **Not WebRTC,** for now (§4.3). [PEERS.md](PEERS.md) §17 came to the same answer.
- **A C++ plugin,** `src/plugins/viewport_broadcast`, shipped like V-Ray Render and RTX Render. It
  opens no port until Broadcast is pressed.
- **A broadcast is a room** in the sense of [PEERS.md](PEERS.md): the host orders messages, the
  frames and channels are the same, and a browser is a guest in it (§7).
- **The room is built once, in `usdcc_peers`.** It is the first piece of Peers (its phase 1a), and
  this plugin is its first user. Decided 2026-11-17 (§7).
- **A watcher can only look.** With the host's permission one watcher can also move the camera.
  Nothing a watcher sends can select, edit, or run anything.

## 2. For the user

### 2.1 The host

- **A Broadcast submenu** in the viewport tab's right-click menu and its options menu: **Start
  Broadcast**, **Stop Broadcast** and **Copy Link**, the ones that don't apply greyed out. Start
  copies the link and shows a toast: "Broadcasting Viewport 1. Link copied", with **Copy Link**
  and **Stop**.
- **The viewport shows it is live:** a red border around it while it is sent (built, §11.2). The
  number of watchers is in the Peers panel and the status bar; whether it also belongs on the
  viewport is §12 question 4.
- **The Broadcast side panel:** the link, the watchers by name, who is navigating, the chat, and
  **Stop**. Per watcher: **Allow navigating**, **Take back**, **Remove**.
- **A watcher asks to navigate:** a toast, "Anna asks to navigate Viewport 1", with **Allow** and
  **Deny** ([NOTIFICATIONS.md](NOTIFICATIONS.md); *checked:* `ToastManager::show()` takes actions).
- **The host always wins.** Pressing a mouse button in the viewport while a watcher navigates
  pauses the watcher's input until release. **Take back** ends it.
- **Closing the viewport asks first:** "Viewport 1 is being broadcast. Closing it ends the broadcast
  for 2 watchers", with **Close and Stop Broadcast** and **Cancel**. Closing its stage or the app
  ends the broadcast without asking. The page says so.

### 2.2 The watcher

Opens the link, types a name (kept by the browser for next time), and sees the viewport, scaled to
fit the window. A bar holds the stage's name, the watchers, **Ask to navigate**, and the chat.
While navigating: drag to orbit, middle or shift drag to pan, wheel to zoom, **F** to fit, as in
the app.

## 3. What is sent

- **The viewport's own picture.** *Checked:* Storm draws into an offscreen buffer, then the grid,
  the selection outline, the collider overlay and the active tool's gizmo are drawn into it, and
  `HydraViewportWindow::grabFrame()` reads it back. The HUD is drawn afterwards on the window, so it
  is not in the picture.
- **Only when it changed.** The viewport draws on demand, so a still viewport sends nothing. A
  progressive renderer sends as it converges.
- **At the viewport's size,** up to a limit (1920 wide to start with). Resizing the panel changes
  the stream's size; the page fits whatever arrives.
- **Small while moving, sharp when still.** During an orbit or playback, frames go out at reduced
  size and quality. When nothing new has been drawn for a moment, one full-quality frame follows.
- **Beside the picture:** the stage's name, the renderer, the camera view, and the watchers.

## 4. Technology

### 4.1 Transport

| Part | Choice | Why |
|---|---|---|
| Page and stream | Qt Network's `QTcpServer` and Qt WebSockets, one port | Both LGPL-3.0 and in vcpkg. Not Qt HttpServer, which is GPL-3.0 only (see below and [RELEASE.md](RELEASE.md) §6). [PEERS.md](PEERS.md) serves the page and the stream for the broadcast's room |
| Messages | The frame of [PEERS.md](PEERS.md) §7.1: a JSON header, then raw bytes | A picture is the raw part. The browser reads it with `DataView` and `createImageBitmap` |
| The page | One HTML file with its script, no framework and no build step, read from the plugin's folder | Editable without rebuilding |

**Measured: both ways work** (Qt 6.11.1). Qt HttpServer was used first, and replaced by the
fallback:

- **Qt HttpServer as vcpkg ships it has no WebSocket upgrade.** Its WebSocket API
  (`newWebSocketConnection()`, `addWebSocketUpgradeVerifier()`) is compiled in only when Qt
  WebSockets is installed first, and the port doesn't depend on it, so it was built without
  (*checked:* vcpkg installed `qthttpserver` before `qtwebsockets`, and the library lacked the
  symbols). An overlay port that adds the dependency fixes it: the spike used one, and phase 1
  adds it as `cmake/ports/qthttpserver`. With it, the page and the WebSocket share one port, and
  `/ws` is accepted by a verifier.
- **The fallback works too:** a `QTcpServer` that peeks at the request, serves the page itself, and
  hands an upgrade to a `QWebSocketServer` that isn't listening. `handleConnection()` re-emits
  `readyRead` for a request already received (*checked* in its source), so peeking is enough. It
  needs one module fewer.

**The fallback is the choice** (`src/cpp/peers/src/page_server.cpp`, [PEERS.md](PEERS.md) §6.1).
Qt HttpServer was, from phase 1 until 26.11.11, through `QAbstractHttpServer::newWebSocketConnection`
and an upgrade verifier for `/ws`. It is available only under a commercial licence or GPL-3.0, with
no LGPL option, and the standard release is BSD-3-Clause and loads NVIDIA's proprietary RTX runtime
into the same process, so it was dropped ([RELEASE.md](RELEASE.md) §6). What a guest's page needs of
HTTP is small: a GET for a file, answered and closed, and the upgrade.

### 4.2 Pictures

| | JPEG frames | H.264 with WebCodecs |
|---|---|---|
| New dependencies | None: Qt writes JPEG | An encoder (below) |
| Browser | Any | Chrome, Edge, Safari, Firefox of the last two years |
| Works on a plain `http://` LAN link | Yes | **No.** WebCodecs exists only on `https://` pages and `localhost` (per its specification; *not tried*) |
| Data while orbiting | *Measured:* 13 to 21 Mbit/s at 1920×1415, 30 frames a second (§11.1); about a third of that at 1280×720 | Roughly 2 to 5 Mbit/s (*estimate*) |
| A still viewport | Nothing sent | Nothing sent |
| Good for | A LAN; the internet at a reduced size and rate | The internet |

So JPEG is the first version and stays as the fallback. H.264 is phase 5, and only matters once
links go over the internet, where the page is `https://` anyway (§5).

**The H.264 encoder** would be the machine's own: Media Foundation on Windows, which uses the GPU's
encoder where there is one. That adds no library and no patent licence of ours. FFmpeg (one API for
NVENC, AMF, Quick Sync and VA-API; *checked:* in vcpkg) is the alternative when Linux needs it.
A software encoder built from source (`openh264`, `x264`) is not planned: x264 is GPL, and
openh264's patent cover applies to Cisco's binary only.

**Why H.264 and not a newer codec.** It is the only one every host can encode on its GPU and
every browser can decode. The codec is negotiated, so it is the floor, not the limit:

| Codec | Host: GPU encoder | Browser: decoder | Here |
|---|---|---|---|
| H.264 | Any GPU of the last ten years | All | The floor |
| AV1 | Recent GPUs only (NVIDIA RTX 40, AMD RX 7000, Intel Arc and later) | Chrome, Edge and Firefox everywhere; Safari only on devices with an AV1 chip | Used when both ends have it: about a third fewer bits, sharper edges and text, no patent fees |
| HEVC | Most GPUs | Depends on the browser, the OS and the GPU | Not planned: patchy in browsers, and its licensing is the worst of the four |
| VP9 | Almost none | Most | Not planned: no GPU encoders |

A newer codec in software isn't an option: it would take the processor from a machine that is busy
rendering. The page tells the host what it can decode (`VideoDecoder.isConfigSupported()`), the
host picks the best it can encode on its GPU, and JPEG remains when there is nothing in common.
*The support columns are from memory, not tested;* the negotiation makes the plan independent of
them. AV1 is a second encoder behind the same interface, so it follows H.264 rather than
replacing it.

### 4.3 Why not WebRTC

It is what Unreal's and Omniverse's pixel streaming use, and it is the right tool at their scale.
Here it costs more than it gives:

- **It needs an encoder anyway,** and browsers only promise H.264 and VP8 for it, so JPEG's
  "no new dependency" start is lost.
- **It needs a server anyway** to introduce the two sides, and a relay for the routers hole
  punching fails on ([PEERS.md](PEERS.md) §1).
- **It is a large dependency:** `libdatachannel` (*checked:* in vcpkg, MPL-2.0) brings ICE, DTLS,
  SCTP and SRTP.
- **On a LAN it changes nothing,** and a LAN or a tunnel is where this starts.

What it would give is UDP (no stall on a lost packet) and its own rate control. Worth another look
if broadcasts over poor connections become common. The protocol above wouldn't change, only the
pipe the pictures take.

### 4.4 Keeping up

- **One frame in flight per watcher.** The page acknowledges each frame. The host sends that
  watcher the newest frame only then, and drops what was waiting. This is PEERS's "latest only"
  delivery, and it makes each watcher's rate follow their own connection.
- **Encoding is off the UI thread.** Reading the buffer needs the viewport's GL context, so it
  stays on the UI thread; compressing goes to a worker. One encode serves every watcher.
- **Reading back is the cost that matters** (*measured*, §11.1). `grabFrame()` takes 3 ms for a
  3-megapixel viewport that is still, and 13 ms while the camera moves: it waits for the GPU to
  finish the frame, on the UI thread. So the read moves to a pixel buffer object, which hands over
  the frame a little later without waiting. Compressing, on a worker, is 3 to 7 ms.

## 5. The link, and who it reaches

```
http://192.168.1.20:47800/#s=<secret>
https://<tunnel or relay>/b/8f3k2m#s=<secret>
```

The part after `#` is never sent in the HTTP request. The page's script reads it and gives it in
its first WebSocket message, so it stays out of proxy and server logs.

| The watcher is | Link | Needs |
|---|---|---|
| On this machine | `http://localhost:…` | Nothing |
| On this network | `http://<this machine's address>:…` | Windows Firewall's permission, once, as an administrator ([PEERS.md](PEERS.md) §5.2). Denied, the panel says so |
| Anywhere, phase 4a | The user's own tunnel (cloudflared, Tailscale Funnel, ngrok, a reverse proxy) pointed at the port, and its address in **Public address** in the settings | The tunnel. It works because everything is HTTP on one port. The tunnel's operator can see the stream |
| Anywhere, phase 4b | The Peers relay, `usdcc_peers.relay` ([PEERS.md](PEERS.md) §5.4), which also serves the page | The relay. Pictures and chat are encrypted between host and page with a key from the link's `#` part, so the relay sees sizes only |

A plain `http://` link on a LAN is not encrypted. The secret keeps strangers out; it doesn't hide
the stream from someone listening on that network. Phase 4b's encryption could be turned on for
LAN links too (a script library instead of the browser's own, which `http://` pages don't get).

## 6. Navigating

- **One navigator at a time.** Allowing a second watcher takes it from the first.
- **The watcher sends moves, not mouse events:** `orbit dx dy`, `pan dx dy`, `zoom d`, `fit`,
  `view <name>`, as fractions of the picture's size. The host applies them to that viewport's
  camera and refuses them from anyone who isn't the navigator.
- **The camera code is shared with the mouse.** *Checked:* `cameraState()` and `setCameraState()`
  are public, but orbit, pan and zoom live in the window's mouse handlers. They move into
  `orbitBy()`, `panBy()` and `zoomBy()`, which both use, so a Z-up stage, an upside-down orbit and
  an orthographic view behave the same from a browser.
- **It is the host's real camera.** The host sees it move, and so does every watcher. Nothing is
  recorded for undo, as for the host's own navigation.
- **How it feels** is one round trip: move, draw, compress, send, show. *Not measured.*

## 7. What it takes from Peers

[PEERS.md](PEERS.md) isn't built yet either. This plan uses its ideas now and its code later:

| Peers | Here |
|---|---|
| A room, hosted by one instance, which orders the messages | A broadcast |
| A peer | A *guest*: a browser. It has a name, no key and no USD |
| Channels and the frame of §7.1 | `view` (pictures, latest only), `nav` (direct, to the host), `chat` and `presence` unchanged |
| The secret in an invite link | The secret in the web link |
| "Anna wants to join": Allow, Deny | "Anna asks to navigate": Allow, Deny |
| Roles: view-only, remove a member | Watching, navigating, **Remove** |
| The relay forwards what it can't read | The same relay, also serving the page |
| Off until asked; nothing received acts by itself; limits on size and rate | The same |

**Not all of Peers first, but its first piece** (decided 2026-11-17). The two plans overlap in one
place: hosting a room over a WebSocket, the frame, channels with their three kinds of delivery,
roles, and limits. That is [PEERS.md](PEERS.md) phase 1a, and it is what this plugin's server
would be anyway. So it is written once, as the start of `usdcc_peers` in `src/cpp/peers/`, with
Peers's `Room` and `Channel` API, and the plugin is its first user.

- **Which room.** Broadcast hosts a room for the broadcast if this instance hosts none. Once Peers
  has rooms of its own, a broadcast started while hosting one joins that room, and its members see
  it too.

- **What waits for Peers itself:** identity and keys, the encrypted handshake, discovery,
  reconnecting by `seq`, transfers, the Python module, the relay, and all of the shared stage.
  None of it is needed to watch a viewport on a LAN.
- **What Peers gains:** its channels are proven by a demanding user (binary frames, thirty a
  second, latest only) before the shared stage depends on them, and part of its spike is answered
  (the firewall, hosting).
- **What Peers had to allow for:** a member that is a browser, a *guest*
  ([PEERS.md](PEERS.md) §6.1). It has no key pair, the room serves it a page on the same port, it
  reaches only the channels opened to guests, and it is encrypted through a relay but not on a
  plain LAN link (§5).
- **Later, for free:** a broadcast's chat is the room's chat, so people in browsers and people in
  usdcc talk in one place, and a shared stage can have a broadcast for those without usdcc.

Building all of Peers's phases 1 and 2 first would delay this by the larger part of that work for
no gain here. Building this entirely alone would mean writing the server twice.

## 8. What changes in the app

Small and general, so other plugins gain from them too:

1. **`HydraViewportWindow::frameRendered()`.** *Checked:* today there is `firstFrameRendered()`
   and `cameraChanged()`, nothing per frame, and `renderNow()` is called directly from many
   places, so a plugin can't watch for it either. The spike polled instead (*measured*): a still
   viewport then costs 60 reads a second only to find nothing new, and the wait for the next poll
   was most of the delay between a move and its picture (§11.1). **Built** (phase 1): after every
   frame that reached the screen.
2. **`orbitBy()`, `panBy()`, `zoomBy()`** (§6). **Built** (phase 3, §11.4).
3. **Entries in a viewport's menus from a plugin:** `Host::addViewportMenuAction()` and
   `usdcc.app.add_viewport_menu_action()`, like the Outliner's prim menu
   ([PLUGINS.md](PLUGINS.md) §1). *Checked:* there is no such extension point. **Built** (phase 1):
   `usdcc::usd::ViewportMenuEntries`, at the end of the options menu and the tab's menu (the 3D
   view has no right-click menu of its own), optionally checkable, and greyed out when they don't
   apply (`enabled`).
4. **A mark on a viewport from a plugin** (the live mark). **Built** as a border:
   `ViewportViewPanel::setBorderColor()`. Nothing draws over the 3D view's native window, so the
   border is a 3-pixel margin of the frame around it, painted in the colour; the view, and with it
   the picture sent, is 6 pixels smaller each way while it shows.
5. **Build:** `qtwebsockets` in `vcpkg.json`; `Network` and `WebSockets` in `find_package(Qt6 …)`.
   (`qthttpserver` and its overlay port too until 26.11.11, §4.1.) New ports need a
   private vcpkg root in a worktree ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §12).
6. **Deploy,** beside `usdcc.exe`, where the plugin, Peers and MCP all find them. *Measured:*
   each of these was missing at first and stopped the spike:
   - `Qt6Network.dll`, `Qt6WebSockets.dll` (and, until 26.11.11, `Qt6HttpServer.dll` and its
     `Qt6Concurrent.dll`). Not beside
     the plugin: the app loads plugins with `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, which doesn't
     search the plugin's folder for what it links, nor `PATH`.
   - `libcrypto-3-x64.dll`. Qt Network as built here links OpenSSL at load time, even with no
     TLS in use. PySide6 ships a file of the same name; both are OpenSSL 3.6.3 today
     (*checked*), so they don't clash yet ([PEERS.md](PEERS.md) §13).
   - `imageformats/qjpeg.dll` and `jpeg62.dll`. Without them `QImageWriter` writes nothing and
     says so only by returning false. Only `platforms/qwindows.dll` is deployed today.

The plugin (`usdcc_add_plugin(viewport_broadcast …)`): a room from `usdcc_peers` (§7) with the
page served beside it, the frame source, the page in `web/`, and its settings. As built, §11.2.
The page's colors and type are `usdcc.css`, the app's dark theme as CSS that every page usdcc
serves shares ([RENDER_NODE.md](RENDER_NODE.md) §4.5); the plugin's build copies it beside the page.

**Sources other than the Storm viewport.** The frame source is an interface: "a new picture",
"read it", "move the camera". The viewport is the first. A Render View panel (V-Ray, RTX) already
shows `QImage`s and follows the viewport's camera, so it is a natural second (phase 5).

## 9. Security and privacy

- **No port is open until Broadcast,** and it closes on Stop.
- **The link is the invitation:** a 128-bit random secret, new for each broadcast. A setting makes
  the host approve each watcher as well.
- **A watcher's messages are a closed list** (hello, ack, chat, ask, and the moves of §6), each
  with a size and a rate limit. Anything else closes the connection.
- **Chat is shown as text** on both sides, never as HTML.
- **The WebSocket checks `Origin`,** so another web page can't connect from a watcher's browser.
- **A limit on watchers** (8 to start with).
- **The picture shows what the viewport shows,** including the selection. The stage's name is
  sent; file paths are not.
- **The secret is never logged.** The log category is `usdcc.broadcast`.

## 10. Testing

- **`tests/viewport_broadcast_test.cpp`,** over `127.0.0.1` with a `QWebSocket` as the watcher: a
  wrong secret is refused; a frame arrives after the viewport draws; a second isn't sent before
  the first is acknowledged; chat goes round; a move without the role is refused, with it the
  camera moves, and after **Take back** it is refused again; Stop closes the port.
- **The page** has no test framework. Its protocol half is covered by the test above; the rest is
  looked at in a browser.
- **By hand:** the app under `USDCC_TEST_PROFILE`, the link opened in the built-in browser on this
  machine, then on a second machine and a phone.

## 11. Phases

| Phase | Builds | Done when |
|---|---|---|
| **0. Spike** | A throwaway plugin: the server, `grabFrame()` on a timer, a bare page | It answers: does Qt HttpServer pass upgrades to Qt WebSockets here; milliseconds to read back and compress 1920×1080; bytes per frame; how an orbit feels on the LAN and through a tunnel; what Windows Firewall asks. **On this machine: done** (§11.1); the LAN, the tunnel and the firewall remain |
| **1. Watch** | The room and channels in `usdcc_peers` (§7); §8 items 1, 3, 5 and 6; the plugin with Broadcast, the link, the page, the watcher list, Stop | A browser on a second machine follows an orbit and an edit, a still viewport sends nothing, and Stop closes the port. **Built** (§11.2); all of it checked on this machine, not yet from a second one |
| **2. Chat** | Names, the chat on both sides, the side panel, toasts | A watcher and the host chat. **Built** (§11.3) |
| **3. Navigate** | §8 item 2; ask, allow, deny, take back; the moves | A watcher orbits the host's viewport; the host takes it back with a click. **Built** (§11.4); not yet tried from a browser |
| **4. Beyond the LAN** | a: the **Public address** setting. b: the relay and encryption, with [PEERS.md](PEERS.md) phase 4 | a: a watcher on a phone network watches through a tunnel. b: the relay's log shows sizes only. **4a built** (§11.5), not yet tried through a real tunnel; **4b waits** for the relay, which `usdcc_peers` doesn't have yet |
| **5. Sharper and wider** | H.264 where the page allows it; Render View panels as sources | A 1080p orbit over the internet stays under 5 Mbit/s; an RTX render is broadcast. **Render Views built** (§11.6), except RTX, which has no picture to read; **H.264 not built** (§11.6) |
| **Later** | A watcher's pointer shown to all; drawing on the picture; several broadcasts at once | |

Phases 0 to 3 need no server and no new codec.

### 11.1 The spike's results

Run 2026-11-17 with a throwaway C++ plugin, not kept in the repository, that polls the active viewport's `grabFrame()`, skips a frame equal to the last by its hash, compresses
on a worker thread with Qt's JPEG writer (libjpeg-turbo), sends each watcher the newest frame once
the last is acknowledged, and moves the camera with `setCameraState()` for a page's drag. The
watcher was the desktop app's built-in browser on the same machine, so the network is loopback.
Release build, GeForce RTX 4090, a 3072×1728 screen at 125 %.

| Question | Answer |
|---|---|
| Qt HttpServer passes upgrades to Qt WebSockets? | Yes, once built with it: an overlay port (§4.1). No longer used: GPL-3.0 only |
| Read back, 2033×1498 | 3 ms when still; **13 ms while the camera moves**, waiting for the GPU (§4.4) |
| Compress, 1920×1080 | 2.6 to 2.8 ms at quality 50 to 90 (1280×720: 1.2 ms; 960×540: 0.7 ms) |
| Compress, 1920×1415 as sent | 5.6 to 7 ms on a worker |
| Bytes a frame, 1920×1080 | `cube_and_sphere.usda`: 55 KB at q50, 71 KB at q75, 107 KB at q90. `physics_playground.usda`: 43, 53, 81 KB |
| Orbit, sustained (camera turned 60 times a second) | **30 frames a second** at 1920×1415, q60, 55 KB a frame, **13.5 Mbit/s** |
| Orbit by dragging in the page | Frames follow the drag's ~22 moves a second; 105 KB a frame at q80, 18 to 21 Mbit/s |
| A move in the page to its picture on screen | **64 ms** median polling every 33 ms (Windows' coarse timer fires every ~46 ms); **25 ms** polling every 16 ms with a precise timer, the stand-in for `frameRendered()` |
| Decoding in the page | 4 to 5 ms a frame (`createImageBitmap`) |
| A still viewport | Nothing sent after the first frame, but polling read it back 60 times a second to know |
| Chat, presence, a wrong secret | Chat goes both ways; the watcher list updates; a wrong secret closes with "Wrong link" |
| The fallback server | Same numbers |

**What it changes:**

- **Polling is out:** `frameRendered()` (§8 item 1) is needed for both the cost when still and the
  delay when moving.
- **The read back is the one cost on the UI thread worth fixing,** with a pixel buffer object
  (§4.4). Compressing is cheap enough on a worker.
- **JPEG is good enough on a LAN,** as planned: 30 frames a second of 1920×1415 is 13 to 21 Mbit/s.
  Over the internet that wants the half size the plan already sends while moving (about a third of
  the bytes), or video.
- **Deploying** needs seven more files (§8 item 6).

**Not tried yet:**

- **The LAN and a second machine,** and with them **what Windows Firewall asks:** binding to the
  network raises its prompt for this `usdcc.exe` (*checked:* it has no rule yet), which is the
  user's to answer.
- **A tunnel** (cloudflared, Tailscale Funnel), and how an orbit feels through one.
- **Another browser, a phone,** a large stage, a progressive renderer.

**Found on the way, not this plan's:** a fresh Windows configure fails to find OpenUSD built with
Python 3.13. The pybind11 overlay port brings vcpkg's Python 3.12, whose CMake wrapper takes over
OpenUSD's `find_dependency(Python3 3.13 EXACT)`. The spike set the wrapper aside in its own
install tree.

### 11.2 Phase 1 as built

[src/plugins/viewport_broadcast](../src/plugins/viewport_broadcast), bundled and enabled like Jolt
Physics, and shipped (`tools/deploy.py`):

- **`usdcc_viewport_broadcast_core`** (`broadcast.h`, `.cpp`; Qt and `usdcc_peers` only), a static
  library the plugin and the tests share. `FrameSource` is "a new picture", "grab it", "its
  title", "gone"; `Broadcast` hosts the room and sends the pictures.
- **The plugin** (`plugin.cpp`): the Broadcast entry, the viewport as a `FrameSource`, toasts.
- **The page** (`web/index.html`): one file, no build step, served by the room at `/`.

**In the app:**

- **`HydraViewportWindow::frameRendered()`** (§8 item 1).
- **`ViewportMenuEntries`** (§8 item 3), with `Host::addViewportMenuAction(label, run, checked,
  applies)` for C++ and `usdcc.app.add_viewport_menu_action(label, callback, checked=None,
  applies=None, id=None)` for Python, whose callback gets a `usdcc.app.Viewport`.
- **`Host::peers`:** a C++ plugin gets the app's `PeersController`, through
  `PluginManager::setPeers()`.

**Where it differs from the plan:**

- **Its own room,** hosted through the app's `Peers`: it shows in the Peers panel with its watchers
  and chat, and in the status bar. Not a room this instance already hosts (§7): whether a room takes
  guests is fixed when it is hosted.
- **Its own port,** 47810, not Settings > Peers' (a room of the user's may have that); taken, any
  free one. On the network by default, so Windows asks once. Settings, with no page in Settings yet:
  `PluginSettings/org.usdcc.viewport-broadcast/onNetwork`, `port`, and `allowedOrigins` (a tunnel's
  address). `USDCC_BROADCAST_LOOPBACK` keeps it on 127.0.0.1, for checks.
- **The channel** is `org.usdcc.viewport-broadcast/view` (a plugin's channels start with its id):
  latest only, only the host sends, open to guests. A newcomer gets the last picture first, as the
  channel's `snapshot`, so it doesn't wait for the next change.
- **No acknowledgements.** The room's latest-only queue (a frame goes to a socket only while less
  than 128 KB waits unwritten, [PEERS.md](PEERS.md) §14.1) does what the spike's acks did: a slow
  watcher gets fewer pictures, never older ones. One compression serves every watcher.
- **Pacing:** a frame is grabbed after `frameRendered()`, at most one compression at a time, and the
  newest picture when it's done. Quality 60 while the picture changes; 250 ms after the last change,
  the same picture at 85, scaled smoothly. Wider than 1920 is scaled down. A picture equal to the
  last one sent (by its hash) isn't sent. Nothing is grabbed while nobody watches.
- **Reading back is still `grabFrame()`,** synchronous on the UI thread: the pixel buffer object of
  §4.4 isn't built.
- **Chat comes with it,** part of phase 2: the room's `chat` is opened to guests, the page shows and
  sends it, and the host has it in the Peers panel. Lines nobody said (the host's "You host …") are
  left out of the page.
- **The watcher list** is the Peers panel's, and a toast when someone starts watching. No side panel
  of its own, no "ask before a watcher joins".
- **The menu** is a Broadcast submenu: Start Broadcast, Stop Broadcast, Copy Link, greyed out
  when they don't apply to that viewport (2026-11-18; at first a single checkable Broadcast).
- **The live viewport has a red border** (`#e5534b`, the page's LIVE red), from Start until the
  broadcast ends (2026-11-18).
- **Closing a broadcast viewport asks** (2026-11-18): Close and Stop Broadcast, or Cancel. It is a
  close guard, `ViewportViewPanel::addCloseGuard()`, asked by the tab's close button, the tab
  menu's Close, closing its dock area or floating window, and a workspace switch: a viewport now
  has ADS's `CustomCloseHandling` and hides itself once every guard agrees. A viewport hidden
  some other way ends its broadcast, rather than leave its watchers on the last picture.
- **It ends** with Stop Broadcast, the toast's Stop, the viewport or its stage closing, and quit.
  One at a time: Start in another viewport ends the first. The host gets one toast, "The broadcast
  ended": the Peers controller no longer adds "Left …" for a room this instance hosts and closed
  itself (2026-11-18); a peer that was in the room still gets it.

**Tests:** in `usdcc_ui_tests` (`viewport_broadcast_test.cpp`): a watcher gets the picture, as a
JPEG of the source's size and colour; a still picture sends nothing more after its sharp one, and a
change does; a newcomer gets the picture at once without a grab; the page is served, a wrong secret
is refused, and a watcher may not send pictures; Stop tells the watcher why and closes the port; the
broadcast ends with its source; the viewport menu's entries (options menu and tab menu, checked,
left out when they don't apply, removed). In `usdcc_python_tests`: `add_viewport_menu_action()`.
The UI tests now get Qt's plugin folder (`QT_PLUGIN_PATH`), for the JPEG writer.

**Checked in the app** (Release, a test profile, `USDCC_BROADCAST_LOOPBACK`, the desktop app's
browser on this machine; a script opened the viewport tab's menu and pressed Broadcast): the entry
is in the menu, unchecked, then checked while live; the link is copied; the page shows the stage's
title, the picture, LIVE and the watchers. A box moved 16 times a second sent 96 pictures in six
seconds at about 29 KB (an 883×741 viewport), then one sharp one of 48 KB, then nothing while still.
Fit All Views sent two. Chat went from the page through the host and back. A second watcher got the
picture and the chat at once, and both listed both. Pressing Broadcast again closed the room, the
page said "The broadcast ended", and port 47810 refused connections. Quitting while live closed the
room cleanly.

**The submenu and the border, checked in the app** (2026-11-18, the same way): before Start, the
tab menu's Broadcast submenu has Start Broadcast enabled and Stop Broadcast and Copy Link greyed
out; while live, the other way round. Start, Copy Link and Stop Broadcast each did their part. A
grab of the window showed the red ring around the viewport while live, and none after Stop (a
widget grab can't show the 3D view inside it, so the view's own pixels weren't seen in it).
Closing the broadcast viewport from its tab's menu showed the warning; Cancel kept the viewport
and the broadcast, and Close and Stop Broadcast closed the viewport, ended the broadcast ("The
viewport closed") and closed the port.

**Tried 2026-11-22 from WSL 2** (another address behind NAT, through Windows Firewall): the
broadcast started on `0.0.0.0:47810` with its link on the LAN address, but a watcher in WSL timed
out in the WebSocket handshake. Windows had made a Block rule for that `usdcc.exe` (its prompt
cancelled). The app said nothing: no toast, nothing in the log, although §5's table says a denial
shows in the panel. **With the rule set to Allow, it worked:** a watcher in WSL (a Python
WebSocket client doing what the page does: the guests' `hello` with the link's secret) connected
in 0.04 s, got `welcome` and the chat's snapshot, and its "hello from WSL" came back to it as the
room's `say`. The host logged "WSL watcher is watching". The script changed the stage's time 5
times a second, and the watcher got **101 pictures in 20 s (5 a second, one per change), 28 KB
each on average (one sphere, the viewport at its default size), 1.2 Mbit/s**. The first message on the
view channel was a snapshot with no picture in it (0 bytes), which the page skips.

**Not tried yet:** a browser on another machine, and with it Windows Firewall's question; a tunnel;
another browser or a phone; a progressive renderer; closing the viewport or its stage in the app
(the tests cover the source going away). The plugin built outside this tree against the SDK: its
`UsdccPluginConfig.cmake` doesn't yet find Qt Network and WebSockets, which `usdcc_peers`' headers
need.

**Found on the way, for Peers:** the host's chat snapshot sends a newcomer the lines of the host's
own panel ("You host …", "Anna joined"), worded for the host. The page filters them; a peer joining
a room sees them in its Peers panel.

### 11.3 Phase 2 as built

Most of phase 2 came with phase 1: the page asks for a name (kept by the browser), the chat goes
both ways through the room's `chat` channel, the host has it in the Peers panel, and Peers' chat
toasts (Settings > Peers) show a watcher's line while the Peers panel is hidden. What phase 2
added is **the Broadcast side panel** (`broadcast_panel.h`, `.cpp`, in a static library of its own,
`usdcc_viewport_broadcast_ui`, which the plugin and the tests link):

- **What it shows:** what is broadcast and to how many ("Broadcasting Viewport 1 · shot.usda to 2
  watchers"), the link with **Copy**, **Stop**, the watchers by name, and the broadcast's chat with
  a line to type in. With no broadcast: "Not broadcasting", and where Start Broadcast is. After
  one ends: why.
- **Per watcher: Remove** (select, then Remove): the room removes them, and their page says "The
  host removed you from the broadcast". They can come back with the link; a new broadcast has a
  new one.
- **The chat is the Peers panel's,** from `PeersController::chat()` and `chatAdded`; the host's line
  goes out through `say()`, so both panels show the same lines. Text only.
- **Where it is:** a side panel on the right, hidden until asked for: the View menu, or **Watchers**
  on the "Broadcasting …" toast (which opened the Peers panel before). A saved layout shows it
  where it was.
- Not in it yet: **Allow navigating** and **Take back** (phase 3).

**Tests:** `broadcast_panel_watchers_and_chat` (`usdcc_ui_tests`): the idle text, the link, a
watcher listed and counted, their chat line in the panel, the host's line from the panel reaching
the watcher, Remove, Stop asking to stop, and the panel saying the broadcast ended. **Checked in
the app** (test profile): the panel exists, starts hidden, is in the View menu, and says "Not
broadcasting" when shown. Starting a broadcast from a script wasn't reached (the viewport menus
are rebuilt each time they open); the test covers the panel while live.

### 11.4 Phase 3 as built

- **In the viewport** (`HydraViewportWindow`, §8 item 2): `orbitBy(dx, dy)`, `panBy(dx, dy)` and
  `zoomBy(steps)`, in the window's pixels and wheel notches. The mouse handlers and these share
  `orbitStep()` and `zoomStep()`, so half a degree a pixel, the upside-down orbit, a Z-up stage and
  0.9 a notch are the same either way. Each leaves a stage camera for the orbit camera first, as a
  drag does; an orthographic axis view doesn't orbit. `panBy()` keeps what's in the middle of the
  view under the same spot, as a drag does with what's under the cursor.
  `isMouseNavigating()` says the host's own mouse is on the camera or a tool.
- **The channel** `org.usdcc.viewport-broadcast/nav`: direct, open to guests. A watcher sends
  `ask` and `cancel`, and while it navigates `orbit {x, y}` and `pan {x, y}` (fractions of the
  picture, at most 1), `zoom {steps}` (at most 10) and `fit`. The host sends every watcher
  `navigator {id, name}` (empty when nobody), when it changes and to a newcomer, and the one it
  turned down `denied`.
- **The rules** (`Broadcast`): one navigator; allowing another takes it from the first. A move
  from anyone else, out of range, or while the host's mouse is down on the viewport is dropped
  and counted (`Stats::movesRefused`): the host always wins. Any other message type on the
  channel removes the page (§9's closed list). A navigator who leaves, or sends `cancel`, ends
  it. Nothing is recorded for undo.
- **The host:** "Anna asks to navigate Viewport 1" as a toast that stays, with **Allow** and
  **Deny**. In the Broadcast panel the watchers show "(navigating)" or "(asks to navigate)", with
  **Allow Navigating** for the one selected and **Take Back**.
- **The page:** **Ask to navigate** in the bar (then **Cancel asking**, then **Stop
  navigating**), and who navigates. While it's the watcher: drag to orbit, shift or middle drag
  to pan, wheel to zoom, F to fit; the picture gets an accent outline and a grab cursor. Moves
  are added up and sent at most once a frame.
- **Fit** is the viewport's Fit to View (the host's selection, or the stage).

**Tests:** `broadcast_navigating` (`usdcc_ui_tests`): the nav channel is open to watchers; a move
before being allowed is refused; ask reaches the host, deny tells the watcher, ask again and allow
make them the navigator and tell the others; orbit, zoom and fit arrive as sent; another watcher's
move, one out of range, and one while the host's hand is on it are refused; Take back tells the
watcher and refuses the next move; a message outside the list removes the page.
`viewport_navigates_by_moves` (`usdcc_python_tests`): orbit, zoom and pan move the camera as the
mouse does and say so. The page's script parses (`node --check`); **not yet tried in a browser.**

### 11.5 Phase 4a as built

- **Settings > Peers > Viewport Broadcast** (the plugin defines them; the keys are the ones
  phase 1 read, `PluginSettings/org.usdcc.viewport-broadcast/…`): **Broadcast on the network**,
  **Port**, and the new **Public address**. `allowedOrigins` stays a key without a page.
- **Public address:** the address of the user's own tunnel or reverse proxy pointed at the
  port, such as `https://watch.example.com/`, a path allowed. Only an `http` or `https` address
  with a host is taken. With it:
  - the link is the tunnel's: `https://watch.example.com/studio/#s=<secret>` (a path gets its
    trailing `/`, so the page's files resolve under it). The Broadcast panel and Copy Link give
    that one; `Broadcast::localLink()` keeps this machine's.
  - the tunnel's origin may open the WebSocket, as if it were in `allowedOrigins`.
- **The page** opens its WebSocket beside itself (`ws` relative to the page, `wss:` on an `https`
  page) rather than at the host's root, so a proxy that serves it under a path carries the
  pictures too.
- As §5 says, the tunnel's operator can see the stream; the setting's description says so.

**Tests:** `broadcast_public_address` (`usdcc_ui_tests`): the link is the tunnel's with the
secret and a trailing `/`, the local link stays this machine's, a page from the tunnel's origin
connects and one from another origin doesn't, and without the setting the link is as before.
**Not tried yet:** a real tunnel (cloudflared, Tailscale Funnel) and a watcher on a phone network.

### 11.6 Phase 5 as built: Render Views as sources

- **`RenderViewPanel`** says when it shows a new picture (`imageShown()`: `showImage()`, a linear
  image once it's through the view transform, `showPresentedFrame()`) and gives it
  (`currentImage()`, virtual: `image()` here). So V-Ray Render and any Python renderer's view
  can be broadcast, in display colors.
- **RTX Render presents on its own** (a native window, `showPresentedFrame()`): it has no
  `QImage`, `currentImage()` is null, and nothing is sent. It needs a read-back of its frame
  (an override of `currentImage()`), which is RTX's to add. So "an RTX render is broadcast"
  isn't done.
- **Starting one:** the Broadcast panel has a source list and **Start**: every open viewport and
  Render View, read when the list opens. The viewport menus' Broadcast is as before. Render
  Views have no menu entry of their own: a plugin can't add to their menus yet.
- **A Render View can't be navigated** (`FrameSource::canNavigate()`, false unless the source
  says so; a viewport does): the nav channel isn't opened, so the page shows no **Ask to
  navigate**. It follows a viewport's camera or one of the stage's, which the host moves.
- No red border and no close guard for a Render View: closing it ends the broadcast ("The
  Render View closed").

**Tests:** `broadcast_sources` (`usdcc_ui_tests`): a source that can't be navigated offers no
nav channel and nobody can be allowed; the panel lists the sources it's given and Start asks
for the one chosen; the list is off while live. `render_view_panel_image_shown`: a picture shown
says so and is `currentImage()`. **Not tried in the app** with a real renderer.

### 11.7 H.264: not built, and why

The plan (§4.2) assumed the room's latest-only delivery carries video as it carries JPEG. It
doesn't: a JPEG stands alone, but most H.264 frames are differences from the frame before. Latest
only drops frames for a slow watcher, and the next difference then decodes into garbage until a
key frame. So H.264 needs, before an encoder:

1. **Per-watcher delivery that never drops a difference:** an ordered stream per watcher with
   its own small budget, and when that budget is spent, a request for a key frame instead of
   dropping. The room has nothing per watcher like that; it is a `usdcc_peers` change.
2. **Key frames on demand:** for a newcomer (its snapshot must be a key frame), for a watcher
   that fell behind, and when the size changes. One encoder then serves everyone only if all
   watchers take the same key frames, which costs the fast ones bandwidth; one encoder per
   watcher costs the GPU. To decide.
3. **Two streams at once:** a watcher on a plain `http://` LAN link has no WebCodecs (§4.2) and
   keeps JPEG, so a broadcast to both sends both.
4. Then the encoder (Media Foundation's H.264 MFT, NV12 input, Annex B out) and the page's
   `VideoDecoder`, negotiated as §4.2 says.

This is its own piece of work, best planned with PEERS.md's relay (4b), which is where the
bandwidth matters.

## 12. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Build it alone, after Peers, or with it? | **Decided:** with it. The room and channels are built once, as the first piece of `usdcc_peers`, and this is their first user. The rest of Peers follows in its own time (§7) |
| 2 | Links beyond the LAN in the first version? | Phase 4a only: the user's own tunnel. The relay comes with Peers's |
| 3 | Must the host approve each watcher? | No, the link is the invitation, as in Peers. A setting to ask |
| 4 | Where is the live mark: the HUD, or the panel's title? | **Decided:** a red border around the viewport (built, §8 item 4). The number of watchers in the HUD as well is still open |
| 5 | Is the HUD in the picture? | No. The scene, the selection and gizmos only, which is what the buffer holds |
| 6 | Several broadcasts at once (two viewports)? | One per instance at first. The link's path leaves room |
| 7 | Shipped and on by default? | Shipped and enabled, like the render plugins. It does nothing until Broadcast |
| 8 | May a navigator also change the time, or select? | Not at first: camera only. Time is the next candidate |

## 13. Not planned

- **Sending the stage to the browser** (USD in WebAssembly, three.js). The watcher could look
  around freely, but it wouldn't be the host's viewport or renderer, a large stage wouldn't load,
  and the assets would leave the machine.
- **WebRTC,** for now (§4.3).
- **A watcher editing.** That is what a shared stage between peers is for
  ([SHARED_STAGE.md](SHARED_STAGE.md)).
- **Voice and video,** as in Peers.
- **A public relay run by the project** ([PEERS.md](PEERS.md) §16 question 3).
- **Recording** a broadcast.
