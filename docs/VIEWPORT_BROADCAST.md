# Viewport Broadcast (plan)

**Broadcast** on a viewport gives a web link. Whoever opens it in a browser sees that viewport,
live, with nothing to install. One watcher at a time may be allowed to navigate, and everyone can
chat.

**Status:** planned 2026-11-17. Nothing is built yet. "Checked" below means read in this checkout
or in the vcpkg checkout beside it; everything else is an assumption until the spike (§11).

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

- **Broadcast** in the viewport's options menu and its right-click menu. It starts the broadcast,
  copies the link, and shows a toast: "Broadcasting Viewport 1. Link copied", with **Copy Link**
  and **Stop**.
- **The viewport shows it is live:** a red mark with the number of watchers. *Where exactly (the
  HUD, the panel's title) is §12 question 4.*
- **The Broadcast side panel:** the link, the watchers by name, who is navigating, the chat, and
  **Stop**. Per watcher: **Allow navigating**, **Take back**, **Remove**.
- **A watcher asks to navigate:** a toast, "Anna asks to navigate Viewport 1", with **Allow** and
  **Deny** ([NOTIFICATIONS.md](NOTIFICATIONS.md); *checked:* `ToastManager::show()` takes actions).
- **The host always wins.** Pressing a mouse button in the viewport while a watcher navigates
  pauses the watcher's input until release. **Take back** ends it.
- **Closing the viewport, the stage or the app ends the broadcast.** The page says so.

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
| Page and stream | Qt HttpServer and Qt WebSockets, one port | Both in vcpkg (*checked:* ports `qthttpserver` and `qtwebsockets` 6.10.1 in the checkout beside this one). Qt Network is already built (*checked:* `Qt6Network.dll` is in `vcpkg_installed`); nothing links it yet. [PEERS.md](PEERS.md) and [MCP.md](MCP.md) plan the same modules |
| Messages | The frame of [PEERS.md](PEERS.md) §7: a JSON header, then raw bytes | A picture is the raw part. The browser reads it with `DataView` and `createImageBitmap` |
| The page | One HTML file with its script, no framework and no build step, read from the plugin's folder | Editable without rebuilding |

*Not tried:* that Qt HttpServer hands a WebSocket upgrade on its own port to Qt WebSockets in this
Qt (the API arrived in Qt 6.8). If it doesn't, a `QTcpServer` that serves the one page and passes
upgrades to `QWebSocketServer::handleConnection()` does the same with one module fewer.

### 4.2 Pictures

| | JPEG frames | H.264 with WebCodecs |
|---|---|---|
| New dependencies | None: Qt writes JPEG | An encoder (below) |
| Browser | Any | Chrome, Edge, Safari, Firefox of the last two years |
| Works on a plain `http://` LAN link | Yes | **No.** WebCodecs exists only on `https://` pages and `localhost` (per its specification; *not tried*) |
| Data, 1280×720 while moving | Roughly 15 to 30 Mbit/s at 30 frames a second (*estimate*) | Roughly 2 to 5 Mbit/s (*estimate*) |
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
- **Reading back costs a frame's worth of time** (`grabFrame()` is a synchronous read and a
  copy). *Not measured.* If it shows while orbiting, the read moves to a pixel buffer object,
  which returns a frame later without waiting.

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
| Anywhere, phase 4b | `usdcc-relay` ([PEERS.md](PEERS.md) §5.4), which also serves the page | The relay. Pictures and chat are encrypted between host and page with a key from the link's `#` part, so the relay sees sizes only |

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
| Channels and the frame of §7 | `view` (pictures, latest only), `nav` (direct, to the host), `chat` and `presence` unchanged |
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
   and `cameraChanged()`, nothing per frame.
2. **`orbitBy()`, `panBy()`, `zoomBy()`** (§6).
3. **Entries in a viewport's menus from a plugin:** `Host::addViewportMenuAction()` and
   `usdcc.app.add_viewport_menu_action()`, like the Outliner's prim menu
   ([PLUGINS.md](PLUGINS.md) §1). *Checked:* there is no such extension point.
4. **A mark on a viewport from a plugin** (the live mark), if question 4 says the HUD.
5. **Build:** `qtwebsockets` and `qthttpserver` in `vcpkg.json`; `Network`, `WebSockets` and
   `HttpServer` in `find_package(Qt6 …)`; their DLLs deployed to `bin/`, where the plugin, Peers
   and MCP all find them. New ports need a private vcpkg root in a worktree
   ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §12).

The plugin (`usdcc_add_plugin(viewport_broadcast …)`): a room from `usdcc_peers` (§7) with the
page served beside it, the frame source, the side panel, the page in `web/`, and its settings
(size limit, quality, public address, ask before a watcher joins). The port is the room's, in
Settings > Peers.

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
| **0. Spike** | A throwaway plugin: the server, `grabFrame()` on a timer, a bare page | It answers: does Qt HttpServer pass upgrades to Qt WebSockets here; milliseconds to read back and compress 1920×1080; bytes per frame; how an orbit feels on the LAN and through a tunnel; what Windows Firewall asks |
| **1. Watch** | The room and channels in `usdcc_peers` (§7); §8 items 1, 3 and 5; the plugin with Broadcast, the link, the page, the watcher list, Stop | A browser on a second machine follows an orbit and an edit, a still viewport sends nothing, and Stop closes the port |
| **2. Chat** | Names, the chat on both sides, the side panel, toasts | A watcher and the host chat |
| **3. Navigate** | §8 item 2; ask, allow, deny, take back; the moves | A watcher orbits the host's viewport; the host takes it back with a click |
| **4. Beyond the LAN** | a: the **Public address** setting. b: the relay and encryption, with [PEERS.md](PEERS.md) phase 4 | a: a watcher on a phone network watches through a tunnel. b: the relay's log shows sizes only |
| **5. Sharper and wider** | H.264 where the page allows it; Render View panels as sources | A 1080p orbit over the internet stays under 5 Mbit/s; an RTX render is broadcast |
| **Later** | A watcher's pointer shown to all; drawing on the picture; several broadcasts at once | |

Phases 0 to 3 need no server and no new codec.

## 12. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Build it alone, after Peers, or with it? | **Decided:** with it. The room and channels are built once, as the first piece of `usdcc_peers`, and this is their first user. The rest of Peers follows in its own time (§7) |
| 2 | Links beyond the LAN in the first version? | Phase 4a only: the user's own tunnel. The relay comes with Peers's |
| 3 | Must the host approve each watcher? | No, the link is the invitation, as in Peers. A setting to ask |
| 4 | Where is the live mark: the HUD, or the panel's title? | The HUD, top left, in red: it is where the eye is, and it must not be missed |
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
  ([PEERS.md](PEERS.md) §10).
- **Voice and video,** as in Peers.
- **A public relay run by the project** ([PEERS.md](PEERS.md) §16 question 3).
- **Recording** a broadcast.
