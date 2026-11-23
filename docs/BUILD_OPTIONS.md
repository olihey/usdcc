# Build Options (plan)

One way to leave a feature out of a build. A studio that doesn't want a feature at all, not even
turned off, builds without it, and the build can show that the feature isn't there.

**Status:** planned 2026-11-22; **phases 1 and 2 built** the same day (§10). Remote Support,
the plan's first user, doesn't exist yet, so Viewport Broadcast was the first. Phase 3 (Updates)
and 4 (Peers) are still to do. "Checked" below means looked at in this repository; everything
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
| **2. The plugins** | Viewport Broadcast, `http`, `webdav`, `s3`, Jolt, V-Ray, RTX (with the alias), Blender mesh tools; `deploy.py` reads `features.json` | Each one off builds, deploys and starts; CI builds once with all of them off, after a merge (§8) |
| **3. Updates** | `USDCC_BUILD_UPDATES` | Off: no update check, no Help > Check for Updates |
| **4. Peers** | A plan of its own first | |

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
| Deploy | [tools/deploy.py](../tools/deploy.py) | `SHIPPED_PLUGINS` and `SHIPPED_NATIVE_PLUGINS` stay as the candidates; those whose feature is off aren't copied or required, on Windows and Linux. They're still kept out of `examples/`, so a left-out plugin doesn't come back as an example. RTX follows its feature, and `--omit-rtx` still leaves it out |
| `tools/build.py --without FEATURE` | [tools/build.py](../tools/build.py) | Repeatable, or `--without all`; every run passes each default-on feature as on or off, so a build without `--without` has them all again. Without Blender mesh tools it doesn't fetch their backend |
| Tests | `tests/CMakeLists.txt` | A feature's tests are built only with it: the physics tests, `physics_timeline_test`, `viewport_broadcast_test`, the V-Ray tests, and each protocol's part of `assets_net_test.cpp` (`#ifdef` on its directory). `python_physics` checks `USDCC_FEATURE_PHYSICS_JOLT` |
| CI | `build.yml`, Windows and Linux, x64 and arm64 | On pushes to `master` only, not for pull requests: after the usual build and tests, the same build with `--without all --without-rtx`. [check_features_left_out.py](../tools/ci/check_features_left_out.py) then fails if `features.json` has a feature included, or `usdcc_node --version` doesn't list each as left out. Not deployed, its tests not run |

Tests: `plugin_left_out_of_the_build` (`usdcc_python_tests`: the features, a copied plugin of a
left-out feature refused with its dependent, its code never run, no reload), `about_dialog`
(the rows and the text), `python_app` (`build_features()`).
