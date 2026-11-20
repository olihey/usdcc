# USD plugins in CI

How usdcc's CI builds OpenUSD's third-party plugins (render delegates, file formats) the
way it already builds OpenUSD itself, and which of them a release ships. This is the
"USD plugins" item of [CI.md](CI.md) §7.

**Status:** phases 1 and 2 are on `master`, and CI has published the OpenUSD asset for
Python 3.13 with the Adobe pack. Phase 3, Gatling, is built and checked locally on Windows
and Linux and renders in usdcc's viewport; CI builds it but doesn't publish it (§5).
Phase 7, Arnold, is built locally on Windows and renders in usdcc's viewport when
Settings > Renderers > Arnold names an Arnold folder; it is a `local` pack, which CI never builds. The
rest is a plan.

## 1. Where we are

- **OpenUSD is already built by CI.** `deps-usd.yml` builds the commit `deps/usd.json` pins
  and publishes the install as a release asset; `build.yml` and `release.yml` download it
  ([CI.md](CI.md) §2–§4). `tools/build_usd.py` does the same build locally.
- **The plugins are not.** They are built by hand in a separate checkout,
  `C:\Users\olive\DEV\git\USD`, whose `tools/build.ps1` is a list of commented-out commands
  with machine paths. Its submodules are OpenUSD, Aurora, Cycles, Gatling, Adobe's
  USD-Fileformat-plugins and usdtweak.
- **Only Gatling is built against the OpenUSD usdcc uses** (`build/usd_typhoon`, since
  2026-11-11). The Cycles and Adobe builds there are configured against the older
  `build/usd`, and Aurora and Arnold aren't built.
- So a CI build or a release has Storm and Embree only, and a local build has whatever
  that folder happens to hold. `tools/deploy.py` copies all of `plugin/usd`, stale or not.

## 2. Decisions

- **Two layers, as CI.md §7 planned.** The OpenUSD asset stays as it is. Each plugin is a
  **pack**: a zip built against that asset, which unzips over it. OpenUSD's own
  `plugin/usd/plugInfo.json` includes `*/resources/`, so a pack's plugin is found with no
  further setup, by usdcc and by `tools/deploy.py`.
- **One pack per upstream project, not one big build.** A plugin's bump or breakage then
  rebuilds that pack only, not two hours of OpenUSD, and each stays inside the 6-hour job
  limit. It also lets the licences decide what ships (§5).
- **Pins live in `deps/usd-plugins/<name>.json`**, next to `deps/usd.json`: repository,
  commit, patches, CMake arguments, the SDKs to download (URL and SHA-256), licence, set.
- **One script builds a pack, locally and in CI:** `tools/build_usd_plugin.py <name>
  --usd-install <folder>`, as `tools/build_usd.py` does for OpenUSD. It replaces `build.ps1`.
- **A pack's name carries a hash** of its pin, its patches, the build script and the
  OpenUSD asset's name, so a new OpenUSD rebuilds every pack and a stale one is never used:
  `usdplugin-<name>-<short commit>-<hash>-<platform>-py313.zip`. Packs are attached to
  the OpenUSD asset's own release (`deps/usd-<commit>-<hash>`), so one release holds an
  OpenUSD and everything built against it.
- **A pack never brings a DLL the OpenUSD asset already has.** Two DLLs with one name can't
  both load in a process; the first one wins, and the other plugin gets a version it wasn't
  built with. A pack builds against the asset's copy or links its own statically. The pack
  check fails on a duplicate name (§4).
- **Hosted runners have no GPU,** so a pack's check there is "it loads and registers".
  A render delegate is a Hydra plugin like Storm or Embree, and is tried the way they are:
  in usdcc's viewport, on a machine with a GPU (§7 phase 3 has how). It has nothing to do
  with the RTX Render plugin or its runner.

## 3. The plugins

