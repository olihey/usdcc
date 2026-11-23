# USD plugins in CI

How usdcc's CI builds OpenUSD's third-party plugins (render delegates, file formats) the
way it already builds OpenUSD itself, and which of them a release ships. This is the
"USD plugins" item of [CI.md](CI.md) §7.

**Status:** phases 1 and 2 are on `master`, and CI has published the OpenUSD asset for
Python 3.13 with the Adobe pack for Windows; its Linux pack, and usdcc's Linux build and
deploy with it, are built and checked locally (phase 8). Phase 3, Gatling, is built and checked locally on Windows
and Linux and renders in usdcc's viewport; CI builds it but doesn't publish it (§5).
Phase 7, Arnold, is built locally on Windows and renders in usdcc's viewport when
Settings > Renderers > Arnold names an Arnold folder; it is a `local` pack, which CI never builds.
Phase 6, Aurora, was tried locally (2026-11-23): hdAurora **builds against our OpenUSD** with a
small patch, no Autodesk fork needed, and registers in usdcc, but its renderer fails to start
there; no pin yet (§7 phase 6). The rest is a plan.

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
| `adobe-fileformats` | `adobe/USD-Fileformat-plugins` 2026.07, Apache-2.0 | Fetches its own small dependencies (TinyGLTF, fmt, Happly, spz, ...). FBX needs Autodesk's FBX SDK, SBSAR needs Adobe's Substance SDK | **Proven:** builds unpatched against the published asset and passes the pack check, on Windows (§7 phase 1) and on Linux (phase 8), though its README only names USD 23.08–25.11 |
| `gatling` | `pablode/gatling`, **GPL-3.0** | NVIDIA's MDL SDK binaries; many submodules. A Vulkan ray tracing GPU to run | **Proven:** commit `945f47d` with MDL SDK 2026.0.2 builds unpatched against the published asset and renders (§7 phase 3). 110 MB installed, 109 of them `libmdl_sdk.dll`; 58 MB zipped |
| `cycles` | `blender/cycles`, Apache-2.0 | Blender's precompiled libraries (`make update`, Git LFS, several GB). `-DPXR_ROOT=<usd>`; USD 25.11 or newer. With an external USD it builds without OSL and NanoVDB | Not proven. The local build is against the older USD |
| `aurora` | `Autodesk/Aurora` v25.08-beta, Apache-2.0 | Its `installExternals.py` builds its own USD, an Autodesk fork (`autodesk-forks/USD`, tag `v25.08-Aurora-v25.08`); Slang, the DirectX shader compiler. A DXR GPU to run | **Builds** against our OpenUSD with `aurora-openusd-26.08.patch`, its externals script told not to build what OpenUSD has (`aurora-externals-from-usd.patch`). **Doesn't render yet:** creating the renderer fails in usdcc (§7 phase 6) |
| `arnold` | `Autodesk/arnold-usd`, Apache-2.0 | The Arnold SDK, which is proprietary and behind an Autodesk Account login. Its docs name USD 20.08–25.11 and the dev branch | **Proven:** tag `Arnold-7.5.4.0` with the Arnold SDK 7.5.4.0 builds against our OpenUSD with one patch, in about a minute, and renders in usdcc's viewport (§7 phase 7). 13 files, 5.6 MB; the Arnold core (`ai.dll`, 174 MB) isn't in the pack |
| `moonray` | `OpenMoonRay/openmoonray` (DreamWorks' MoonRay, an ASWF project since May 2026), Apache-2.0 | Linux x64 and macOS on Apple silicon; **no Windows**. ISPC, OpenVDB, Lua, JsonCpp, log4cplus, libunwind, Open Image Denoise, and the libraries OpenUSD has too (Boost, TBB, Embree 4, OpenImageIO, OpenEXR, OpenColorIO, OpenSubdiv, MaterialX). OptiX 7.6 headers and CUDA only for its GPU mode | Not proven. Its own `building/` scripts build their own USD; upstream tests Rocky 9 (GCC 11) and macOS 14–26, not Ubuntu 24.04 |

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
- **Cycles.** The render delegate to ship first: it builds on Windows, Linux and macOS,
  so it is the one renderer besides Storm and Embree that every platform gets (MoonRay
  has no Windows build).
  - **Its libraries.** `make update` fetches Blender's precompiled libraries for the
    platform (Git LFS, several GB; CI caches them). With `PXR_ROOT` set, Cycles takes
    OpenImageIO, OpenEXR, OpenColorIO, OpenSubdiv, MaterialX, TBB and Python from the USD
    install (`USD_OVERRIDE_*` in `src/cmake/external_libs.cmake`), but **not Embree**, and
    Blender's libraries have an `embree4.dll` too. That is the first thing the
    duplicate-DLL rule has to settle: the asset's Embree if it is Embree 4, otherwise
    Blender's linked statically. What the pack keeps from Blender's libraries is only
    what hdCycles links and the asset lacks, Open Image Denoise above all.
  - **TBB.** Blender's libraries are built with oneTBB, and current Cycles expects it; the
    asset has TBB 2020 (§6). Two TBBs can't share a process, so `--onetbb` in the OpenUSD
    build comes first (§7 phase 4), and OpenVDB with it (`--openvdb`), so Cycles shares
    the asset's instead of bringing its own.
  - **CPU first.** The CPU renderer needs no SDK, so the pack check renders a frame on a
    hosted runner. The GPU devices each need a vendor SDK at build time and come after,
    one at a time (§7 phase 5): Metal on macOS needs none; CUDA and OptiX need NVIDIA's
    CUDA Toolkit and OptiX headers, which build on a runner without a GPU, though
    compiling kernels for many GPU generations is slow against the 6-hour job limit, so
    the pin names a few; HIP and oneAPI last.
  - **Material Editor nodes.** Cycles has no Sdr plugin; the Material Editor's Cycles
    nodes need the generator in MATERIAL_EDITOR.md §3.4, which this pack builds, so
    `cycles_shaders.usda` ships with it.
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
  - **Not right yet.** Known issues, each tracked on GitHub with the `known-issue` label
    ([RELEASE.md](RELEASE.md) §9):
    [#7](https://github.com/olihey/usdcc/issues/7) the stretched orthographic view,
    [#8](https://github.com/olihey/usdcc/issues/8) the grid over Arnold's picture,
    [#9](https://github.com/olihey/usdcc/issues/9) "AiEnd() was not called".
    Usdcc's grid and axis lines draw over Arnold's picture, as if the
    depth Arnold gives is not what the overlay compares against. Built with
    `-DHYDRA_NORMALIZE_DEPTH=ON` they no longer do, but the grid is then gone everywhere;
    neither is right. At exit Arnold logs "AiEnd() was not called".
    An orthographic view is stretched by the viewport's width/height ratio, more as a
    resize takes it further from square (Storm and Embree aren't). arnold-usd's bug, not
    usdcc's: `HdArnoldCamera::GetScreenWindowFromOrthoProjection` (`camera.cpp`, also
    called from `render_pass.cpp` on every resize) gives the `ortho_camera` a
    `screen_window` whose Y is the projection's own half-height, but Arnold scales the
    window's Y by the image aspect itself, so the aspect counts twice. A 0.5-unit sphere
    rendered 200×100 by Arnold alone: window (-1,-1)–(1,1) gives 50×50 px, round; the
    (-1,-0.5)–(1,0.5) arnold-usd passes gives 50×100. Its comment calls taking Y from
    the X extent wrong, but that is what Arnold wants; upstream `master` still has it
    (2026-11-20). The fix, for the patch: Y from the X extent again, offset included
    (`unitX` throughout).
  - **Licence.** Without an Arnold licence every render has an "arnold" watermark. The
    3ds Max and Maya entitlement only covers rendering inside those programs.
- **MoonRay.** A production CPU path tracer with a hybrid GPU mode (XPU: CUDA/OptiX on
  Linux, Metal on macOS) and a Hydra render delegate, HdMoonRay. It doesn't build on
  Windows, so it is the first pack that is Linux and macOS only; on Windows the renderer
  is simply not in the list.
  - **What the pack is.** HdMoonRay in `plugin/usd`, MoonRay's core libraries in `lib`,
    and its shader DSOs (the RDL2 classes the scene is made of) in a folder of their own.
    Not `moonray_gui` or `arras_render`, which need Qt 5 and would put a second Qt into
    usdcc's process. Not Arras, the distributed rendering framework, if HdMoonRay renders
    in-process without it (§8); it brings libmicrohttpd (LGPL-2.1) and libcurl.
  - **CPU only first** (`-DMOONRAY_USE_OPTIX=NO`): no NVIDIA headers on the runner, no
    CUDA runtime to ship, and nothing hosted runners can't run. Since it renders on the
    CPU, its check can render a frame, not just load the plugin: the first render delegate
    that hosted CI can actually try (§2).
  - **Our OpenUSD, not its own.** MoonRay's `building/` scripts build every dependency,
    USD included; the Houdini route (`-DNOUSD=1`, the `*-houdini-release` presets) shows
    it can take an outside USD. The pack builds the dependencies the asset lacks from
    those scripts, and takes the rest from the asset (§2's duplicate-DLL rule). Where the
    versions disagree, Embree and TBB first: MoonRay needs Embree 4, and the asset's TBB
    is TBB 2020 (§6 `--onetbb`). Whatever only MoonRay needs is linked statically or goes
    into `lib` under a name the asset doesn't have.
  - **Turning it on.** MoonRay finds its shader DSOs through its environment (its
    `setup.sh` sets the DSO path). usdcc sets that in-process at start-up, before
    anything asks for a renderer, the way `enableArnold()` loads Arnold: a
    `moonray_runtime` beside `arnold_runtime`, Linux and macOS only, on by default since
    the pack ships (§5) and needs no SDK.

## 4. The build

### `deps/usd-plugins/<name>.json`

```json
{
  "repository": "https://github.com/adobe/USD-Fileformat-plugins",
  "commit": "6441b97807a59472337f9c3d02df089d45ed1c48",
  "version": "2026.07",
  "patches": [],
  "cmake": ["-Dpxr_ROOT={usd}", "-DUSD_FILEFORMATS_BUILD_TESTS=OFF", "-DUSD_FILEFORMATS_ENABLE_FBX=OFF", "..."],
  "platforms": ["win64", "linux-x64"],
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
- `platforms`: where the pack is built. CI's `windows` and `linux` jobs each take the
  packs that list theirs.
- One of Adobe's `cmake` arguments is for Linux only: `-DUSD_FILEFORMATS_ENABLE_CXX11_ABI=ON`.
  Without it Adobe's build sets `_GLIBCXX_USE_CXX11_ABI=0`, the old `std::string`, and
  the plugins then don't find OpenImageIO's functions in our OpenUSD, which GCC 13 built
  with the new one. Windows ignores the argument.
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

**On Linux** it configures with Ninja, and after step 5 gives the pack's binaries RUNPATHs
relative to `$ORIGIN` (patchelf), so the pack works wherever it is unzipped. A binary is
changed in two cases, and gets its own folder and the `lib` folder:

- the build linked it by absolute path, to the OpenUSD install's `lib` (Gatling's
  `hdGatling.so`);
- it needs a library of `lib`, the pack's or the install's, and its RUNPATH doesn't lead
  there. Adobe's plugins are installed into `plugin/usd` with `$ORIGIN/.` only, and
  `libfileformatUtils.so`, which all five need, goes into `lib`.

A prebuilt library that came with an SDK is neither and stays as it is.

**Folders are used as given, not resolved.** A plugin with many submodules, like OpenUSD
itself, doesn't fit under Windows' 260-character path limit in a deep folder. CI builds in
`C:\b`. Locally a drive letter mapped onto the folder does it (`subst W: <folder>`, then
`--work W:\gatling`), and the script lets git write long paths, since git resolves the
drive letter back.

### Workflow `deps-usd-plugins.yml`

One job per pack and platform (a matrix from the JSON files, `usd_plugin_asset.py
--matrix`, which also names each platform's runner): `win64` and `linux-x64`, and
`win-arm64` and `linux-arm64` on GitHub's Arm runners, for the pins that list them
([ARM64_RELEASES.md](ARM64_RELEASES.md)). The Windows Arm jobs set up Python as
`build.yml`'s do, and set `CMAKE_POLICY_VERSION_MINIMUM=3.5` for that image's CMake 4. It runs by hand, on a push that changes a pin, a patch or one of the scripts,
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
  usdcc builds without it) and an error in `release.yml` (`--require-packs`). The
  `windows` and the `linux` jobs both do: each gets the standard packs whose pins list
  its platform.
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
  - On Linux it is the same, with the libraries in `lib/`, beside OpenUSD's, where
    Windows has `bin/`. The packs go in before the deploy sets its RUNPATHs, so their
    files get the deploy's (`$ORIGIN/../../lib` in `plugin/usd`, `$ORIGIN` in `lib`) and
    are among those the deploy checks with `ldd`. The libraries of a pack that is left
    out stay behind too, though they sit among OpenUSD's own in the install's `lib`.
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
| `standard` | In the usdcc zip | `adobe-fileformats` (glTF, OBJ, PLY, STL, SPZ), `cycles` and `moonray` (Linux, later macOS), each `optional` and unpublished until its licences are read; `aurora` |
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
- **Cycles is Apache-2.0**, kept apart from Blender's GPL code, so it can go in the
  standard zip, which stays BSD-3-Clause. Apache-2.0 asks for its `LICENSE` and `NOTICE`
  in the release, and for files our patches change to say so. What it brings:
  - Cycles' own `third_party/`: small permissive libraries (Apache, MIT, zlib, BSD);
    licence texts only.
  - Blender's precompiled libraries: a mix, with GPL ones such as FFmpeg among them.
    **Only what hdCycles links is shipped**, and Cycles doesn't use FFmpeg. A new step in
    `tools/ci/check_usd_plugin.py` fails the pack on a library the pin doesn't list, found from what the binaries load
    (`dumpbin /dependents` on Windows, `ldd` on Linux), so nothing GPL comes in by
    accident.
  - Open Image Denoise, OpenVDB, Embree: Apache-2.0; licence texts only.
  - The GPU devices, each read before it ships: compiled CUDA kernels (NVIDIA's CUDA
    EULA, which generally allows redistributing compiled kernels), OptiX (its SDK
    licence; the headers are build-time and the runtime is in the driver), HIP (AMD,
    mostly MIT), oneAPI (Intel's runtime licence).

  Until step 6 of the build has collected that list and it has been read, Cycles' pin
  says `"set": "optional"` with `"publish": false`, for the reason given for MoonRay
  below. If the GPU kernels make the standard archive too big, they become a pack of
  their own in the `optional` set, and the standard one stays CPU only.
- **MoonRay is Apache-2.0**, so it can go in the standard zip, which stays BSD-3-Clause.
  Apache-2.0 asks for its `LICENSE` and `NOTICE` in the release, and for files our patches
  change to say so. What it brings, as far as is known before a build:
  - Permissive, licence texts only: OpenImageIO, OpenVDB, Open Image Denoise, OpenSubdiv,
    Boost, JsonCpp, Lua, log4cplus, libunwind, Random123, libcurl.
  - Build only, not shipped: ISPC (BSD-3-Clause), CppUnit (LGPL, tests).
  - libmicrohttpd (LGPL-2.1), Arras only: allowed as a separate shared library the user
    can replace, with its source named in `SOURCES.txt`, as Qt is (RELEASE.md §6).
    Better left out with Arras.
  - NVIDIA's OptiX SDK and CUDA runtime, GPU mode only: their own licences, read before
    the GPU build ships. The OptiX headers are build-time; the runtime is in the driver.

  Until step 6 of the build has collected that list and it has been read, the pin says
  `"set": "optional"` with `"publish": false`, as Gatling's does. Not `standard` yet:
  `release.yml` fetches the standard packs with `--require-packs`, so an unpublished
  standard pack would fail every Linux release.

## 6. Changes to the OpenUSD build itself

`build_usd.py` can build more of OpenUSD's own plugins. Each flag changes
`deps/usd.json`, so one rebuild (two hours) covers all of them, and every pack follows.

| Flag | Adds | Recommendation |
|---|---|---|
| `--draco` | `usdDraco`, and Draco-compressed glTF in Adobe's `usdgltf` | Yes |
| `--alembic` | `usdAbc`, reading `.abc` | Yes, without `--hdf5` (Ogawa files only) |
| `--openvdb` | `hioOpenVDB`: volumes in Storm, and an OpenVDB Cycles can share | Yes, if Cycles ships |
| `--onetbb` | oneTBB instead of TBB 2020, which the install has now (`tbb.dll`). Blender's libraries and Adobe's README expect oneTBB | Yes, before Cycles (§3), which can't share a process with TBB 2020. Try it locally first: it changes the ABI of everything |
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
5. **Cycles**, after phase 4 has given the asset oneTBB and OpenVDB (§3, §6).
   1. A local build on Windows against that asset, CPU only: which Embree it uses, and
      the list of what hdCycles loads from Blender's libraries.
   2. `deps/usd-plugins/cycles.json` (`win64` and `linux-x64`, `optional`,
      `"publish": false`). The pack check renders a small scene on the CPU and fails on a
      library the pin doesn't list (§5). The pack builds the Material Editor's
      `cycles_shaders.usda` too.
   3. In usdcc's viewport on both platforms, and the Material Editor's Cycles nodes.
   4. The licence list read (§5), then `"publish": true`, CI publishes the pack, and only
      then `"set": "standard"`, so `release.yml` requires it and the smoke test expects it.

   Done when a Windows and a Linux release have Cycles and render with it in the viewport,
   on a machine without a GPU. Then the GPU devices, one at a time, each a pin change and
   a licence read (§5): CUDA and OptiX (checked on the self-hosted RTX runner), HIP,
   oneAPI. On macOS, Cycles and its Metal device follow usdcc's own macOS build, as
   MoonRay does (phase 10).
6. **Aurora.** First a local build against our OpenUSD. If it needs Autodesk's fork, stop
   and drop it.
   *Tried locally on Windows (2026-11-23): it doesn't need the fork, but it doesn't render
   in usdcc yet, so there is no pin.*
   - *The externals. v25.08-beta's `installExternals.py`, with
     `deps/usd-plugins/patches/aurora-externals-from-usd.patch`, leaves out what our OpenUSD
     install has (OpenUSD itself, MaterialX, OpenImageIO, OpenEXR, OpenSubdiv, TBB, boost,
     zlib and the image libraries) and builds the rest in a few minutes: DXC, Slang, glm, stb,
     miniz, uriparser, tinygltf, tinyobjloader, tinyexr, GLEW, GLFW, cxxopts, gtest.*
   - *The build. Configured with `-Dpxr_ROOT=<OpenUSD> -DCMAKE_PREFIX_PATH=<OpenUSD>`, no
     usdview, tests or applications. Aurora's own libraries build as they are; hdAurora needed
     three changes for OpenUSD 26.08 (`aurora-openusd-26.08.patch`): `IsSupported()` takes the
     create arguments as a schema (`HdRendererCreateArgsSchema`), and
     `HdMeshUtil::ComputeTriangulatedFaceVaryingPrimvar()` returns `HdMeshComputationResult`,
     which is `[[nodiscard]]` (with `/WX`, an error) and isn't a `bool`. No `/WX` change was
     needed.*
   - *The runtime. hdAurora's install has `hdAurora.dll` and its `plugInfo.json`; beside it
     it needs `aurora.dll`, DXC's `dxcompiler.dll` and `dxil.dll`, Slang's DLLs and
     `glew32.dll`. The folder needs a `plugInfo.json` with `"Includes": ["*/resources/"]`, as
     OpenUSD's `plugin/usd` has. Aurora's CMake copies the Windows SDK's `dxcompiler.dll`
     (`DXCOMPILER_DLL`), which wouldn't load here (error 193); the one DXC's release has
     does. A pin would point `DXCOMPILER_DLL` at it.*
   - *In usdcc (test profile, the RTX 4090 here): "Aurora" is among the viewport's
     renderers. Switching to it ends the app: Aurora's `Renderer` creation fails, and
     hdAurora reports that as `TF_FATAL_ERROR("HdAurora fails to create renderer!")`. The
     cause isn't found; the dump has "D3D12Device2 not supported, cannot import device"
     among its strings, which suggests the DirectX backend's device or its interop with
     Storm's GL. That is the next thing to look at, with Aurora's console log
     (`AU_DEV_ALWAYS_DUMP_LOGS`).*
   - *Not done: the pin. Aurora's build isn't plain CMake (its externals script first),
     which needs §4's "small Python hook next to the JSON", not built yet; and there's no
     point publishing a pack that can't start its renderer.*
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
   - *Not done: the overlay's depth and the stretched orthographic view (§3; #8, #7), materials and the Material Editor's Arnold
     nodes, GPU rendering, Linux, and the self-hosted runner.*
8. **Linux** for each pack that supports it (all but Aurora's DirectX backend), as
   `linux` jobs in the same matrix. *The job and the build script's RUNPATH step are
   there since phase 3, with Gatling's pack.*
   - *Adobe's file formats (2026-11-20): the pin lists `linux-x64`. Built and checked
     locally, in WSL (Ubuntu 24.04, GCC 13, a Python 3.13 from uv) against the published
     Linux asset; no workflow has run with it. The source needed no patch, and the build
     takes a minute and a half. Two things were wrong with the first pack, and the check
     found both: the plugins' RUNPATH didn't lead to `lib` (§4, the build script's Linux
     step now covers that), and they were built with the old `std::string` (§4, the pin's
     `-DUSD_FILEFORMATS_ENABLE_CXX11_ABI=ON`). The pack is 34 files, 2.8 MB zipped: five
     plugins in `plugin/usd`, three libraries in `lib`, the licences. Its zip, unzipped
     over a second copy of the asset with the build folder moved away, passes the same
     check as on Windows. zlib is the system's there (`libz.so.1`; the job installs
     `zlib1g-dev`), since OpenUSD brings none on Linux. *Since 2026-11-23 the pin also lists
     `win-arm64` and `linux-arm64`, built on the Arm runners, so arm64 releases have it too
     ([ARM64_RELEASES.md](ARM64_RELEASES.md)).* The RUNPATH step, tried on
     Gatling's pack from phase 3, changes nothing in it. The Windows pack wasn't built
     again locally: the edits rename it, so CI builds it again.*
   - *usdcc with the pack (2026-11-20): `tools/deploy.py` takes packs on Linux (§4), and
     the `linux` jobs of `build.yml` and `release.yml` download them with
     `tools/fetch_usd.py`, which needed no change; `release.yml` requires them and passes
     `--expect-standard-usd-packs` to the smoke test. Checked locally in WSL, with no
     workflow run: `fetch_usd.py --packs-only --zips` put the zip built there over the
     Linux asset; usdcc built against it passes all twelve test programs under Xvfb with
     llvmpipe, and `usd_plugin_pack_file_formats` reads the four samples (4 of 4, none
     skipped). Its deploy has the five plugins in `plugin/usd`, the three libraries in
     `lib`, the licences in `licenses/OpenUSD-plugins/adobe-fileformats/`, the entry in
     `SOURCES.txt` and `usd-plugins/adobe-fileformats.json`; the deploy's own `ldd` check
     passes, and the smoke test with `--expect-standard-usd-packs` loads the five plugins
     from the deploy and opens the samples.*
   - *One thing about that machine, not the pack: `usdcc_python_tests` needs the Python's
     `lib` folder on `LD_LIBRARY_PATH` when the Python is uv's, whose program doesn't link
     its own `libpython`. `actions/setup-python` sets that on a runner.*
   - *Left: a workflow run of all of it, and Gatling's items from phase 3.*
9. **MoonRay on Linux, CPU only** (§3).
   1. A local build in WSL (Ubuntu 24.04, GCC 13) against the published Linux asset: first
      MoonRay's `building/` dependencies without USD and without what the asset has, then
      MoonRay with the asset's `pxr`. This answers §8 items 8 and 9; carry fixes as patches.
   2. `deps/usd-plugins/moonray.json` (`linux-x64` only, `optional`, `"publish": false`), and the pack
      check renders a small scene with HdMoonRay on the CPU.
   3. `moonray_runtime` in usdcc, and MoonRay in the viewport's renderer list and renders
      on Linux.
   4. The licence list read (§5), then `"publish": true`, CI publishes the pack, and only
      then `"set": "standard"`, so `release.yml`'s Linux job requires it and the smoke
      test expects it. Note the pack's size against the
      release's 2 GiB limit; if it makes the standard archive too big, it moves to
      `optional`.

   Done when a Linux release has MoonRay and renders with it in the viewport.
10. **MoonRay on macOS**, after usdcc itself builds and releases on macOS (CI.md §7): an
    OpenUSD asset for `macos-arm64`, a usdcc build and release job on a macOS runner,
    PySide6 for macOS, the app bundle, and signing and notarisation, which need an Apple
    Developer ID. Then the MoonRay pack is one more platform in its pin, and its Metal
    XPU mode needs no extra SDK.
11. **MoonRay's GPU mode on Linux** (CUDA/OptiX), once its licences are read (§5), checked
    on the self-hosted RTX runner.

## 8. Open questions

| # | Question | Assumption until decided |
|---|---|---|
| 1 | Do the plugins build against 26.08 from `typhoon/main` at all? Gatling and Adobe's are proven; arnold-usd names 25.11 as its newest | Try each locally before its phase; carry small fixes as patches in `deps/usd-plugins/` |
| 2 | May usdcc publish a GPL-3.0 Gatling pack as a separate download? | No, until it has had the Blender configuration's review. Then `"publish": true` in `deps/usd-plugins/gatling.json` is all it takes |
| 3 | May a `usdfbx` or hdArnold built against the vendor's SDK be published without the SDK? | No: `local` only. Arnold's `EULA.txt` allows shipping even the core inside an application, on conditions (§5); Autodesk's general Terms of Use and the FBX SDK's licence are still to read |
| 4 | Does hdAurora build against a stock OpenUSD? | **Yes** (2026-11-23), with a three-change patch (§7 phase 6). Whether it renders in usdcc is still open |
| 5 | Switch OpenUSD to oneTBB? | Yes if a local build of usdcc and its tests pass with it, since Cycles and Adobe expect it. Cycles waits for it (§7 phase 5) |
| 6 | Are the Releases page's `deps/...` entries getting too many (RELEASE.md §7 item 2)? | Packs add assets to existing releases, not new releases, so no |
| 7 | Should usdcc set `USD_FILEFORMATS_WRITE_USDPREVIEWSURFACE=0`? It ends the warning at the first file read, but then an imported material is only an OpenPBR MaterialX network, which a delegate without MaterialX can't show | No: keep Adobe's default and the one warning in the log. A setting in Settings > USD when someone wants it |
| 8 | Does HdMoonRay build against 26.08 from `typhoon/main`, with the asset's Boost, TBB, Embree and OpenImageIO? | Unknown; phase 9 step 1 answers it. If Embree or TBB don't fit, MoonRay waits for §6's `--onetbb` and an Embree 4 in the asset |
| 9 | Does HdMoonRay render in-process without Arras? | Assume yes, and leave Arras out of the pack; if not, ship it, with libmicrohttpd as a replaceable shared library (§5) |
| 10 | MoonRay on macOS before usdcc runs there? | No: phase 10 follows the macOS build, not the other way round |

## 9. Later

- **More render delegates.** AMD's Radeon ProRender (`hdRpr`) and Intel's OSPRay
  (`hdOSPRay`, a CPU renderer, so testable on hosted runners) are open source. Whether
  either still follows current OpenUSD has to be checked first. MoonRay, which doesn't
  build on Windows, is planned for Linux and macOS (§3, §7 phases 9–11). Karma, Redshift, Octane and V-Ray's delegate ship inside their own products,
  built for those products' USD, and can't be loaded here.
- **A canary run.** A weekly job that builds the packs against OpenUSD's newest release,
  without publishing, so a plugin that breaks on the next OpenUSD is known before the bump.
- **A symbols asset** for OpenUSD and the packs (`relwithdebinfo`), CI.md §6 item 3.
- **macOS**, with CI.md §7.
