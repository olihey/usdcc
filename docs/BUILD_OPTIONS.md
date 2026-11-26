# Build Options (plan)

One way to leave a feature out of a build. A studio that doesn't want a feature at all, not even
turned off, builds without it, and the build can show that the feature isn't there.

**Status:** planned 2026-11-22; **phases 1 and 2 built** the same day (§10), **phase 3
(Updates)** on 2026-11-23 (§11). Remote Support, the plan's first user, doesn't exist yet, so
Viewport Broadcast was the first. Phase 4 (Peers) is **planned** (§12, 2026-11-23): its
three steps are still to build. "Checked" below means looked at in this repository; everything
else is an assumption until it's tried.

## 1. What there is today

*Checked* in [CMakeLists.txt](../CMakeLists.txt) and [tools/deploy.py](../tools/deploy.py):

| What | Kind | Default | Notes |
|---|---|---|---|
| `USDCC_BUILD_RTX_PLUGIN` | `option()` | Off | Adds `src/plugins/rtx_render_native`. `deploy.py --omit-rtx` leaves it out of a deploy |
| `USDCC_BUILD_TESTS` | `option()` | On | Tests, not a feature |
| `USDCC_USE_PCH` | `option()` | On | Build speed, not a feature |
| `USDCC_RTX_GPU_TESTS` | `option()` | Off | Needs `USDCC_BUILD_RTX_PLUGIN`, checked with a `FATAL_ERROR` |
| `USDCC_BUILD_BLENDER_MESH_TOOLS` | Cache variable, **forced on** | On | Always built. `deploy.py --with-blender-mesh-tools` makes the GPL artifact ([BLENDER_MESH_TOOLS.md](BLENDER_MESH_TOOLS.md)) |
| `USDCC_VELOPACK_FOUND`, `USDCC_PYTHON_FOUND` | Found, not chosen | | Updates and Python follow what is installed |
| Viewport Broadcast, Jolt Physics, `http`, `webdav`, `s3` | Always built | | `deploy.py`'s `SHIPPED_NATIVE_PLUGINS`, a fixed list |
| V-Ray Render | A Python plugin, always deployed | | `deploy.py`'s `SHIPPED_PLUGINS` |

So there are options, but each is its own: names differ, `deploy.py` keeps its own lists and flags,
nothing stops a plugin left out of the build from being copied in, and the app can't say what it
was built with.

## 2. Decisions

- **One option per feature,** named `USDCC_BUILD_<FEATURE>`, as the existing ones are.
- **Declared in one place,** `cmake/UsdccFeatures.cmake`, with one call per feature:

  ```cmake
  usdcc_feature(REMOTE_SUPPORT
      DEFAULT ON
      DOC "Remote support: show usdcc's windows to a helper (docs/REMOTE_SUPPORT.md)"
      PLUGINS remote_support
      NEEDS PEERS)
  ```

  It defines the option, keeps the list of features, and checks `NEEDS`.
- **Off means not there, not hidden:**
  1. Its directory isn't added, so nothing of it is compiled or linked.
  2. `deploy.py` doesn't copy it (§4).
  3. The app refuses to load its plugin ids from any folder (§3), so a copy from another build
     doesn't bring it back.
  4. About usdcc lists it as left out (§5).
- **A feature that needs another** (`NEEDS`) defaults to that one's value
  (`cmake_dependent_option`): with Peers off, Remote Support and Viewport Broadcast are off too.
  Asking for a feature whose base is off fails the configure with a message, as
  `USDCC_RTX_GPU_TESTS` does today.
- **Existing names stay.** `USDCC_BUILD_RTX_PLUGIN` keeps working as an alias of the new
  `USDCC_BUILD_RTX_RENDER` for a while, with a deprecation message.
- **Not a security boundary on its own.** Whoever has the source can build anything. The option
  gives a studio a build that provably lacks the feature; it doesn't stop someone from making
  another build.

## 3. The app knows its features

- `usdcc_feature()` writes a generated header, `usdcc/core/build_features.h`, with one entry per
  feature: its name, on or off, and its plugin ids. `usdcc_core` exposes it as
  `usdcc::core::buildFeatures()`.