| Pack | Source and licence | Needs | Against our OpenUSD (26.08, `typhoon/main`) |
|---|---|---|---|
| `adobe-fileformats` | `adobe/USD-Fileformat-plugins` 2026.07, Apache-2.0 | Fetches its own small dependencies (TinyGLTF, fmt, Happly, spz, ...). FBX needs Autodesk's FBX SDK, SBSAR needs Adobe's Substance SDK | **Proven:** builds unpatched against the published asset and passes the pack check (§7 phase 1), though its README only names USD 23.08–25.11 |
| `gatling` | `pablode/gatling`, **GPL-3.0** | NVIDIA's MDL SDK binaries; many submodules. A Vulkan ray tracing GPU to run | **Proven:** commit `945f47d` with MDL SDK 2026.0.2 builds unpatched against the published asset and renders (§7 phase 3). 110 MB installed, 109 of them `libmdl_sdk.dll`; 58 MB zipped |
| `cycles` | `blender/cycles`, Apache-2.0 | Blender's precompiled libraries (`make update`, Git LFS, several GB). `-DPXR_ROOT=<usd>`; USD 25.11 or newer. With an external USD it builds without OSL and NanoVDB | Not proven. The local build is against the older USD |
| `aurora` | `Autodesk/Aurora` v25.08-beta, Apache-2.0 | Its `installExternals.py` builds its own USD, an Autodesk fork (`autodesk-forks/USD`, tag `v25.08-Aurora-v25.08`); Slang, the DirectX shader compiler. A DXR GPU to run | Not proven. The last commit is from August 2025; an earlier local attempt needed `/WX` turned off |
| `arnold` | `Autodesk/arnold-usd`, Apache-2.0 | The Arnold SDK, which is proprietary and behind an Autodesk Account login. Its docs name USD 20.08–25.11 and the dev branch | **Proven:** tag `Arnold-7.5.4.0` with the Arnold SDK 7.5.4.0 builds against our OpenUSD with one patch, in about a minute, and renders in usdcc's viewport (§7 phase 7). 13 files, 5.6 MB; the Arnold core (`ai.dll`, 174 MB) isn't in the pack |

Notes:

- **Adobe.** `usdgltf`, `usdobj`, `usdply`, `usdstl` and `usdspz` need nothing proprietary.
  OpenUSD's own `usdObj` example would clash with `usdobj`; `--no-examples` is already in
  `deps/usd.json`. `usdfbx` links the FBX SDK statically, so Autodesk's code ends up in the DLL.
  The plugins' names in OpenUSD are `usdGltf_plugin`, `usdObj_plugin` and so on, and they
  share three DLLs that go in `bin/` (`fileformatUtils`, `usdIpc`, `usdSerialization`).
  `usdGltf` also registers a package resolver for `.gltf` and `.glb`. By default the
  plugins write each material twice, as a UsdPreviewSurface network and as an OpenPBR
  MaterialX one, and the first read in a process warns that
  `USD_FILEFORMATS_WRITE_USDPREVIEWSURFACE` is deprecated. usdcc leaves the default (§8).
- **Gatling.** Its Sdr plugins also give the Material Editor MDL nodes
  ([MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) §3.5). The pack carries two patches
  (`deps/usd-plugins/patches/`), neither of them upstream:
  - `gatling-implicit-surfaces.patch`. Gatling only draws meshes, and brings nothing that
    turns USD's implicit shapes (Sphere, Cube, Cone, Cylinder, Capsule, Plane) into meshes
    when a host feeds it through scene indices, as usdcc's viewport does. Unpatched, such
    prims are simply missing from the picture. The patch adds the small scene index plugin
    that Storm and Embree each have, adapted from Embree's.
  - `gatling-linux-assembly.patch`. On Linux, CMake stops with "CMAKE_ASM_COMPILE_OBJECT"
    not set, because a bundled library enables assembly in its own folder only. The patch
    enables it at the top.
- **Cycles.** With `PXR_ROOT` set, Cycles takes OpenImageIO, OpenEXR, OpenColorIO,
  OpenSubdiv, MaterialX, TBB and Python from the USD install (`USD_OVERRIDE_*` in
  `src/cmake/external_libs.cmake`), but **not Embree**, and Blender's libraries have an
  `embree4.dll` too. That is the first thing the duplicate-DLL rule has to settle. The CPU
  renderer needs no SDK. The GPU kernels (CUDA, OptiX, HIP) each need a vendor SDK at build
  time, so they come after the CPU build works. Cycles has no Sdr plugin; the Material
  Editor's Cycles nodes need the generator in MATERIAL_EDITOR.md §3.4, which this pack
  could build.
