# Render Node

usdcc on a machine with nobody at it: no window, no screen, started by a service or a farm
manager. It also checks a stage, with `usdcc_node validate` ([VALIDATION.md](VALIDATION.md) §17).
It renders final frames ([FINAL_RENDER.md](FINAL_RENDER.md)) in one of two ways:

- **One render:** a stage file and a frame range on the command line, done, exit. A farm manager
  (Deadline, OpenCue, Tractor, Flamenco, a script) runs it per frame or per chunk, as it would run
  `husk`.
- **A node for peers:** it joins rooms ([PEERS.md](PEERS.md)) and takes the frames and tiles that
  artists' instances hand out, like an instance with **Render for peers** on (FINAL_RENDER.md §7).
- **A queue server:** it takes jobs from artists, keeps them when the artist's instance closes,
  hands their tasks to nodes and writes the frames (§4.4).

**Status:** planned 2026-11-21; the queue server added the same day. Phases 1 to 4 built 2026-11-21
(§7): the shared startup, `usdcc_render`, `usdcc_node render`, `renderers` and `serve`, and the
OpenGL check for Storm. Phases 6 to 9 built 2026-11-22: the queue server, its state, the app's
side and the web page (§4.4, §4.5, "As built" there). Not built: the service (phase 5). "Checked"
below means looked at in this repository; everything else is an assumption until it's tried.

## 1. Decisions

- **A program of its own, `usdcc_node`, beside `usdcc.exe`.** Not a `--headless` flag on the app:
  - `main.cpp` is the app's composition root, about 1,500 lines built around `MainWindow`:
    docking, sessions, workspaces, toasts, questions asked with a notification. A flag would mean
    `if (!headless)` all through it.
  - A node wants other things: a console (log to stdout, an exit code, Ctrl+C), `QGuiApplication`
    rather than `QApplication`, no updates, no session, no layout, no startup scripts unless asked.
  - It ships in the same folder and uses the same DLLs, plugins and Python, so the package hardly
    changes.
- **What both programs do to start is shared,** moved out of `main.cpp` into one function (§3.1).
  The two can't drift apart on profiles, settings, resolvers or plugins.
- **The render code without widgets becomes a library of its own** (§3.2), so the node doesn't
  need `usdcc_usd_ui`.
- **Both modes use the queue the app uses** (`RenderScheduler`). A node's results are the same
  files, with the same names, as the app's.