- **The plugin manager checks it** before it reads a plugin's metadata
  ([PLUGINS.md](PLUGINS.md) §3.10 has it check every enabled plugin before running any of its code). A plugin whose id
  belongs to a feature that is off isn't loaded. The Plugin Manager lists it as "Left out of this
  build", and the log says so. This covers C++ and Python plugins alike.
- **Code inside the libraries** (Peers, updates; §6) can test `USDCC_FEATURE_<NAME>`, a compile
  definition of `usdcc_core`'s users, where a plugin boundary doesn't exist yet.

## 4. Deploy

- The configure writes `build/features.json`: each feature, on or off, and its plugins.
- `deploy.py` reads it instead of `SHIPPED_PLUGINS` and `SHIPPED_NATIVE_PLUGINS`. A feature that's
  off isn't copied, and isn't in `deploy.py`'s `required` list, the files a deploy must have
  (*checked*).
- `--omit-rtx` stays and means the same as building without RTX Render.
- The GPL question of Blender mesh tools stays `deploy.py`'s: which artifact it is, not whether the
  plugin is built. The forced `ON` becomes a normal option, still on by default.

## 5. Seeing what a build has

- **About usdcc** gets a **Features** section: each feature with "Included" or "Left out of this
  build". **Copy** in the dialog copies it with the rest.
- **`usdcc_node --version`** prints the same, for builds with no screen ([RENDER_NODE.md](RENDER_NODE.md)).
- **`usdcc.app.build_features()`** in Python, for a studio's checks.

## 6. Candidates

| Feature | Option | Kind | Default | Why a studio may want it out | Effort |
|---|---|---|---|---|---|
| Remote support | `USDCC_BUILD_REMOTE_SUPPORT` | Plugin | **On** (decided 2026-11-22) | Someone else controlling the app | Small: new, built for it |
| Viewport Broadcast | `USDCC_BUILD_VIEWPORT_BROADCAST` | Plugin | On | Opens a port and serves a page | Small |
| UV View | `USDCC_BUILD_UV_VIEW` | Plugin | On | Hardly; the Primvars panel's UV tab ([PRIMVAR_VIEWER.md](PRIMVAR_VIEWER.md) §18) | Small: new, built for it |
| Remote asset protocols | `USDCC_BUILD_HTTP`, `_WEBDAV`, `_S3` | USD plugins | On | Assets only from the studio's storage | Small |
| Jolt Physics | `USDCC_BUILD_PHYSICS_JOLT` | Plugin | On | Hardly; for completeness | Small |
| V-Ray Render | `USDCC_BUILD_VRAY_RENDER` | Python plugin, deploy only | On | Not licensed | Small: deploy and the id |
| RTX Render | `USDCC_BUILD_RTX_RENDER` | Plugin | Off (as today) | | Rename with an alias |
| Blender mesh tools | `USDCC_BUILD_BLENDER_MESH_TOOLS` | Plugin | On | GPL | Small: stop forcing it |
| Updates | `USDCC_BUILD_UPDATES` | In the app (`app_updates.cpp`) | On (without Velopack it only says that there is a newer release) | The studio deploys versions itself, and nothing should ask the internet | Medium: compile definition in `usdcc_app` |
| Peers | `USDCC_BUILD_PEERS` | A library (`usdcc_peers`), linked by `usdcc_usd`, `usdcc_ui`, `usdcc_app` and `usdcc_node` (*checked*) | On | A build that never opens a connection | **Large:** the shared stage, rendering with peers, the render queue and the Peers panel all build on it. Its own plan when it's wanted |
| Editor link | `USDCC_BUILD_EDITOR_LINK` | In a library (`usdcc_scripting_app`) | On | Other programs running scripts in the app ([VSCODE.md](VSCODE.md)) | Done with its first phase |
| MCP | to come with it | | | | Has an option from its first phase |

Not candidates: Python scripting and the plugin SDK. Plugins, settings and much of the UI rely on
them.

## 7. Phases

