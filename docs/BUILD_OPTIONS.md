# Build Options (plan)

One way to leave a feature out of a build. A studio that doesn't want a feature at all, not even
turned off, builds without it, and the build can show that the feature isn't there.

**Status:** planned 2026-11-22. Nothing is built. Its first user is
[REMOTE_SUPPORT.md](REMOTE_SUPPORT.md). "Checked" below means looked at in this repository;
everything else is an assumption until it's tried.

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
| Remote asset protocols | `USDCC_BUILD_HTTP`, `_WEBDAV`, `_S3` | USD plugins | On | Assets only from the studio's storage | Small |
| Jolt Physics | `USDCC_BUILD_PHYSICS_JOLT` | Plugin | On | Hardly; for completeness | Small |
| V-Ray Render | `USDCC_BUILD_VRAY_RENDER` | Python plugin, deploy only | On | Not licensed | Small: deploy and the id |
| RTX Render | `USDCC_BUILD_RTX_RENDER` | Plugin | Off (as today) | | Rename with an alias |
| Blender mesh tools | `USDCC_BUILD_BLENDER_MESH_TOOLS` | Plugin | On | GPL | Small: stop forcing it |
| Updates | `USDCC_BUILD_UPDATES` | In the app (`app_updates.cpp`) | On, when Velopack is found | The studio deploys versions itself, and nothing should ask the internet | Medium: compile definition in `usdcc_app` |
| Peers | `USDCC_BUILD_PEERS` | A library (`usdcc_peers`), linked by `usdcc_usd`, `usdcc_ui`, `usdcc_app` and `usdcc_node` (*checked*) | On | A build that never opens a connection | **Large:** the shared stage, rendering with peers, the render queue and the Peers panel all build on it. Its own plan when it's wanted |
| MCP, VS Code | to come with them | | | | Have an option from their first phase |

Not candidates: Python scripting and the plugin SDK. Plugins, settings and much of the UI rely on
them.

## 7. Phases

| Phase | What | Done when |
|---|---|---|
| **1. The mechanism** | `usdcc_feature()`, the generated header and `features.json`, the plugin manager's refusal, About's Features section. Remote support is its first user ([REMOTE_SUPPORT.md](REMOTE_SUPPORT.md) phase 1) | A build with `USDCC_BUILD_REMOTE_SUPPORT=OFF` has no plugin, refuses a copied one, and About says "Left out of this build" |
| **2. The plugins** | Viewport Broadcast, `http`, `webdav`, `s3`, Jolt, V-Ray, RTX (with the alias), Blender mesh tools; `deploy.py` reads `features.json` | Each one off builds, deploys and starts; CI builds once with all of them off |
| **3. Updates** | `USDCC_BUILD_UPDATES` | Off: no update check, no Help > Check for Updates |
| **4. Peers** | A plan of its own first | |

## 8. Testing

- **A CI job with every optional feature off,** so a feature that's off keeps compiling and
  starting. It doesn't deploy.
- **Unit tests** for the refusal: a plugin folder with a refused id isn't loaded and is listed as
  left out.
- Tests of a feature that's off are not built (they live with the feature's tests).

## 9. Open questions

1. **Where About's Features section goes:** its own tab, or rows under the components.
2. **Whether a left-out feature shows in Settings at all.** Its settings don't exist; should
   Settings say "Remote support: left out of this build" where its group would be?