- **Aurora.** Whether hdAurora's DirectX backend builds against a stock OpenUSD, rather
  than Autodesk's fork, is not known. It decides whether this pack exists at all (§8).
- **Arnold.** Its Sdr plugin starts the Arnold core at discovery (`AiBegin`), so the pack
  is off unless the user turns it on. (Here that took under half a second, for 160 Arnold
  shader nodes, but it also loads Arnold's own plugins, its USD procedural among them.)
  - **Where it is.** The pack installs into `plugin/arnold` (`install_prefix`, with
    `-DPREFIX_PLUGINS=.`), beside OpenUSD's `plugin/usd`, whose `plugInfo.json` only
    includes its own subfolders. Built with `--install`, or copied over the OpenUSD
    install, it is in `<OpenUSD>/plugin/arnold`; a deploy would have it in
    `<deploy>/plugin/arnold`, but `tools/deploy.py` leaves `local` packs out (§5).
  - **Turning it on.** Settings > Renderers > Arnold > Arnold folder (`USD/arnoldFolder`),
    from the next start: an Arnold SDK or install, the folder with `bin/ai.dll`. Once
    Arnold is on, its 54 render settings join it in that group.
    `usdcc::usd::enableArnold()` (`arnold_runtime.h`), from
    `applyUsdEnvironmentSettings()`, then:
    1. finds the pack in `plugin/arnold` next to the folder OpenUSD's libraries were
       loaded from, and reads which Arnold it was built for from
       `usd-plugins/arnold.json` (the pin's `version`);
    2. loads `bin/ai.dll` by its full path (`LOAD_WITH_ALTERED_SEARCH_PATH`), so its own
       DLLs come from its own folder and nothing goes on `PATH`. That folder has Qt 6
       DLLs of its own (3ds Max's copy and the SDK both do);
    3. asks the core its version (`AiGetVersion`). The plugins need the Arnold they were
       built for or a later one of the same 7.x: built against 7.5.4, they don't load
       with 3ds Max 2027's 7.5.2 ("The specified procedure could not be found"). Any
       other version unloads the core again, and a warning in the log says why;
    4. registers the pack's plugins (`PlugRegistry::RegisterPlugins()`). Not through
       `PXR_PLUGINPATH_NAME`: OpenUSD reads that in a static constructor of `usd_plug.dll`,
       before `main()`, so setting it there is too late. Child processes don't get them
       either, which they couldn't use without the core.
  - **The patch.** `arnold-usd-26.08.patch`: OpenUSD 26.08 (`HD_API_VERSION` 99) removed
    `HdRenderIndex::IsSceneIndexEmulationEnabled()`, which the render delegate calls in
    two places. With 26.08 the render index always takes scene indices, so the patch
    answers `true` from that version on. The other three plugins build unpatched.
  - **Not right yet.** Usdcc's grid and axis lines draw over Arnold's picture, as if the
    depth Arnold gives is not what the overlay compares against. Built with
    `-DHYDRA_NORMALIZE_DEPTH=ON` they no longer do, but the grid is then gone everywhere;
    neither is right. At exit Arnold logs "AiEnd() was not called".
  - **Licence.** Without an Arnold licence every render has an "arnold" watermark. The
    3ds Max and Maya entitlement only covers rendering inside those programs.

## 4. The build

### `deps/usd-plugins/<name>.json`

```json
{
  "repository": "https://github.com/adobe/USD-Fileformat-plugins",
  "commit": "6441b97807a59472337f9c3d02df089d45ed1c48",
  "version": "2026.07",
  "patches": [],
  "cmake": ["-Dpxr_ROOT={usd}", "-DUSD_FILEFORMATS_BUILD_TESTS=OFF", "-DUSD_FILEFORMATS_ENABLE_FBX=OFF", "..."],
  "platforms": ["win64"],
  "license": "Apache-2.0",
  "set": "standard",
  "check": {
    "plugins": ["usdGltf_plugin", "..."],
    "extensions": ["gltf", "glb", "obj", "ply", "stl", "spz"],
    "open": [{"file": "test_data/formats/triangle.gltf", "prims": {"Mesh": 1}}]
  }
}
```

- `{usd}` and `{pack}` in `cmake` become the OpenUSD install and the pack folder.
- `patches` are relative to `deps/usd-plugins/`.
- An unknown key is an error, so a misspelt one isn't ignored.
- What Gatling's pin adds (`deps/usd-plugins/gatling.json`):
  - `submodules`: the checkout's submodules too, at the commits it names.
  - `downloads`: an archive per name and platform, with its URL and SHA-256. It is
    downloaded once, checked and unpacked, and `{<name>}` in `cmake` is its folder
    (`-DMDL_ROOT={mdl-sdk}`).
  - `targets`, `install_components` and `install_prefix`: what to build, which install
    components to take, and where in the pack they go (`plugin/usd`).
  - `licenses`: more licence files to ship, such as the downloaded SDK's.
  - `publish`: `false` keeps the pack off the release (§5).
  - `renderers` in `check`: render delegates OpenUSD has to list.
- What Arnold's adds (`deps/usd-plugins/arnold.json`):
  - `sdks`: an SDK that can't be downloaded, as the Arnold SDK needs a login. The pin
    gives a description and the files the folder must have; the build takes the folder
    as `--sdk arnold=<folder>` and stops with a line saying what it needs without it.
    `{arnold}` in `cmake` and `licenses` is that folder. `SOURCES.txt` names the SDK by
    its description, not the folder, which is the machine's.
  - `preload` and `plugin_folders` in `check`: a library the check loads by its full
    path first (`{arnold}/bin/ai.dll`; `tools/ci/check_usd_plugin.py` takes `--sdk` too),
    and the pack's folders it registers, as usdcc does for a pack that is off by default.
- Still to come: a small Python hook next to the JSON for a build that isn't plain CMake
  (Cycles' `make update`, Aurora's externals script).

### `tools/build_usd_plugin.py`

`python tools/build_usd_plugin.py <pack> --usd-install <folder>`, with the pin's Python:

1. Fetch the pinned commit (shallow), with its submodules if the pin says so, and apply
   the patches.
2. Download the pin's archives, check their SHA-256 and unpack them.
3. Configure with the OpenUSD install on `CMAKE_PREFIX_PATH`, Release, `/MD`, Visual
   Studio 2022, the Python it runs on: the same rules as usdcc plugins (CI.md §2).
4. Build (the pin's targets, or everything) and install (its components, or everything)
   into an empty **pack folder** laid out like the OpenUSD install (`plugin/usd/...`,
   `bin/` for DLLs the plugins need).
5. Drop what only a build needs: `include/`, CMake and pkg-config files, import libraries.
6. Collect licences into `licenses/usd-plugins/<pack>/<library>/`: the plugin's own, those
   of what CMake fetched into it (`tools/ci/usd_licenses.py`), each submodule's, and the
   files the pin names. `SOURCES.txt` beside them says what the binaries were built from:
   the repository and commit, the patches, each download's URL and SHA-256.
7. Write `usd-plugins/<pack>.json`: the pin, the OpenUSD it was built against, and the
   list of the pack's files, which `deploy.py` and the check go by.

`--install` copies the pack over the OpenUSD install, which is what `build.ps1` did.
Linux, with RUNPATHs relative to `$ORIGIN`, is phase 8.

**Folders are used as given, not resolved.** A plugin with many submodules, like OpenUSD
itself, doesn't fit under Windows' 260-character path limit in a deep folder. CI builds in
`C:\b`. Locally a drive letter mapped onto the folder does it (`subst W: <folder>`, then
`--work W:\gatling`), and the script lets git write long paths, since git resolves the
drive letter back.

### Workflow `deps-usd-plugins.yml`

One job per pack and platform (a matrix from the JSON files, `usd_plugin_asset.py
--matrix`). It runs by hand, on a push that changes a pin, a patch or one of the scripts,
and after `deps-usd.yml` has run, since a new OpenUSD gives every pack a new name.

1. Name the pack (`tools/ci/usd_plugin_asset.py`) and skip the job if it's published.
2. Download the OpenUSD asset and unzip it.
3. `tools/build_usd_plugin.py`, then zip the pack.
4. **Check the zip** (`tools/ci/check_usd_plugin.py`), unzipped elsewhere, with the build
   tree deleted:
   - it holds exactly the files its `usd-plugins/<pack>.json` lists;
   - none of its files is in the OpenUSD asset, and no DLL or `.so` in it has the name of
     one there;
   - no text file in it names the build folder;
   - then, copied over the OpenUSD asset, in a fresh Python with only the asset on `PATH`:
     OpenUSD finds each plugin and loads it, so every dependent DLL is there; each
     extension has a file format; each sample file opens with the prims it should have.
   - a render delegate has to be among those OpenUSD lists. It isn't created: a path
     tracer's needs a GPU, which a hosted runner doesn't have.
   - Still to come: no DLL name shared with another pack.
5. Attach the zip to the OpenUSD asset's release, or, for a pin with `"publish": false`,
   keep it with the workflow run for two weeks (§5). The pack's earlier zips stay there: an
   older usdcc tag's pins name them, and `release.yml` can be run again for that tag.

A pack marked `local` (§5) is built only when its SDK is there (a secret URL, or the
self-hosted runner) and is kept as a workflow artifact, never published.

### `build.yml`, `release.yml`, `tools/deploy.py`

- **`tools/fetch_usd.py <folder>`** downloads the OpenUSD asset and the `standard` packs
  for the current pins, and unzips the packs over it. A new machine then needs no OpenUSD
  build, and a local build uses exactly what CI does.
  - They are release assets of a public repository, so it needs no login and no `gh`.
  - Run again after a pin changed, it replaces a pack that is out of date, deleting the
    old one's files by its list. An OpenUSD that is out of date is refused: nothing lists
    its files, so it wants a new folder.
  - `--zips <folder>` takes the zips from a folder first, for a pack built locally.
- **`build.yml` and `release.yml`** run it with `--packs-only` after unzipping OpenUSD.
  A pack that isn't published is a warning in `build.yml` (it may still be building, and
  usdcc builds without it) and an error in `release.yml` (`--require-packs`). Windows
  only, as the packs are.
- **`tools/deploy.py`** no longer copies all of `plugin/usd`:
  - OpenUSD's own plugins come from a list of names (`OPENUSD_PLUGINS`), reviewed against
    the pinned OpenUSD and its flags. The Blender configuration already worked that way.
    So a renderer installed by hand into a local OpenUSD isn't deployed any more.
  - A `standard` pack's files are copied one by one from its list: `bin/` and
    `plugin/usd/` as they are, its licences to `licenses/OpenUSD-plugins/<pack>/`.
    `licenses/SOURCES.txt` gets the pack's version, licence, repository and commit.
  - The deploy gets `usd-plugins/<pack>.json`, listing the pack's files as they are there.
  - A pack of another set stays behind, with a line saying so. The Blender configuration
    takes no packs: its review covers OpenUSD's own plugins only.
  - On Linux only the list of OpenUSD's own plugins applies; packs come with phase 8.
- **`tools/ci/smoke_test.py`**, inside the started deploy: OpenUSD finds Storm and Embree
  and loads them from the deploy, and for each pack the deploy says it has, the pin's
  `check` holds (the plugins load from the deploy, the extensions have file formats, the
  samples open). `--expect-standard-usd-packs`, which `release.yml` passes, fails a deploy
  without a pack of the standard set.
- **`usdcc_tests`** has `usd_plugin_pack_file_formats`: each sample in `test_data/formats`
  opens with one mesh of one triangle. A format this OpenUSD has no plugin for is skipped
  with a line saying so, so the tests pass against a plain OpenUSD.

## 5. What ships

Three sets, decided by licence:

| Set | Where it goes | Packs |
|---|---|---|
| `standard` | In the usdcc zip | `adobe-fileformats` (glTF, OBJ, PLY, STL, SPZ), `cycles`, `aurora` |
| `optional` | Its own zip on the usdcc release, unzipped over the usdcc folder by the user | `gatling` |
| `local` | Built on a machine that has the SDK; never published | `arnold`, `usdfbx`, `usdsbsar` |

- **Gatling is GPL-3.0.** Loaded into usdcc's process it makes the whole a combined work,
  the question the Blender mesh tools already raised ([RELEASE.md](RELEASE.md) §6). It
  stays out of the standard zip, which remains BSD-3-Clause. Whether usdcc may publish even
  a separate pack needs the same review as the Blender configuration, with OpenUSD's TOST
  licence in it. Until then it is built and checked in CI and used locally: its pin says
  `"publish": false`, so `deps-usd-plugins.yml` keeps the zip with the workflow run for
  two weeks and attaches nothing to the release. The pack already carries what a GPL
  binary has to: the licence texts of Gatling, its 24 submodules and the MDL SDK
  (BSD-3-Clause), and `SOURCES.txt` naming the commit and the SDK it was built from.
- **Arnold, FBX and Substance** need SDKs that can't be republished. Whether a plugin
  built against them may be published without the SDK is a question for their licence
  texts (§8).
- **Arnold's SDK licence** (`EULA.txt` in the SDK, which the pack carries in
  `licenses/usd-plugins/arnold/arnold-sdk/`) is more open than that: it lets the SDK be
  distributed "as an integrated part of" an application, only embedded in it, only to
  those the application is licensed to, under terms "no less protective" of Autodesk
  than its Terms of Use, and renders from it have the watermark. It refers to Autodesk's
  general Terms of Use, which haven't been read here (the page refuses automated
  fetches). usdcc's BSD-3-Clause doesn't meet the "no less protective" condition by
  itself, so shipping Arnold would need terms of its own for that part. Until that is
  decided the pack stays `local`.
- **Standard packs are Apache-2.0**, but each brings libraries of its own (Cycles: OpenVDB,
  Open Image Denoise; Aurora: Slang, the DirectX shader compiler's `dxil.dll`). Step 5 of
  the build collects their licences, and a pack isn't `standard` until that list is read.

## 6. Changes to the OpenUSD build itself

`build_usd.py` can build more of OpenUSD's own plugins. Each flag changes
`deps/usd.json`, so one rebuild (two hours) covers all of them, and every pack follows.

| Flag | Adds | Recommendation |
|---|---|---|
| `--draco` | `usdDraco`, and Draco-compressed glTF in Adobe's `usdgltf` | Yes |
| `--alembic` | `usdAbc`, reading `.abc` | Yes, without `--hdf5` (Ogawa files only) |
| `--openvdb` | `hioOpenVDB`: volumes in Storm, and an OpenVDB Cycles can share | Yes, if Cycles ships |
| `--onetbb` | oneTBB instead of TBB 2020, which the install has now (`tbb.dll`). Blender's libraries and Adobe's README expect oneTBB | Try it locally first: it changes the ABI of everything |
| `--ptex` | Ptex textures | No, until someone asks |
| `--vulkan` | HgiVulkan for Storm. Needs the Vulkan SDK on the runner | Not now |
| `--prman` | hdPrman. Needs RenderMan installed | No: a `local` build at most |

## 7. Phases

1. **The framework, with Adobe's file formats.** The JSON format,
   `tools/build_usd_plugin.py`, `tools/ci/usd_plugin_asset.py`, `deps-usd-plugins.yml` with
   the pack check, and `adobe-fileformats` (glTF, OBJ, PLY, STL, SPZ) on Windows. It needs
   no GPU and no SDK, so all of it can be checked on a hosted runner. Done when the pack is
   published and passes its check from another folder.
   *Built, and checked locally; not published. Against the published OpenUSD asset,
   unzipped in a temporary folder, Adobe's 2026.07 built without a patch. The pack is 33
   files (five plugins, three shared DLLs, the licences of Adobe's code and of nine
   libraries built into it) and 1.6 MB zipped.
   Its zip, unzipped over a second copy of the asset, passed the check: the five plugins
   load and `test_data/formats/triangle.gltf`, `.obj`, `.ply` and `.stl` each open with one
   Mesh. `.spz` only has its file format checked; there is no sample. The workflow has not
   run: it starts with the first push to `master` that has it.*
2. **usdcc uses packs.** `build.yml`, `release.yml`, `deploy.py` by pack name, the smoke
   test, the file format test, `tools/fetch_usd.py`. Done when a `master` build opens a
   `.gltf` in its tests and a deploy has the plugins and their licences.
   *Built, and checked locally on Windows; no workflow has run with it. `fetch_usd.py`
   put the asset and the pack into a new folder from local zips, replaced an older build
   of the pack on a second run, and warned, or failed with `--require-packs`, for the
   pack that isn't published; the asset's download address was tried on its first bytes
   only. usdcc built against that folder: all nine test programs pass, `usdcc_tests` with
   223 tests and the four samples read, and against the asset without the pack the same
   test skips all four. A deploy has the five plugins, their three DLLs, the licences in
   `licenses/OpenUSD-plugins/adobe-fileformats/` and its entry in `SOURCES.txt`; started by
   the smoke test, it loads Storm, Embree and the five plugins from its own folder and
   reads the samples, and the smoke test fails it once its pack record is taken away.
   Not tried: Linux, where `deploy.py` now also takes OpenUSD's plugins from the list.*
   *The published Windows OpenUSD asset (built 2026-11-06) has no `licenses/OpenUSD/`,
   which `deploy.py` has needed since; the local deploy got the two files from an OpenUSD
   checkout. `release.yml`'s Windows deploy needs `deps-usd.yml` run with `replace`.*
   *Since then: both are on `master`. The Python 3.13 pin (CI.md §2.1) rebuilt the asset,
   which has `licenses/OpenUSD/` now, and CI published the Adobe pack beside it. A deploy
   built from that asset needed no workaround (phase 3).*
3. **Gatling**, as the `optional` set's first pack, from the commit and MDL SDK that work
   locally. Done when the pack is published and a deploy with it unzipped over lists
   Gatling and renders in usdcc's viewport.
   *Built, and checked locally on Windows and Linux; not published, by decision (§5, §8
   item 2), and no workflow has run with it.*
   - *Windows. Against the published OpenUSD asset for Python 3.13, downloaded with
     `tools/fetch_usd.py`, Gatling builds in 7 minutes: 76 files, 58 MB zipped. Its zip,
     unzipped over a second copy of the asset, passes the check: the plugin loads and
     `HdGatlingRendererPlugin` is among three render delegates. usdcc built against the
     asset passes all ten test programs; its deploy lists Storm and Embree, and with the
     pack unzipped over the deploy it lists Gatling too and loads it from its own folder
     (`tools/ci/smoke_test.py`, which now reads a pin's `renderers`).*
   - *In usdcc's viewport. That deploy, started with a window on the RTX 4090 here,
     opened a scene of a Sphere, a ground mesh and a dome light; a script switched the
     viewport from Storm to Gatling (`Viewport.set_renderer()`) and saved the viewport
     window's own picture after each. Gatling drew the dome and the ground but not the
     sphere, which is what `gatling-implicit-surfaces.patch` (§3) is for; with it the
     sphere is there, path traced, with its shadow.*
   - *Linux. In WSL (Ubuntu 24.04, GCC 13), with a Python 3.13 from uv in a folder of its
     own, against the published Linux asset: it took the assembly patch (§3) and one more
     thing, since OpenUSD's `retainedDataSource.h` doesn't compile as C++20 with GCC, so
     the new file is compiled as C++17 there. The pack is 76 files, 71 MB zipped;
     `hdGatling.so` gets the RUNPATH `$ORIGIN:$ORIGIN/../../lib`, and the pack passes the
     same check over a second copy of the asset. `deps-usd-plugins.yml` has a `linux` job
     for the packs whose pins list `linux-x64`.*
   - *Not done: a render on Linux (WSL has no Vulkan ray tracing), the Linux deploy taking
     packs (phase 8), and any workflow run. Each edit to `tools/build_usd_plugin.py` or
     `tools/ci/usd_plugin_asset.py` renames every pack, so the Adobe pack is rebuilt by CI
     after this one too, and `build.yml` warns that it is missing until then.*
4. **The OpenUSD flags** of §6, tried locally first, then one `deps-usd.yml` run. Done when
   the new asset and every pack are published and `build.yml` is green.
5. **Cycles, CPU only.** Settle the Embree clash, then the pack. Done when it renders a
   frame on a machine without a GPU. Then the GPU kernels, one device at a time.
6. **Aurora.** First a local build against our OpenUSD. If it needs Autodesk's fork, stop
   and drop it.
7. **Arnold**, as a `local` pack on the self-hosted runner.
   *Built, and checked locally on Windows; never built by CI (`local`).*
   - *The SDK. 3ds Max 2027's Arnold (MAXtoA) has the core, 7.5.2, but no headers and no
     `ai.lib`, and its own `hdArnold.dll` is built against 3ds Max's OpenUSD
     (`3dsmax_usd.dll`, oneTBB's `tbb12.dll`), so none of it can be used here. The SDK
     comes from Autodesk Account only with an Arnold, Maya or Collection subscription;
     the 30-day Arnold trial gave it.*
   - *The pack. `python tools/build_usd_plugin.py arnold --usd-install <OpenUSD> --sdk
     arnold=<Arnold SDK>` built it in a minute, against a local build of the OpenUSD the
     pin names; without `--sdk` it stops and says what it needs. `check_usd_plugin.py`
     with `--sdk` passed over a second copy of that OpenUSD: the core loads, the four
     plugins load from the install, and `HdArnoldRendererPlugin` is among three render
     delegates. OpenUSD's shader registry then has 160 Arnold nodes.*
   - *In usdcc's viewport. With the pack copied over that OpenUSD and usdcc (Release) built
     against it, under a test profile: with no Arnold folder, Storm and Embree only and no
     `ai.dll` in the process. With the SDK as the Arnold folder, after a restart, Arnold
     is among the renderers, with neither its folder on `PATH` nor anything added to
     `PXR_PLUGINPATH_NAME`; a script switched a scene of a Sphere, a Cube, a ground mesh,
     a dome and a distant light from Storm to Arnold and saved the viewport window's
     picture: all five there, path traced, with shadows and the watermark, and 54 Arnold
     settings in the viewport's renderer settings. With 3ds Max's 7.5.2 as the folder, a
     warning in the log and no Arnold. `usdcc_tests`' `arnold_runtime` checks the version
     rule and what stops it early, without an Arnold.*
   - *Not done: the overlay's depth (§3), materials and the Material Editor's Arnold
     nodes, GPU rendering, Linux, and the self-hosted runner.*
8. **Linux** for each pack that supports it (all but Aurora's DirectX backend), as
   `linux` jobs in the same matrix. *The job and the build script's RUNPATH step are
   there since phase 3, with Gatling's pack. Left: the Adobe pack's pin, and
   `tools/deploy.py` and `tools/fetch_usd.py` taking packs on Linux.*

## 8. Open questions

| # | Question | Assumption until decided |
|---|---|---|
| 1 | Do the plugins build against 26.08 from `typhoon/main` at all? Gatling and Adobe's are proven; arnold-usd names 25.11 as its newest | Try each locally before its phase; carry small fixes as patches in `deps/usd-plugins/` |
| 2 | May usdcc publish a GPL-3.0 Gatling pack as a separate download? | No, until it has had the Blender configuration's review. Then `"publish": true` in `deps/usd-plugins/gatling.json` is all it takes |
| 3 | May a `usdfbx` or hdArnold built against the vendor's SDK be published without the SDK? | No: `local` only. Arnold's `EULA.txt` allows shipping even the core inside an application, on conditions (§5); Autodesk's general Terms of Use and the FBX SDK's licence are still to read |
| 4 | Does hdAurora build against a stock OpenUSD? | Unknown; phase 6 answers it |
| 5 | Switch OpenUSD to oneTBB? | Yes if a local build of usdcc and its tests pass with it, since Cycles and Adobe expect it |
| 6 | Are the Releases page's `deps/...` entries getting too many (RELEASE.md §7 item 2)? | Packs add assets to existing releases, not new releases, so no |
| 7 | Should usdcc set `USD_FILEFORMATS_WRITE_USDPREVIEWSURFACE=0`? It ends the warning at the first file read, but then an imported material is only an OpenPBR MaterialX network, which a delegate without MaterialX can't show | No: keep Adobe's default and the one warning in the log. A setting in Settings > USD when someone wants it |

## 9. Later

- **More render delegates.** AMD's Radeon ProRender (`hdRpr`) and Intel's OSPRay
  (`hdOSPRay`, a CPU renderer, so testable on hosted runners) are open source. Whether
  either still follows current OpenUSD has to be checked first. MoonRay doesn't build on
  Windows. Karma, Redshift, Octane and V-Ray's delegate ship inside their own products,
  built for those products' USD, and can't be loaded here.
- **A canary run.** A weekly job that builds the packs against OpenUSD's newest release,
  without publishing, so a plugin that breaks on the next OpenUSD is known before the bump.
- **A symbols asset** for OpenUSD and the packs (`relwithdebinfo`), CI.md §6 item 3.
- **macOS**, with CI.md §7.