| Phase | What | Done when |
|---|---|---|
| **1. The mechanism** | `usdcc_feature()`, the generated header and `features.json`, the plugin manager's refusal, About's Features section. Remote support is its first user ([REMOTE_SUPPORT.md](REMOTE_SUPPORT.md) phase 1) | A build with `USDCC_BUILD_REMOTE_SUPPORT=OFF` has no plugin, refuses a copied one, and About says "Left out of this build" |
| **2. The plugins** | Viewport Broadcast, `http`, `webdav`, `s3`, Jolt, V-Ray, RTX (with the alias), Blender mesh tools; `deploy.py` reads `features.json` | Each one off builds, deploys and starts; CI builds once with all of them off, after a merge (§8) |
| **3. Updates** *(done, §11)* | `USDCC_BUILD_UPDATES` | Off: no update check, no Help > Check for Updates |
| **4. Peers** *(planned, §12)* | A plan of its own first: §12, steps 4a–4c | §12.5 |

## 8. Testing

- **A CI step with every optional feature off,** so a feature that's off keeps compiling and
  starting. It runs on every platform after a merge into `master`, not for pull requests, and
  doesn't deploy.
- **Unit tests** for the refusal: a plugin folder with a refused id isn't loaded and is listed as
  left out.
- Tests of a feature that's off are not built (they live with the feature's tests).

## 9. Open questions

1. **Where About's Features section goes:** its own tab, or rows under the components.
   *Decided: rows under the components* (§10).
2. **Whether a left-out feature shows in Settings at all.** Its settings don't exist; should
   Settings say "Remote support: left out of this build" where its group would be?
   *For now it doesn't:* nothing in Settings mentions a left-out feature.

## 10. Phases 1 and 2 as built

| Piece | Where | Notes |
|---|---|---|
| `usdcc_feature()`, `usdcc_write_features()` | [cmake/modules/UsdccFeatures.cmake](../cmake/modules/UsdccFeatures.cmake) | Takes `LABEL` and `FOLDERS` (the plugin folders `deploy.py` copies) besides §2's arguments. `NEEDS` defaults a feature to its base's value and fails the configure when it's asked for with its base off; no feature uses it yet |
| The features | Top-level [CMakeLists.txt](../CMakeLists.txt), before `usdcc_core` | Viewport Broadcast, HTTP, WebDAV, S3, Jolt Physics, V-Ray Render, Blender mesh tools (default on, no longer forced), RTX Render (default off). Each plugin's `add_subdirectory()` only when its feature is on |
| `USDCC_BUILD_RTX_PLUGIN` | Same | Still sets `USDCC_BUILD_RTX_RENDER` when it's given or changes, with a deprecation note. `tools/build.py` and `rtx-gpu.yml` use the new name |
| What the build knows | `usdcc::core::buildFeatures()`, `featureOfPlugin()`, `buildFeaturesText()` ([build_features.h](../src/cpp/core/include/usdcc/core/build_features.h)) | From `build/generated/build_features.cpp`, which the configure writes. `USDCC_FEATURE_<NAME>=0/1` is a public compile definition of `usdcc_core` |
| The refusal | `PluginManager::readManifests()` | A plugin whose id belongs to a feature that's off is `Refused`, "Left out of this build.", before its USD folders are registered or its code runs. What requires it is refused as any dependent is. `whyNotReloadable()` says the same |
| About usdcc | [about_dialog.cpp](../src/cpp/ui/src/about_dialog.cpp) | A Features heading under the components, "Included" or "Left out of this build" per feature; Copy has them as `Features:` lines |
| `usdcc_node --version` | [node/main.cpp](../src/cpp/node/main.cpp) | The version and one line per feature, then exits |
| Python | `usdcc.app.build_features()` | `(name, label, included, plugin ids)` per feature |
| `build/features.json` | The configure | Name, label, included, plugin ids and folders per feature |
| Deploy | [tools/deploy.py](../tools/deploy.py) | `SHIPPED_PLUGINS` and `SHIPPED_NATIVE_PLUGINS` stay as the candidates; those whose feature is off aren't copied or required, on Windows, Linux and macOS. They're still kept out of `examples/`, so a left-out plugin doesn't come back as an example. RTX follows its feature, and `--omit-rtx` still leaves it out |
| `tools/build.py --without FEATURE` | [tools/build.py](../tools/build.py) | Repeatable, or `--without all`; every run passes each default-on feature as on or off, so a build without `--without` has them all again. Without Blender mesh tools it doesn't fetch their backend |
| Tests | `tests/CMakeLists.txt` | A feature's tests are built only with it: the physics tests, `physics_timeline_test`, `viewport_broadcast_test`, `uv_layout_test` and `uv_view_test`, the V-Ray tests, and each protocol's part of `assets_net_test.cpp` (`#ifdef` on its directory). `python_physics` checks `USDCC_FEATURE_PHYSICS_JOLT` |
| CI | `build.yml`, Windows and Linux, x64 and arm64 | On pushes to `master` only, not for pull requests: after the usual build and tests, the same build with `--without all --without-rtx`. [check_features_left_out.py](../tools/ci/check_features_left_out.py) then fails if `features.json` has a feature included, or `usdcc_node --version` doesn't list each as left out. Not deployed, its tests not run |