- **Nobody answers questions.** Where the app asks (a job from a peer it doesn't trust), the node
  says no. What it may do is set before it starts: arguments or a config file.
- **No new protocol for a node.** A peer node speaks FINAL_RENDER.md §7.2 as an app instance
  does; the submitter can't tell them apart, except by the name.
- **The queue server is a mode of `usdcc_node` too** (`usdcc_node queue`), not a program of its
  own and not an option of `serve`:
  - An app instance already is a queue: `RenderScheduler` with its workers, a `PeerWorker` for
    each node and a `LocalWorker` for itself. A queue server is that, in a process that stays up
    and takes jobs from others. The code is `usdcc_render`'s; a third executable would repeat
    §3.1 and link the same libraries.
  - It isn't an option of `serve` because the two are opposite ends of FINAL_RENDER.md §7.2: a
    node takes tasks, a queue hands them out. They're set up and deployed differently: a node on
    every farm machine, a queue once per studio or team. `queue --render` does both in one
    process, for a team with one big machine.
  - Not in the Python hub (PEERS.md §9): the hub has no USD and no Qt, so the scheduler, the
    tiles' stitching and the EXR writing would be written twice. The queue joins the hub's room
    as a member, or hosts a room of its own.

## 2. What's already there

*Checked* in this repository:

| Part | Where | Without a window? |
|---|---|---|
| Jobs, tasks, tiles, output names | `render_job`, `render_spec`, `render_scheduler` | Yes: Qt Core, USD, OpenEXR, OCIO |
| Hydra renderers | `hydra_final_renderer` | Yes. Storm needs `QOffscreenSurface` and `QOpenGLContext` (Qt Gui); every other delegate runs with `gpuEnabled = false` and needs no OpenGL |
| Renderers that read a USD file | `scene_file_renderer` | Yes |
| Rendering for peers | `render_network`, `render_bundle` | Yes: Qt Core and `usdcc_peers`. Questions go through `RenderNetworkOptions::ask`, a callback `main.cpp` fills |
| The menu, Render Settings, Render Queue | `render_menu`, `render_settings_view_panel`, `render_queue_panel` | No: widgets. The node doesn't need them |
| Plugins adding a renderer | `Host::addFinalRenderer()` | Yes. `Host::require()` throws "the app has no main window here" when a plugin adds a panel or a menu entry to a host without one |

- **All of it is compiled into `usdcc_usd_ui`,** which links Qt Widgets, OpenGL, Svg and
  QtNodes (§3.2).
- **The node's side is already tested without a UI:** `render_network_test.cpp` runs a submitter
  and a node in one process.

## 3. The code

### 3.1 Starting up, shared

`usdcc_app_core`, a static library in `src/cpp/app/` (`startup.h`), with what `main.cpp` does
before it makes the window:

- the test profile (`USDCC_TEST_PROFILE`) and the application name;
- settings, the log and its file;
- USD's plugin paths, resolvers, color management;
- `registerHydraFinalRenderers()`;
- Python (`PythonRuntime`) and the plugins, with `--no-plugins`, `--plugin-path`;
- the `Peers` object, without its panel.

Each step is a function. `main.cpp` calls them, then builds the window as it does now.

*As built:* `configureLogLevels()`, `setDefaultSurfaceFormat()`, `applyTestProfile()`,
`openLogFile()`, `installSettings()` (usdcc.ini, Settings > USD's environment, the OCIO config,
the Python and plugins settings), `pluginFolders()`, `configurePluginUsd()` (the schema
libraries, Settings > Assets), `registerRenderers()` and `peersOptions()`. The theme, sessions,
updates and the viewports' settings stay in `main.cpp`: the node has none of them.

### 3.2 The render library

`usdcc_render` (shared): the files of §2 that need no widgets, moved out of `usdcc_usd_ui`.

- **Links:** `usdcc_usd`, `usdcc_peers`, Qt Core and Gui (Storm's offscreen context).
- **Headers stay where they are** (`usdcc/usd/render/...`), so plugins and includes don't
  change.
- **`usdcc_usd_ui` links it** and keeps the menu and the two panels.
- **The plugin SDK** links it too, for `Host::addFinalRenderer()`.
- **A helper from `usdcc_usd_ui`** that the moved files use, if any, moves with them or behind
  an interface. *As built:* `RenderCamera` and `primRenderCamera()` moved out of
  `render_view_panel.h` into `usdcc/usd/render_camera.h`, in `usdcc_render`; the panel keeps
  `viewportRenderCamera()` and `cameraPrims()`. The moved headers export with
  `USDCC_RENDER_API`.
- **The node still loads `usdcc_usd_ui`,** through the plugin manager (`usdcc_scripting_app`),
  which plugins' renderers need. It makes no widget: plugins get a `Host` without a window, and
  view panel types go into a registry that never makes one. Splitting the plugin manager from
  the app's UI is for later.

### 3.3 The executable

`src/cpp/node/`, `usdcc_node.exe`:

- **A console program** (no `WIN32` subsystem), so its log reaches whoever started it.
- **`QGuiApplication`,** for Storm's OpenGL. With `--no-gpu`, `QCoreApplication`, and Storm
  isn't offered (§5).
- **Its own `Host`** for plugins: no main window, no view panels, no tool set, no undo. A plugin
  that adds only a renderer works; one that adds a panel throws, which the plugin manager logs.
  The plugin's renderer is all the node wants from it.
- **Ctrl+C, or the service stopping:** a peer node tells its submitters, which put its tasks back
  in their queues (FINAL_RENDER.md §7.1 already does this for a node that leaves), and exits.
  One render cancels, and exits with 130. A queue server saves its state, tells its nodes to
  stop, and exits; it goes on where it was at the next start (§4.4).

## 4. The modes

### 4.1 One render

```
usdcc_node render shot.usda [--settings /Render/Settings] [--frames 1-24[x2]]
                            [--renderer HdEmbreeRendererPlugin] [--camera /cams/main]
                            [--resolution 1920x1080] [--output "renders/<stage>.####.exr"]
                            [--format exr|png] [--tiles 2x2] [--time-limit 600]
                            [--set embree:ty:maxBounces=8]... [--asset-env prod]
```

- **The stage opens from its file,** with its resolver context. No session layer, no unsaved
  edits: what's on disk is what renders. Its asset environment is `--asset-env`, else
  `USDCC_ASSET_ENV`, else the configuration's ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §4).
- **The settings** are the stage's RenderSettings prim, as in the app (FINAL_RENDER.md §2.1).
  Each argument overrides its attribute on the job's snapshot only, never in the file.
- **No camera anywhere** is an error: there's no viewport to borrow one from.
- **The job goes to a `RenderScheduler` with only the local worker.** The same tiles, retries,
  output names and files as the app.
- **Progress** is a line per task on stdout. `--json` makes each a JSON line for a farm manager
  to read.
- **Exit codes:** 0 every frame written; 1 the job failed (the reason on stderr); 2 bad
  arguments, or the stage, settings or renderer can't be used; 130 cancelled.
- *As built:* the overrides go into the stage's session layer, which the job's snapshot flattens
  in and nothing saves; settings without a RenderSettings prim get one there, aimed at the
  stage's first camera, and settings that name no camera get the stage's first. A relative
  `--output` is under the working folder. `--json` lines: `job`, `task`, `file`, `end`.
- **`usdcc_node renderers`** lists the renderers and why one can't render here
  (`setupProblem()`), so a farm can check a machine before it sends work.

### 4.2 A node for peers

```
usdcc_node serve --join "usdcc://join/...#k=...&s=..." [--join ...]
                 [--public] [--trust abcd-efgh]... [--trust-file trusted.txt]
                 [--key-file node.key] [--name "Farm 07"] [--renderers id,id]
                 [--inbox D:/usdcc-inbox] [--inbox-limit 200GB] [--config node.json]
```

- **Which rooms:**
  - `--join`: an invite link, joined at start and again after a lost connection. This is
    PEERS.md §5.3's "rooms joined at start", which isn't built yet; the node needs it first.
  - `--public`: every public room found on this machine and network (PEERS.md §14.3), joined as
    they appear. For a team that has one public room and no hub.
- **Who it renders for:** only fingerprints in `--trust` or `--trust-file`. Others are declined
  (`decline {job, reason: "not trusted"}`), and their tasks go back to the submitter without
  counting as a try. The file is read again when it changes.
- **Identity:** `--key-file`, a key pair in a file, as the Python peers have (PEERS.md §9). A
  service account then needs no credential store, and a machine image can carry its key. Without
  it, the profile's key as in the app. The fingerprint is printed at start, to put in the
  artists' trust lists if they keep one.
- **What it offers:** every renderer that can render here, or those `--renderers` names. It says
  `node: true` in `org.usdcc.render/node` from the start; there's no setting to turn on.
- **The inbox:** bundles land in `--inbox`, by hash, as now (PEERS.md §7.3). A node keeps them
  across jobs, so a shot's textures arrive once. Over `--inbox-limit` the oldest bundles not in
  use are removed. (The inbox's limit is in PEERS.md §11's list of settings, not built.)
- **`--config node.json`** holds the same as the arguments, for a service. Arguments win.
  *Not built yet:* with the service (phase 5). Nor is `--inbox-limit`.
- *As built:* the key file is a secret key's 64 bytes, as `identity.key`; it's made, readable
  by its owner only, when it doesn't exist. `--trust` takes a whole fingerprint or its first
  eight (`abcd-efgh`). Ctrl+C or the console closing closes the rooms, so submitters put the
  node's tasks back at once; a node that's killed is noticed when the room gives up on it
  (PEERS.md's reconnect grace, 30 s).

### 4.4 A queue server

```
usdcc_node queue [--host-room "Render" --port 47900 | --join "usdcc://join/..."]
                 [--accept abcd-efgh]... [--accept-file submitters.txt] [--admin abcd-efgh]...
                 [--key-file queue.key]
                 [--state D:/usdcc-queue] [--output-root //server/renders]
                 [--render] [--name "Studio queue"] [--config queue.json]
```

What it adds to FINAL_RENDER.md §7 is that a job outlives the instance that submitted it.

| | With peers today | With a queue server |
|---|---|---|
| Who holds the queue | The artist's instance | The queue server. The artist's instance may close once the job is sent |
| The scene | Sent to each node | Sent to the queue once; nodes get it from the queue, whose inbox keeps it by hash |
| The frames | Sent back to the submitter, which writes them | The queue writes them (below) |
| A restart | The queue is lost | Jobs and tasks are saved and go on |
| Several artists | A queue each, competing for the nodes | One order for everyone |
| Trust | Each node trusts each artist | Nodes trust the queue's fingerprint; the queue decides which artists it takes |

**The room.** `--host-room` hosts one (by invite link, or public on this network), so the queue
is what's always there. `--join` puts it in a room someone else hosts, such as a hub's. Nodes
(`serve`) and artists join the same room.

**Submitting.** In Render Settings > Output, **Render on** gets a fourth choice: *Queue*, with
the queues in the room listed, as the nodes are now.

- A new message on `org.usdcc.render/jobs`: `submit {job, renderer, title, frames, tasks,
  settings, output, shared?}` from the artist, then the bundle as a transfer (packaged), or the
  shared scene's paths (FINAL_RENDER.md §7.3). The queue answers `queued {job, position}` or
  `refused {job, reason}`.
- **A job sent to a queue is the queue's.** The artist's Render Queue shows it as *on Studio
  queue* and doesn't schedule it itself.
- **From Python:** `render.render_animation(..., queue="Studio queue")`. From a farm script or
  CI, a Python peer can submit with `usdcc_peers` once it can make a bundle; not in the first
  version.

**Handing out tasks.** The queue's `RenderScheduler` with a `PeerWorker` per node, exactly as in
an app instance. With `--render`, a `LocalWorker` too.

- **Order:** first come, first served, across artists, to start with. Priorities and a limit
  of nodes per artist later (§10).
- **A node that leaves:** its task goes back, as now.

**The frames.**

- **Written by the queue,** to `--output-root` joined with the job's output pattern
  (FINAL_RENDER.md §4.3). A relative pattern is under the root; an absolute one must be inside
  it, or the job is refused. The artist's own output folder means nothing on the queue's
  machine.
- **A shared path is the point:** everybody opens the frames from there. Without
  `--output-root` the frames stay in `--state`, and the artist's instance fetches them when it's
  next in the room (a transfer), then they're removed.
- **Nodes still send their images to the queue,** never write outputs themselves: the queue
  stitches tiles, as an instance does.

**Surviving a restart.** In `--state`:

- a JSON file per job: its description, its tasks' states and tries, and where its scene is;
- the scenes, in the inbox (PEERS.md §7.3), kept until their job ends;
- frames finished but not all tiles in, as the tiles' raw images.

At start the queue reads them back. Running tasks become waiting ones; finished frames stay
finished.

**Watching and controlling it.**

- The queue publishes its jobs in a shared map, `org.usdcc.render/queue` (PEERS.md §7.2): per
  job its title, submitter, state, frames done of all, and the nodes working on it. Small enough
  to stay well under a map's limits; a job's tasks are asked for when someone opens it.
- The app's Render Queue panel shows the queues in the room under their own heading, with the
  same columns.
- **Cancel, Retry, Remove** go to the queue as messages (`cancel`, `retry`, `remove`). An artist
  may do them to their own jobs; `--admin` fingerprints to anyone's.
- **A notification** to the submitter when a job ends or fails (PEERS.md §10.2), so the artist
  hears about it in whichever instance they have open.

**Trust.**

- **Artists:** `--accept` and `--accept-file` list the fingerprints the queue takes jobs from.
  Others get `refused`.
- **Nodes:** they `--trust` the queue's fingerprint only. A node trusting an artist directly
  still works, for the peers of FINAL_RENDER.md §7.
- **The queue never runs what it gets.** A scene is rendered by a node's renderer, as now; a
  script in a scene is as harmless or harmful as it is on the artist's own machine.

**As built** (`usdcc/usd/render/render_queue.h`, in `usdcc_render`; `usdcc_node queue`):

- **`RenderQueueServer`** takes jobs, **`RenderQueueClient`** is the artist's side. The app makes
  a client for its render queue rooms; the node's `queue` command makes the server, its own
  `RenderScheduler` and a `RenderNetwork` whose nodes are its workers.
- **Channels:** the messages are on their own Direct channel, `org.usdcc.render/queue-jobs`, and
  the scenes and fetched frames on `org.usdcc.render/queue-files`, not on
  `org.usdcc.render/jobs`, so a queue and the peers of FINAL_RENDER.md §7 don't read each
  other's messages. A queue says it is one in the per-peer state `org.usdcc.render/queuer`
  (`{name, root}`), which goes when it leaves; its jobs are in the map `org.usdcc.render/queue`,
  a key `<queue peer id>|<job id>` each, written only by the queue where it hosts the room.
- **A job travels whole:** `jobToJson()` with its spec and tasks, and the scene as a packaged
  bundle. The queue gives it an id of its own (`q…`), keeps the scene in `--state`, and sends
  `queued` once it has both; `refused` when the artist isn't accepted, the job has no frames, or
  its output leaves the root (`queueOutputPattern()`: a relative pattern under the root, an
  absolute one only inside it).
- **Its state:** a JSON file per job (`jobs/`), the scene (`scenes/`), a finished tile's images
  until its frame is written (`tiles/`), and without a root the frames (`frames/`). Saved as
  things change, so a queue that's killed loses at most half a second of statuses. At start:
  running tasks wait again; a frame with every tile done and none kept was written; a done tile
  kept goes back into its frame (`RenderScheduler::restoreTask()`); a done tile not kept renders
  again.
- **Fetching:** without `--output-root`, once a job is done and its artist is in a room, the
  queue sends the frames; the artist's instance puts them in the folder the job would have
  written to, says `received`, and the queue lets go of them.
- **In the app:** Render Settings > Render on lists *Queue: <name>* for each queue in the rooms
  (`PeerDispatch::queue`, kept between runs); Render Frame and Render Animation then send the job
  there instead of scheduling it. The Render Queue panel lists each queue's jobs under *On
  <name>*, with status, progress, nodes and time; Cancel, Retry and Remove go to the queue, which
  answers `denied` (a toast) when they aren't the artist's to do. Its tasks aren't listed there:
  the web page has them. A job's end is a toast in the artist's instance.
- **`--host-room <name>`** hosts on every address (`--local`: 127.0.0.1 only; `--public`: open
  to anyone on this network, announced), on `--port` or any free one, and prints its link.
  `--join` puts it in another room, joined again when lost. `--render` adds this machine as a
  worker. The queue prints its fingerprint, for the nodes' `--trust`.

### 4.5 The queue's web page

For whoever doesn't have usdcc open: a producer, a supervisor, an artist on a laptop, the farm's
admin.

```
usdcc_node queue ... --web 8080 [--web-token-file web.token]
```

**What it shows,** the Render Queue panel's content (FINAL_RENDER.md §5.2) and the queue's own:

- **The jobs:** title, submitter, renderer, state, frames done of all, the nodes on it, time.
  A job opens to its tasks, with their state, progress, node, tries and time, and the error of a
  failed one.
- **The preview:** the newest frame of the selected job, as a PNG through the view transform,
  made by the queue at most once a second and only while someone looks.
- **The nodes:** each with its renderers, busy or idle, the task it's on, and whether it's
  trusted the queue.
- **Live:** the page holds a WebSocket and gets the changes of `org.usdcc.render/queue` as they
  happen. No reloading.

**Who may do what.**

- **Looking:** the page's link is a guest's (PEERS.md §6.1): `http://<queue>:8080/#s=<secret>`.
  The queue prints it at start and puts it in Render Settings beside the queue's name.
- **Cancel, Retry, Remove, and pausing a node:** only with the token of `--web-token-file`,
  typed once and kept in the browser. A guest has no key pair, so the page can't say who it is;
  the token is the farm admin's. Each action is logged with the browser's address.
- **On a plain `http://` link nothing is encrypted** (PEERS.md §16 question 11). Behind an HTTPS
  reverse proxy for anything but a closed network.

**It looks like usdcc.** Same surfaces, text, accent, radii and density as the app's dark theme,
so the page reads as part of usdcc and not as a separate tool.

- **One set of tokens for both.** The colors, radii and sizes listed at the top of
  `src/cpp/ui/resources/theme/app.qss` (surfaces `#161618`, `#1a1a1c`, `#1c1c1e`, `#141416`,
  `#0e0e10`; hairlines at 6, 8 and 10 % white; text `#e8e8ea` and 75 / 50 / 40 %; 3 px and 4 px
  radii; 11 px type, 20 px controls; the font "Inter", "Segoe UI", sans-serif) become CSS custom
  properties in `src/cpp/ui/resources/theme/usdcc.css`:
  `--surface-bar`, `--surface-panel`, `--inset`, `--line`, `--text`, `--text-muted`, `--accent`,
  `--radius`, ... Kept in sync with `app.qss` by a test (§8), as `app.qss` is with `theme.cpp`
  by hand now.
- **The accent** is the queue profile's Appearance/accentColor, put into `--accent` (and
  `--accent-rgb`, `--accent-hover`) when the page is served, as `theme::resolveTokens()` does for
  the app. The status colors are the toasts': green `#4fd671`, amber `#ffb340`, red `#ff5f56`
  (NOTIFICATIONS.md).
- **The same widgets, drawn in CSS:** the table rows, selection fill (18 % of the accent), header,
  toolbar buttons, progress bars and tabs follow the Render Queue panel's look in the app. Where
  the app and the original design differ (the app can't letter-space or uppercase its section
  labels), the page follows the app.
- **The same icons:** the PNGs `tools/gen_theme_icons.py` makes for the app, served from the
  queue. No icon font, nothing from a CDN: the page works on a network with no internet.
- **Dense, with room for a browser:** 11 px type at 100 %, as in the app; browser zoom does what
  Settings > Appearance > UI scale does in the app. Down to a phone's width, where the job list
  becomes one column and the tasks open below it.
- **Dark only,** as the app.
- **The broadcast's page** ([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md)) has colors of its own
  (`#1e1f22`, `#2b2d30`, ...). It moves to `usdcc.css` too, so every page usdcc serves looks
  alike.

**The code.** Plain HTML, CSS and JavaScript in `src/cpp/node/web/`, no framework and no build
step, like the broadcast's page. Served by the queue's room, as `usdcc_peers` serves guests'
pages (`HostOptions::pageDirectory`, [PEERS.md](PEERS.md) §6.1).

**As built** (`src/cpp/node/queue_web.h`, `src/cpp/node/web/index.html`):

- **A room of its own** on the `--web` port, that only guests use, so the page works whether the
  queue hosts its render queue room or joins someone else's. It serves `node-web/` beside
  `usdcc_node.exe` (the page and `usdcc.css`, copied there by the build).
- **One channel,** `org.usdcc.render/queue-web`, open to guests: the page gets a `snapshot` when it
  joins and a `state` (every job with its tasks, and the nodes) a few times a second at most while
  anything changes, rather than the map's changes, as guests don't see maps (PEERS.md §6.1).
  `preview` carries the PNG.
- **The token** is checked on the queue for each `cancel`, `retry` and `remove`; the answer,
  `result`, goes to that page only. Without `--web-token-file` the page can only look. A file
  that doesn't exist is made, with 32 random hex characters, readable by its owner only. Each
  action is logged with the page's name; Qt's guests don't say their address, so it isn't logged.
  Pausing a node isn't built.
- **The accent** comes in the snapshot (the profile's Appearance/accentColor) and the page sets
  `--accent`, `--accent-rgb` and `--accent-hover` from it.
- **Not built:** the icons (the page uses text buttons, as the panel does), whether a node has
  turned the queue down (the node list shows renderers, busy or idle, and the task it's on), and
  the guest link in Render Settings: the queue prints it.
- **The broadcast's page** links the same `usdcc.css`; the plugin's build copies it beside the
  page.
- **Checked** in the in-app browser against sample jobs: the panel's surfaces, density, accent
  and hairlines, one column at a phone's width with no sideways scroll.

### 4.3 Profile and settings

- **Its own profile,** `usdcc-node` (`--profile name` for another), so a node on an artist's
  machine doesn't share `usdcc.ini`, sessions or peers identity with the app.
- **Settings > Renderers' values** (VIEWPORT_DISPLAY.md §7) and Settings > Final Render's come
  from that profile's `usdcc.ini`. A farm copies one in, or passes `--set`.

## 5. A machine without a screen

| Renderer | Needs | As a Windows service (session 0) | In a logged-in session, no monitor | Linux, no display |
|---|---|---|---|---|
| Embree, Arnold, other CPU delegates | Nothing | Yes | Yes | Yes |
| V-Ray | Its helper process and a licence | Yes, if the licence server is reached | Yes | Not built for Linux |
| RTX (ovrtx) | CUDA, Vulkan | Expected: no window involved. *Not tried* | Expected | Expected |
| Storm | An OpenGL 4.5 context on the GPU | **No**: session 0 gets Windows' software OpenGL 1.1 | Usually; some GPUs want a monitor or a dummy plug. *Not tried* | With EGL. *Not tried* |

- **Storm is only offered when it works.** At start the node makes a `QOpenGLContext` and reads
  its version and vendor. Below 4.5, or Microsoft's GDI renderer: Storm's `setupProblem()` says
  why, and it isn't advertised. `usdcc_node renderers` shows it. *As built:* `probeOpenGl()`,
  `stormOpenGlProblem()` and `setStormProblem()` in `hydra_final_renderer.h`; only the node sets
  the problem, so the app offers Storm as before. With `--no-gpu` there's no context at all.
- **Qt's `offscreen` platform plugin has no OpenGL on Windows.** The node uses `windows`, which
  needs no monitor, only a desktop. On Linux, `offscreen` or `eglfs` with EGL; to be chosen when
  Linux is tried.
- **For Storm on Windows:** a user that logs on by itself (auto-logon) and starts `usdcc_node`
  from a scheduled task "at log on". A service is for the other renderers.
- **NVIDIA cards in TCC mode** have no graphics API at all: CUDA only.

## 6. Running it

- **From each package** ([UPDATES.md](UPDATES.md) §9):

  | Package | The program | Path after an update |
  |---|---|---|
  | Windows installer | `%LOCALAPPDATA%\usdcc\current\bin\usdcc_node.exe` | The same |
  | Windows 7z | `<folder>\bin\usdcc_node.exe` | Updated by hand |
  | macOS | `/Applications/usdcc.app/Contents/MacOS/usdcc_node` | The same |
  | Linux tar.xz | `<folder>/bin/usdcc_node` | Updated by hand |
  | Linux AppImage | `./usdcc-linux-x64.AppImage usdcc_node`, or a link to it named `usdcc_node` | The same, if the file keeps its name |

  A farm is best served by the 7z or tar.xz in a folder named by version: its machines change
  version together, and nothing updates during a job. The AppImage without FUSE needs
  `--appimage-extract-and-run`, which unpacks all of usdcc at each start. On macOS a downloaded
  usdcc is opened once first, so that macOS lets it run.
- **No display on Linux:** `--no-gpu` starts without a window system, for renderers that need
  no OpenGL. Storm there is for later (§7).
- **Windows service:** `usdcc_node service install --config node.json` registers it (as Local
  Service, or an account given); `service remove`. The service is `usdcc_node serve` with the
  config. Its log goes to the profile's log folder and the Event Log for start, stop and errors.
  `service install --queue --config queue.json` does the same for a queue server, which needs
  no GPU and suits a service best.
- **Scheduled task:** for Storm (§5): a user that logs on by itself starts the node at log on,
  in its session, where Storm has the GPU. For example, as that user:

  ```
  schtasks /Create /TN "usdcc render node" /SC ONLOGON /RL LIMITED ^
           /TR "\"C:\Program Files\usdcc\bin\usdcc_node.exe\" serve --public --trust-file C:\farm\trusted.txt --key-file C:\farm\node.key"
  ```

  `usdcc_node renderers` in that session says whether Storm can render there. *Not tried* on a
  machine without a monitor.
- **Linux:** a systemd unit, when Linux is tried.
- **Docker:** not planned (§9).
- **Updates:** the node doesn't update itself. Velopack updates the folder when the app does,
  which on Windows and macOS stops a node running from it: usdcc asks first
  ([UPDATES.md](UPDATES.md) §9). A running service is to be stopped
  first (phase 5). The node doesn't check for updates on its own either: nobody is there, and
  farm managers read its output. `usdcc_node --check-update`, which prints the newest version
  and exits, could be added. On a farm machine without the app, the package is deployed
  as the farm deploys anything.

## 7. Phases

| Phase | Builds | Done when |
|---|---|---|
| **1. Split** *(built)* | §3.1 and §3.2: starting up shared, `usdcc_render` out of `usdcc_usd_ui` | The app starts, renders and renders with peers as before; every test passes |
| **2. One render** *(built)* | `usdcc_node` (§3.3) with `render` and `renderers` (§4.1) | A test runs `usdcc_node render` on a test stage with Embree, 3 frames in 2 × 2 tiles, and the EXRs match the app's; the exit codes for a missing stage, an unknown renderer and no camera (`node_process_render`) |
| **3. Node for peers** *(built)* | `serve` (§4.2): `--join`, `--public`, trust, `--key-file`, the inbox, Ctrl+C | A test like `render_network_packaged`, with the node as a process: an app instance in the test hands it frames, untrusted is declined, killing the node puts its task back (`node_process_serve`) |
| **4. Storm headless** *(built, not tried headless)* | The OpenGL check of §5, the scheduled task | On a machine with no monitor, Storm renders a frame in a logged-on session, and is not offered as a service |
| **5. Service** | `service install`/`remove`, the Event Log | A service on another machine takes frames from an artist's instance across the network |
| **6. Queue: submitting** *(built)* | `usdcc_node queue` (§4.4): its room, `submit`, `queued`/`refused`, the queue's scheduler handing tasks to nodes, the frames written under `--output-root`, *Queue* in Render on | A test: an artist's instance submits 3 frames in 2 × 2 tiles to a queue process and closes; two node processes render them; the EXRs under the root match the app's. An artist not accepted is refused (`node_process_queue`) |
| **7. Queue: restarts** *(built)* | `--state`: jobs, scenes and half-done frames saved and read back; frames fetched by the artist when there's no `--output-root` | The test kills the queue mid-job and starts it again: the job finishes, finished frames aren't rendered again (`node_process_queue_restart`; tiles: `render_queue_restart`) |
| **8. Queue: watching** *(built)* | The `org.usdcc.render/queue` map, the queues in the Render Queue panel, `cancel`/`retry`/`remove`, `--admin`, the notification when a job ends | A UI test: the panel lists a queue's job, Cancel from it cancels on the queue; another artist can't cancel it, an admin can (`render_queue_panel_and_permissions`) |
| **9. Queue: web page** *(built)* | §4.5: `usdcc.css` from the app's tokens, the page, `--web`, the token for actions; the broadcast's page moved to `usdcc.css` | A browser on the guest link lists a job and its tasks as they change; Cancel without the token is refused, with it the job cancels; side by side with the app's Render Queue panel, the same colors, type and spacing (screenshots of both in the PR) (`render_queue_web_page`, `theme_css_matches_dark_qss`) |
| **Later** | Linux and EGL, several tasks at once on a node with several GPUs, a node writing outputs to a shared path itself (FINAL_RENDER.md §7.4), priorities and per-artist limits on a queue, submitting from a Python peer | |

Phase 2 is useful on its own: with it, any farm manager renders usdcc's stages. The queue server
(phases 6–9) needs phase 3's nodes, and not phases 4 or 5.

## 8. Tests

- **`render_node_cli`** (a CTest of its own, runs the executable): §7 phase 2.
- **`render_node_peers`:** §7 phase 3, the app's side in-process, the node as a process on
  `127.0.0.1`, with a test profile each.
- **`render_queue_server`:** §7 phases 6 and 7, the artist in-process, the queue and two nodes
  as processes, each with a test profile; a queue killed and started again.
- **`render_queue_panel_remote`** (usdcc_ui_tests): §7 phase 8.
- **`web_theme_tokens`:** every token in `app.qss`'s header has its custom property in
  `usdcc.css` with the same value, and the served page has the profile's accent in it.
- **`render_queue_web`:** §7 phase 9 without a browser: the page and `usdcc.css` are served on
  the guest link only, the WebSocket gets a job's changes, an action without the token is refused.
- **CI** ([CI.md](CI.md)) has no GPU: Embree only. Storm and RTX are checked by hand.
- **Across a network boundary** (2026-11-22, Windows and WSL 2 on one PC, PEERS.md §14.3): a
  queue on Windows (`--host-room`, `--accept`), a node on **Linux** (`serve --join --trust`, built
  in WSL against Python 3.13's OpenUSD) and an artist's usdcc on Windows with Render on set to the
  queue. The job went artist → queue → Linux node → queue → artist: 3 Embree frames at 320×240
  in 1.3 s from submitting to the frames in the artist's folder. Found on the way:
  - `usdcc.render.jobs()` and `job()` didn't see a job sent to a queue: they listed the app's
    `RenderScheduler` only, so `render_animation()` returned an id that `job()` never found.
    *Fixed:* `RenderQueueClient` keeps what it sent (`sentJobs()`, by the artist's job id, with
    the queue's id once queued, the refusal, the queue's status and the fetched files), and
    `jobs()` lists those too, with `Job.queue`; `cancel()` reaches the queue once it has queued
    the job. Tested in `render_queue_fetch` and `render_queue_submit`.
  - Fetched frames go to the user's Pictures folder even under `USDCC_TEST_PROFILE`.
  - `usdcc_node queue` has no `--name`: every queue is "Render queue", which is also what an
    artist's Render on names it by. §4.4's synopsis shows a `--name`.
  - Every `usdcc_node` start logs an error from Mesh Tools ("the app has no main window here").
  - The queue's web page took 5 s to answer its first request from WSL.
  Still not tried: a second physical machine, the service (phase 5).

## 9. Not planned

- **A farm manager.** The queue server (§4.4) is for teams without one, and for rendering from
  the app with nothing else set up; its web page (§4.5) shows and controls that queue only.
  Dependencies between jobs, pools, users and roles, licence counting: that's what Deadline and the others do, and phase 2 lets them render usdcc's stages.
- **Driving a node with commands** beyond rendering. That's [MCP.md](MCP.md), for a running app.
- **Docker images.** Hydra delegates, OpenGL and licences make one image per studio; a studio
  builds its own from the package.
- **Running the full app without a window** (`--headless` on `usdcc.exe`): §1.

## 10. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | One program, or a flag on `usdcc.exe`? | One program (§1) |
| 2 | Its name? | `usdcc_node`. `usdcc_render` is the library |
| 3 | Do a node's plugins load all of them, or only the ones with renderers? | All, as in the app, with `--no-plugins` and `--plugin-path`. A plugin can't say yet what it adds before it's loaded |
| 4 | Does `--public` render for anyone in a public room? | No: trust still applies. A public room only finds the node |
| 5 | Should a node write outputs to a shared path instead of sending them back? | Later (FINAL_RENDER.md §7.4). Sending back works on any network |
| 6 | Several tasks at once on a big machine? | Later. Until then, one node per GPU, each with its own profile |
| 7 | The queue server: a program of its own, an option of `serve`, or in the Python hub? | A mode of `usdcc_node`, `queue` (§1) |
| 8 | Saved how: a JSON file per job, or SQLite? | A JSON file per job, written whole and renamed into place. Few jobs, read once at start; SQLite when priorities and history need queries |
| 9 | In what order does a queue serve several artists? | First come, first served to start with; priorities and a limit of nodes per artist when someone waits too long |
| 10 | Does an artist's instance still render a job it sent to a queue? | No: the job is the queue's. *This instance and peers* stays for rendering without a queue |
| 11 | Can there be several queues in one room? | Yes, each listed by name in Render on. A node renders for every queue it trusts, one task at a time |
| 12 | The web page's theme: copied tokens with a test, or `app.qss` and `usdcc.css` both generated from one tokens file? | Copied with a test, to start with: `app.qss` stays the file people edit, and only its header's tokens are shared. Generate both if a third consumer appears |
| 13 | Who may act from the web page? | One admin token to start with. Per-person logins only if a studio needs to know who cancelled what |