Tests: `plugin_left_out_of_the_build` (`usdcc_python_tests`: the features, a copied plugin of a
left-out feature refused with its dependent, its code never run, no reload), `about_dialog`
(the rows and the text), `python_app` (`build_features()`).

## 11. Phase 3 as built: Updates

`USDCC_BUILD_UPDATES` (default on; `tools/build.py --without updates`) is a feature with no
plugin: it's in the app.

| Off means | Where |
|---|---|
| `app_updates.cpp` isn't compiled and usdcc doesn't link Velopack's DLL | [src/cpp/app/CMakeLists.txt](../src/cpp/app/CMakeLists.txt) |
| No Help > Check for Updates, no check a few seconds after the start, no "Updated to" note, no Settings > Updates | `#if USDCC_FEATURE_UPDATES` in [main.cpp](../src/cpp/app/main.cpp) |
| `deploy.py` doesn't copy or require `bin/velopack_libc.dll` (not even one an earlier build left in the build folder) | [tools/deploy.py](../tools/deploy.py) |
| About usdcc and `usdcc_node --version` say "Updates: Left out of this build" | §10's mechanism |

- **The launcher and the installer stay.** The launcher (`usdcc.exe` at the top of a deploy) links
  Velopack for the installer's hooks, which never go online. A studio that installs such a build
  with the installer gets an install that doesn't update itself; one that deploys its own way
  uses the 7z or `deploy.py` as before.
- **Not the same as `USDCC_VELOPACK=OFF`**, which leaves Velopack out altogether (no launcher, no
  installer), but keeps Help > Check for Updates, which then only says that there is a newer release.
- **CI's build with every feature left out** (`--without all`, after a merge) leaves out Updates
  too, as it's in `build.py`'s list.

Tests: `plugin_left_out_of_the_build` (Updates is a feature with no plugin ids, included as
configured). A build without it is CI's, as for §10.

## 12. Phase 4: Peers (plan)

`USDCC_BUILD_PEERS`: a build that never opens a connection to another program. Everything below
"checked" was looked at in this repository on 2026-11-23.

### 12.1 What rests on usdcc_peers (*checked*)

`usdcc_peers` (6,300 lines; Qt Network, WebSockets and HttpServer, libsodium, mdns) is linked
publicly by `usdcc_render`, `usdcc_ui`, `usdcc_app_core` and `usdcc_node_core`, so everything
above them sees its headers.

| Where | What uses it | Without peers |
|---|---|---|
| `usdcc_usd` `share/` | Shared stages, `stage_sync`, presence, the Shared Stages section ([SHARED_STAGE.md](SHARED_STAGE.md)) | Gone: a stage is only ever local |
| `usdcc_render` | `render_network` (rendering with peers), the swarm panel | Gone |
| `usdcc_render` | `render_queue`: `addRoom()`, and `Peer` in `cancel`/`retry`/`remove` (who asked) | **Stays**, local only: the queue runs this machine's jobs ([RENDER_NODE.md](RENDER_NODE.md)) |
| `usdcc_render` | Render Settings: the room list of "render with" | The local renderer only |
| `usdcc_ui` | `PeersController`, the Peers panel, the rooms debug dialog | Gone, with Settings > Peers |
| `usdcc_app_core` | `peersOptions()` (mDNS discovery) | Gone |
| `usdcc` (`main.cpp`) | The Peers controller, the render network's trust and rooms, render queues joining rooms | Gone |
| `usdcc_node` | `serve` and `queue` (a node and its queue on the network), `queue_web` | `--version`, `render` stay; `serve`/`queue` say the build has no peers |
| Viewport Broadcast | A room with guests | Left out: `NEEDS PEERS` |
| Python | `usdcc.peers` and plugins that use rooms (the plugin manager knows the module) | No module; such plugins refused as missing a requirement |

### 12.2 Decision: leave the library out, not a stub

Two ways were weighed:

1. **A stub `usdcc_peers`**: the same headers, every host or join failing with "Left out of this
   build", no network code linked. Small for the callers, but the UI would still offer rooms that
   can't open, and "not there" would be a stub that is. **Not taken.**
2. **No `usdcc_peers` at all** (taken): `src/cpp/peers` isn't added and nothing links Qt
   WebSockets, HttpServer, libsodium or mdns. The code that uses rooms is split from the code
   that doesn't, and compiled only with the feature. This is §2's rule ("off means not there"),
   and it is what a studio auditing the build can check: no socket library in the deploy.

`render_queue` is the one piece that must stay without peers, so its rooms move out of it (4a).

### 12.3 Steps, one PR each

| Step | Work | Done when |
|---|---|---|
| **4a. Split the render queue** | `RenderQueue` keeps the jobs and runs them; a `RenderQueueRooms` (new, in `render/`) does `addRoom()`/`removeRoom()`/`link()` and turns a peer's request into `cancel`/`retry`/`remove` with a requester that is a plain struct (id, name, admin), not `peers::Peer`. Render Settings' "render with" asks the render network through a small interface, empty without it | `render_queue_test` passes with no room; the room tests move to the new class; nothing in `RenderQueue`'s header names `usdcc::peers` |
| **4b. The option** | `usdcc_feature(PEERS ...)` before the others; Viewport Broadcast `NEEDS PEERS`. Off: `src/cpp/peers` not added; `share/*`, `render_network`, the swarm panel, `RenderQueueRooms`, `peers_controller`, the Peers panel and debug dialog, `queue_web` not compiled; `#if USDCC_FEATURE_PEERS` around their uses in `main.cpp`, `startup`, the node's `serve`/`queue` and the Stages/Window menus; vcpkg's `libsodium` and `mdns` only with it (a manifest feature); `deploy.py` doesn't copy or require `usdcc_peers.dll`, `Qt6WebSockets.dll`, `Qt6HttpServer.dll` or libsodium; `usdcc.peers` not built | A build with `--without peers` compiles, starts, opens and renders a stage, and has none of those DLLs; About says "Peers: Left out of this build" and "Viewport Broadcast: Left out of this build" |
| **4c. CI and tests** | `PEERS` joins `build.py`'s `OPTIONAL_FEATURES`, so CI's `--without all` (after a merge, §8) leaves it out too and its check lists it; the peer tests (`usdcc_peers_tests`, `shared_stages_test`, `render_network_test`, `peers_panel_test`, the node's `serve` tests) built only with the feature | CI green with it on (pull request) and off (after a merge) |

### 12.4 Settings, menus and Python with it off

- The Peers panel, Settings > Peers, the menu entries that share a stage, the swarm panel and
  the render network's entries aren't there (not greyed out), as §9's second question settled for other features.
- A session or layout that names the Peers panel must open without it, as one naming a left-out
  plugin's panel does (*to check* in 4b).
- `usdcc.peers` isn't importable; a plugin that requires it is refused with "Peers: left out of
  this build" (the plugin manager's existing requirement check, given the feature's name).

### 12.5 Tests

- The split (4a): the queue's own tests with no room; the rooms' tests as before.
- With the feature off (CI only, as for §10): it builds; `usdcc_node --version` lists Peers left
  out; a deploy has no `usdcc_peers.dll`, `Qt6WebSockets.dll`, `Qt6HttpServer.dll` or libsodium.
- Optional, later: a check that a started usdcc without peers has no listening socket
  (`netstat` from the CI step).

### 12.6 Open questions

1. **The default.** On, as now. Off would be the safer default for studios, but would hide
   shared stages and the render network from everyone else. *Recommended: on.*
2. **mDNS alone.** A studio may want rooms by address but no multicast on its network: a
   separate `USDCC_BUILD_PEERS_DISCOVERY` resting on Peers. *Not now*; Settings > Peers' "Look for rooms
   on this network" already turns it off at run time.
