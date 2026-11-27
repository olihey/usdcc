# USD plugins in CI

How usdcc's CI builds OpenUSD's third-party plugins (render delegates, file formats) the
way it already builds OpenUSD itself, and which of them a release ships. This is the
"USD plugins" item of [CI.md](CI.md) §7.

**Status:** phases 1 and 2 are on `master`, and CI has published the OpenUSD asset for
Python 3.13 with the Adobe pack for Windows; its Linux pack, and usdcc's Linux build and
deploy with it, are built and checked locally (phase 8). Phase 3, Gatling, is built and checked locally on Windows
and Linux and renders in usdcc's viewport; CI builds it but doesn't publish it (§5).
Phase 7, Arnold, renders in usdcc's viewport on Windows and macOS (arm64) when Settings >
Renderers > Arnold names an Arnold folder. It is a `local` pack: `olihey/usdcc`'s CI never builds
it, but a private repository's does, for Windows x64, macOS arm64 and Linux x64, and keeps the packs
there, unpublished ([ARNOLD_CI.md](ARNOLD_CI.md)).
Phase 6, Aurora, was tried locally (2026-11-23): hdAurora **builds against our OpenUSD** with a
small patch, no Autodesk fork needed, and registers in usdcc, but its renderer fails to start
there; no pin yet (§7 phase 6). Optional packs are installed in the app, and updated with it
(§10): built and checked on Windows with a test pack, not released. The rest is a plan.

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
| `cycles` | `blender/cycles`, Apache-2.0 | Blender's precompiled libraries (Git LFS, several GB; the pack takes six folders of them). `-DPXR_ROOT=<usd>`; USD 25.11 or newer. With an external USD it builds without OSL and NanoVDB | **Proven:** `deps/usd-plugins/cycles.json` (commit `3eb35f8`, `cycles-usdcc.patch`) builds on CI for Windows x64, Linux x64 and macOS arm64, CPU only, and each pack passes its check, rendering included; about 45 MB zipped. Renders in usdcc's viewport on Windows (§7.1). Not published yet (§5) |
| `aurora` | `Autodesk/Aurora` v25.08-beta, Apache-2.0 | Its `installExternals.py` builds its own USD, an Autodesk fork (`autodesk-forks/USD`, tag `v25.08-Aurora-v25.08`); Slang, the DirectX shader compiler. A DXR GPU to run | **Builds** against our OpenUSD with `aurora-openusd-26.08.patch`, its externals script told not to build what OpenUSD has (`aurora-externals-from-usd.patch`). **Doesn't render yet:** creating the renderer fails in usdcc (§7 phase 6) |
| `arnold` | `Autodesk/arnold-usd`, Apache-2.0 | The Arnold SDK, which is proprietary and behind an Autodesk Account login. Its docs name USD 20.08–25.11 and the dev branch | **Proven:** tag `Arnold-7.5.4.0` with the Arnold SDK 7.5.4.0 builds against our OpenUSD with one patch, in about a minute, and renders in usdcc's viewport (§7 phase 7). 13 files, 5.6 MB; the Arnold core (`ai.dll`, 174 MB) isn't in the pack. Since 2026-11-26 built with the 7.5.4.2 SDKs in `olihey/usdcc-arnold`'s private CI for Windows x64, macOS arm64 and Linux x64, each passing the pack check, which renders a frame with Arnold (`test_data/arnold/check.usda`, the same mean 0.495 on all three); the macOS and Linux packs render in usdcc ([ARNOLD_CI.md](ARNOLD_CI.md)) |
| `moonray` | `OpenMoonRay/openmoonray` (DreamWorks' MoonRay, an ASWF project since May 2026), Apache-2.0 | Linux x64 and macOS on Apple silicon; **no Windows** ([MOONRAY.md](MOONRAY.md)). ISPC, OpenVDB, Lua, JsonCpp, log4cplus, libunwind, Open Image Denoise, and the libraries OpenUSD has too (Boost, TBB, Embree 4, OpenImageIO, OpenEXR, OpenColorIO, OpenSubdiv, MaterialX). OptiX 7.6 headers and CUDA only for its GPU mode | **Proven** on Ubuntu 24.04: `deps/usd-plugins/moonray.json` builds a pack against the published asset that passes its check, rendering included, and renders in usdcc's viewport ([MOONRAY.md](MOONRAY.md) §7.4); CI hasn't built it ([MOONRAY.md](MOONRAY.md) §7.1) |

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
    platform (Git LFS, 7 GB on Windows). The pack's hook (`deps/usd-plugins/cycles.py`)
    checks out only the folders it needs instead (sparse, with their LFS files):
    Open Image Denoise, pugixml, zstd, epoxy's headers, and on Windows zlib and pthreads,
    under 100 MB. With `PXR_ROOT` set, Cycles takes OpenImageIO, OpenVDB, OpenColorIO,
    OpenSubdiv, MaterialX and TBB from the USD install (`USD_OVERRIDE_*` in
    `src/cmake/external_libs.cmake`); the hook points OpenEXR, Imath and Embree there too,
    so one copy of each is loaded. The pack ships Open Image Denoise's base library, core
    and CPU device, nothing else of Blender's; the others are linked in.
  - **TBB.** Blender's libraries are built with oneTBB 2022.3, the asset has oneTBB
    2021.12 (§7 phase 4). Open Image Denoise is the one shipped library that uses it: the
    18 TBB functions its core and CPU device import on Windows are all in the asset's
    `tbb12.dll`.
  - **CPU first.** The CPU renderer needs no SDK, so the pack check renders a frame on a
    hosted runner. The GPU devices each need a vendor SDK at build time and come after,
    one at a time (§7 phase 5): Metal on macOS needs none; CUDA and OptiX need NVIDIA's
    CUDA Toolkit and OptiX headers, which build on a runner without a GPU, though
    compiling kernels for many GPU generations is slow against the 6-hour job limit, so
    the pin names a few; HIP and oneAPI last.
  - **Material Editor nodes.** Cycles has no Sdr plugin; the Material Editor's Cycles
    nodes need the generator in MATERIAL_EDITOR.md §3.4. That is MATERIAL_EDITOR's phase 6,
    not the pack's: the pack has none yet.
- **Aurora.** hdAurora's DirectX backend builds against a stock OpenUSD, not only
  Autodesk's fork, with a small patch (§7 phase 6, §8). Whether this pack exists now depends
  on whether its renderer starts in usdcc, which it doesn't yet.
- **Arnold.** Its Sdr plugin starts the Arnold core at discovery (`AiBegin`), so the pack
  is off unless the user turns it on. (Here that took under half a second, for 160 Arnold
  shader nodes, but it also loads Arnold's own plugins, its USD procedural among them.)
  - **Where it is.** The pack installs into `plugin/arnold` (`install_prefix`, with
    `-DPREFIX_PLUGINS=.`), beside OpenUSD's `plugin/usd`, whose `plugInfo.json` only
    includes its own subfolders. Built with `--install`, or copied over the OpenUSD
    install, it is in `<OpenUSD>/plugin/arnold`; a deploy would have it in
    `<deploy>/plugin/arnold`, but `tools/deploy.py` leaves `local` packs out (§5).
  - **Turning it on.** Settings > Renderers > Arnold > Arnold folder (`USD/arnoldFolder`),
    from the next start: an Arnold SDK or install, the folder with `bin/ai.dll`
    (`bin/libai.dylib` on macOS, `bin/libai.so` on Linux). Once
    Arnold is on, its 54 render settings join it in that group.
    `usdcc::usd::enableArnold()` (`arnold_runtime.h`), from
    `applyUsdEnvironmentSettings()`, then:
    1. finds the pack in `plugin/arnold` next to the folder OpenUSD's libraries were
       loaded from, and reads which Arnold it was built for from
       `usd-plugins/arnold.json` (the pin's `version`);
    2. loads `bin/ai.dll` by its full path (`LOAD_WITH_ALTERED_SEARCH_PATH`), so its own
       DLLs come from its own folder and nothing goes on `PATH`. That folder has Qt 6
       DLLs of its own (3ds Max's copy and the SDK both do). On macOS and Linux
       `bin/libai.dylib` or `bin/libai.so` with `dlopen(RTLD_NOW | RTLD_GLOBAL)`: the plugins
       ask for `@rpath/libai.dylib` or `libai.so`, the install name or soname of the one
       loaded, so they get it, and its own libraries (licensing) come from its folder
       through its `@loader_path` or `$ORIGIN` rpath. On Linux that isn't enough:
       Arnold's `libai.so` has no SONAME, and a library loaded by its path satisfies no
       `NEEDED libai.so`, so the plugins wouldn't load. The pin's `preloaded` takes
       `libai.so` out of their `NEEDED` (`build_usd_plugin.py`, `patchelf`), and their
       Arnold symbols come from the core loaded globally first. And `--no-as-needed`
       (the pin's Linux-only `cmake` entry): Ubuntu's linker drops a library nothing
       before it on the link line uses, and arnold-usd links `usdImaging` before the
       render delegate's static library, whose `reader.cpp` subclasses
       `UsdImagingDelegate`, so `hdArnold.so` lacked `NEEDED libusd_usdImaging.so` and its
       type info was undefined. Linux is built by
       [ARNOLD_CI.md](ARNOLD_CI.md)'s workflow, and renders in usdcc (in a container, §7 there);
    3. asks the core its version (`AiGetVersion`). The plugins need the Arnold they were
       built for or a later one of the same 7.x: built against 7.5.4, they don't load
       with 3ds Max 2027's 7.5.2 ("The specified procedure could not be found"). Any
       other version unloads the core again, and a warning in the log says why;
    4. registers the pack's plugins (`PlugRegistry::RegisterPlugins()`). Not through
       `PXR_PLUGINPATH_NAME`: OpenUSD reads that in a static constructor of `usd_plug.dll`,
       before `main()`, so setting it there is too late. Child processes don't get them
       either, which they couldn't use without the core.
  - **macOS** (2026-11-26). The SDK's `libai.dylib` is universal (arm64 and x86_64). The
    pin lists `macos-arm64`, and its SDK `files` and the check's `preload` are by
    platform. Built with the Arnold SDK 7.5.4.2 for macOS:
    `tools/build_usd_plugin.py arnold --usd-install <OpenUSD> --sdk arnold=<SDK> --install`,
    13 files, the patch unchanged, rpaths only to OpenUSD (`@loader_path/../../lib`).
    `tests/renderer_viewport_check.py` with `RV_RENDERER=HdArnoldRendererPlugin`: all six
    steps converge (first frame 2.8 s), the checker texture, the red material and the
    display colours show, with the watermark. **Not in `~/Downloads`, `~/Desktop` or
    `~/Documents`:** macOS asks before a program reads those, and usdcc then waits in
    `dlopen` (or Arnold in its licensing) for an answer that may never be seen. Put the
    SDK elsewhere, as `~/deps/Arnold-7.5.4.2-darwin`. Arnold's sign-in licensing logs that
    it can't find Autodesk Identity Manager, and renders with the watermark.
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
- **MoonRay.** [MOONRAY.md](MOONRAY.md) is the newer plan (size, nodes, MaterialX) and wins where
  the two differ. A production CPU path tracer with a hybrid GPU mode (XPU: CUDA/OptiX on
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
- `hook`: a Python file next to the JSON whose `build(ctx)` replaces the configure, build
  and install, for a build that isn't plain CMake (`cycles.py`, `moonray.py`; Aurora's
  pin would need one, for its externals script).

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
self-hosted runner) and is kept as a workflow artifact, never published. Arnold's is built in a
private repository, `olihey/usdcc-arnold`, that holds the trimmed SDKs and runs the build with
these tools and this repository's pin, so the public one never touches the SDK; its packs are
release assets there, named as here ([ARNOLD_CI.md](ARNOLD_CI.md)).

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
| `standard` | In the usdcc zip | `adobe-fileformats` (glTF, OBJ, PLY, STL, SPZ), `cycles` (`optional` and unpublished: its licences are read and the GPL atomics replaced; publishing waits on the rebuilt packs, §5), `aurora` (no pin: its renderer doesn't start yet, §7 phase 6) |
| `optional` | Its own archive on the usdcc release, unpacked over the usdcc folder by the user; on macOS a disk image of usdcc with it | `moonray` (Linux x64: `usdcc-<version>-linux-x64-moonray.tar.xz`; macOS: `usdcc-<version>-macos-arm64-moonray.dmg`), `gatling` (unpublished) |

How an optional pack ships (Linux, and macOS in the last point; no Windows one is published yet):

- `deps-usd-plugins.yml` publishes it beside the OpenUSD asset like any pack, once its pin
  says `"publish": true`.
- `release.yml`'s Linux job fetches it too (`tools/fetch_usd.py --optional`, required as the
  standard ones are), and `tools/deploy.py --optional-packs DIR` deploys it with the rest,
  checks it with them (every library loads from the deploy or the system), then moves its
  files out to `DIR/<pack>/<deploy's name>/`, laid out as the deploy is. Its files under
  `plugin/` keep the RUNPATHs the pack's build gave them, which are relative to an OpenUSD
  install, laid out as the deploy is. The deploy's `licenses/SOURCES.txt` doesn't name it; the
  pack carries its own in `licenses/OpenUSD-plugins/<pack>/`.
- After the usdcc archive is made, the job unpacks the pack over the deploy, runs the smoke
  test again (its plugins load from the deploy, its renderers are listed), and attaches
  `usdcc-<version>-linux-<arch>-<pack>.tar.xz`. Its top folder is the deploy's name, so
  `tar -xf` where usdcc was unpacked puts it in place.
- On macOS `--optional-packs DIR` makes `DIR/<pack>/<deploy's name>/usdcc.app`, a copy of the
  deployed bundle with the pack in it, checked and signed as a whole: files unpacked over a signed
  bundle break its signature. The job makes a disk image of each, and starts usdcc from it.
| `local` | Built on a machine that has the SDK, or a private repository's CI (Arnold, [ARNOLD_CI.md](ARNOLD_CI.md)); never published | `arnold`, `usdfbx`, `usdsbsar` |

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
  general Terms of Use, read on 2026-11-26 with the rest ([ARNOLD_CI.md](ARNOLD_CI.md) §6).
  usdcc's BSD-3-Clause doesn't meet the "no less protective" condition by itself, so the core
  is never shipped; the plugins alone could be, as Gaffer ships its own, once Autodesk confirms
  it in writing. Until then the pack stays `local`.
- **Standard packs are Apache-2.0**, but each brings libraries of its own (Cycles: OpenVDB,
  Open Image Denoise; Aurora: Slang, the DirectX shader compiler's `dxil.dll`). Step 5 of
  the build collects their licences, and a pack isn't `standard` until that list is read.
- **Cycles is Apache-2.0**, kept apart from Blender's GPL code, so it can go in the
  standard zip, which stays BSD-3-Clause. Apache-2.0 asks for its `LICENSE` and `NOTICE`
  in the release, and for files our patches change to say so. What it brings:
  - Cycles' own `third_party/`: small permissive libraries (Apache, MIT, zlib, BSD);
    licence texts only.
  - Blender's precompiled libraries: a mix, with GPL ones such as FFmpeg among them.
    **Only what hdCycles links is shipped**, and Cycles doesn't use FFmpeg. The pin's
    check lists the libraries the pack may have outside `plugin/` (`"libraries"`), and
    `tools/ci/check_usd_plugin.py` fails it on any other, so nothing GPL comes in by
    accident: today the three of Open Image Denoise. The ones linked in (pugixml, zstd,
    zlib and pthreads on Windows) and epoxy, whose headers are used, have their licence
    texts in the pack, downloaded by the hook at the versions Blender's `deps.md` names,
    since Blender's library folders carry none.
  - Open Image Denoise, OpenVDB, Embree: Apache-2.0; licence texts only.
  - The GPU devices, each read before it ships: compiled CUDA kernels (NVIDIA's CUDA
    EULA, which generally allows redistributing compiled kernels), OptiX (its SDK
    licence; the headers are build-time and the runtime is in the driver), HIP (AMD,
    mostly MIT), oneAPI (Intel's runtime licence).

  If the GPU kernels make the standard archive too big, they become a pack of their own
  in the `optional` set, and the standard one stays CPU only.

  **The list read (2026-11-27)**, from the three CPU packs `deps-usd-plugins.yml` built
  for the pin (run 45945194968, hash `231aea80`, `091c8c2e` on macOS) and the Cycles
  source at `3eb35f8`. Three problems, all fixed in `cycles-usdcc.patch` and the hook
  (2026-11-27); Cycles' pin keeps `"set": "optional"` with `"publish": false` until the
  packs built with the fix have been checked:
  1. **GPL code compiled in (the blocker).** `src/util/atomic.h` included Blender's
     `third_party/atomic/atomic_ops.h` for every CPU build, and those headers
     (`atomic_ops.h`, `intern/atomic_ops_{ext,unix,msvc,utils}.h`) are jemalloc's
     BSD-2-Clause code with Blender Foundation's 2016 changes under a GPL-2.0-or-later
     licence block. Their inline functions ended up in `hdCycles` on all three platforms,
     so the pack wasn't the Apache-2.0-only work the standard zip needs. Blender itself
     still ships them this way (`intern/atomic`, v4.5).
     **Fix:** the patch has `util/atomic.h` implement the CPU atomics Cycles calls, with
     the same names and signatures and sequentially consistent ordering, written fresh:
     fetch-and-add, -sub and -or, add-and-fetch, compare-and-swap for `uint32_t`,
     `unsigned int` and `size_t`, a max update, and float compare-and-swap and add by
     the float's bits. They use `__atomic_*` built-ins with GCC and Clang, and
     `std::atomic_ref` with MSVC (Cycles is C++20). The patch also takes
     `third_party/atomic` off the include path, and the hook deletes the folder before
     configuring, so a later Cycles that includes it again fails to build. The GPU
     branches of `util/atomic.h` are unchanged. A threaded test of every operation
     (8 threads, exact totals) passes with Apple clang, under ThreadSanitizer too.
  2. **Our patch mislabelled OpenUSD code.** `hydra/implicit_surfaces.{h,cpp}`, adapted
     from hdEmbree, said `SPDX-License-Identifier: Apache-2.0`. OpenUSD's file is under
     OpenUSD's own licence, the Tomorrow Open Source Technology License 1.0, which usdcc
     ships anyway. **Fix:** the headers now name that licence, as OpenUSD's own files do.
  3. **A changed file without a notice.** The patch changes `hydra/light.cpp` (a dome
     light's strength), which Apache-2.0 §4(b) asks to be marked. **Fix:** its header says
     so. The CMake files the patch changes aren't shipped.

  Fine as it is:
  - **What's linked at run time:** only OpenUSD's asset (its USD libraries, OpenImageIO,
    OpenColorIO, OpenVDB, OpenSubdiv, Embree, TBB, MaterialX, Python), the system, and
    the pack's three Open Image Denoise libraries (checked with `otool -L` and
    `objdump -p`). Nothing from Blender's libraries is loaded besides those three.
  - **Statically linked, with their licence texts in the pack:** pugixml (MIT), zstd
    (BSD-3-Clause; it's dual-licensed with GPL-2.0, and we take the BSD option), zlib
    on Linux and Windows (zlib; macOS uses the system's), pthreads4w on Windows
    (Apache-2.0; its `NOTICE` ships), sse2neon on macOS (MIT), and epoxy's headers (MIT).
  - **Open Image Denoise 2.5.0:** Apache-2.0. Its third-party list (oneTBB, PyTorch,
    Catch2, CUTLASS) and oneTBB's (ITT Notify, hwloc and others) are all permissive,
    and both lists ship.
  - **Cycles' own source:** 817 files Apache-2.0, the rest BSD-3-Clause, MIT or Zlib,
    with their texts in `cycles-notices/`. The only other GPL file,
    `src/cmake/dependency_targets.cmake`, is build-only. `cuew`, `hipew` and
    `libc_compat` are Apache-2.0. Cycles has no `NOTICE` file to carry.
  - **No GPU device** is built, so none of the GPU licences apply yet.
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

  The list has been read ([MOONRAY.md](MOONRAY.md) §3.1): the pin says `"set": "optional"`
  with `"publish": true`, a download of its own at about 214 MB, against usdcc's 154 MB
  (decided 2026-11-24).

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
   *Tried locally on Windows (2026-11-23); the pin has them, and no workflow has run.*
   - *The flags: `--onetbb`, `--openvdb`, `--alembic` and `--draco`. `tools/build_usd.py`
     built the pinned commit with them, with the pin's patches and no new one, into a
     folder of its own: oneTBB 2021.12 (`tbb12.dll`), OpenVDB 10.1, Alembic 1.8.5 (Ogawa
     only, no `--hdf5`), Draco 1.5.6, beside what the asset had. `usdAbc`, `usdDraco` and
     `hioOpenVDB` register; `usdcat` wrote the test scene as `.abc` and read it back.*
   - *usdcc against it: a clean build, and all twelve test programs pass, the Blender mesh
     tools' among them. oneTBB changes the ABI of everything built on OpenUSD, which is why
     every pack is built again after it.*
   - *Changed with it: `tools/ci/usd_asset.py` no longer adds a second `--onetbb` on Windows
     ARM64, and `tools/blender_distribution.py` knows oneTBB's `tbb12.dll` and its
     `oneTBB-<version>` source folder for the notices.*
   - *Not done: Linux (the same flags, in `deps-usd.yml`'s Linux job), the workflow run, and
     the packs built again. Until `deps-usd.yml` has published the new asset, `build.yml`
     fails, saying to run it: so the order is merge, run `deps-usd.yml`, then
     `deps-usd-plugins.yml`.*
5. **Cycles**, after phase 4 has given the asset oneTBB and OpenVDB (§3, §6).
   1. A local build on Windows against that asset, CPU only: which Embree it uses, and
      the list of what hdCycles loads from Blender's libraries.
      *Done (2026-11-23), against phase 4's OpenUSD built locally:*
      - *The build. `blender/cycles` at `3eb35f859`, with Blender's precompiled
        `lib/windows_x64` already fetched (7.1 GB), configured with `-DPXR_ROOT=<OpenUSD>`
        and the GPU devices off: OSL and NanoVDB switch themselves off ("not part of the USD
        package"). One thing was needed: OpenVDB comes from OpenUSD now, and its headers
        include boost, which Cycles doesn't put on the include path; the build adds
        OpenUSD's `include/boost-1_86` (here through MSVC's `CL` variable, since Cycles sets
        `CMAKE_CXX_FLAGS` itself). A pin would pass it as a CMake argument or a patch.*
      - *What hdCycles imports (`dumpbin`): TBB (`tbb12.dll`), OpenImageIO, OpenColorIO,
        OpenVDB and OpenUSD's own libraries, all in the asset; from Blender's libraries only
        `OpenImageDenoise.dll`. What that loads in turn at run time (the install has its
        `_core` and one `_device_*` DLL per device) isn't checked.*
      - *Embree: not compiled in. `WITH_CYCLES_EMBREE` is on and Blender's `embree4.lib` is on
        hdCycles' link line, but none of the generated projects defines `WITH_EMBREE`,
        hdCycles doesn't import `embree4.dll`, and the render below says "Using BVH2 layout".
        So this build can't bring a second `embree4.dll` beside OpenUSD's 4.3.3. Why CMake
        leaves it out, and what it costs in speed, isn't known.*
      - *Rendering, outside usdcc: `cycles.exe` from the same build reads USD with its own
        `HdCyclesDelegate` (`src/hydra/file_reader.cpp`), though with a chain of scene indices
        of its own, not the viewport's. On the CPU, at 480×320 with 32 samples, it traced the
        test scene in 0.1 s; that picture was all white. With dimmer lights (64 samples) the
        ground mesh is there, but **the Sphere and the Cube aren't seen**. Why isn't known:
        Cycles counted three meshes, and its source has nothing about implicit surfaces,
        so the gap Gatling's patch fills is one guess.*
      - *In usdcc: see §7.1, which supersedes the rest of this item.*
      - *Install: 126 MB, with GPU kernel sources and runtime DLLs a pack doesn't need.*
   2. `deps/usd-plugins/cycles.json` (`win64` and `linux-x64`, `optional`,
      `"publish": false`). The pack check renders a small scene on the CPU and fails on a
      library the pin doesn't list (§5).
      *Done (2026-11-26), for `win64`, `linux-x64` and `macos-arm64`:*
      - *The hook, `deps/usd-plugins/cycles.py`, checks out six folders of Blender's
        libraries for the platform at the commit the Cycles checkout names (sparse, Git LFS
        for those only), builds hdCycles against the OpenUSD asset (its OpenEXR, Imath and
        Embree too) with the GPU devices, OSL and Alembic off, and puts `hdCycles` with its
        resources in `plugin/usd` and Open Image Denoise's base library, core and CPU device
        in `bin/` (Windows) or `plugin/usd/hdCycles/lib` (not `lib/`, which in an installed
        pack is a link to usdcc's own libraries, §10.2). Linux binaries get RUNPATHs relative
        to `$ORIGIN`.*
      - *What each platform needed: on Windows OpenUSD's `include/boost-1_86` through MSVC's
        `CL` (Cycles sets `CMAKE_CXX_FLAGS` itself); on Linux Blender's zlib, since Cycles
        hides the system's headers once it uses Blender's libraries, and **GCC 14**: Cycles
        is C++20, and GCC 13 refuses a constructor in OpenUSD's `retainedDataSource.h` there
        (the job installs `g++-14`; its libstdc++ is Ubuntu 24.04's own); on macOS
        `sse2neon` from Blender's libraries.*
      - *projects.blender.org refused the Linux runner's fetch (403) on two runs of five and
        served it on the others; the hook tries four times, a minute apart.*
      - *The check (`check.usda`: a mesh, a sphere and a cube, which only the patch's scene
        index draws, two lights, a UsdPreviewSurface) renders on each in 2 to 3 s, mean
        0.519 on all three. 22 files on Windows and Linux, 20 on macOS; 44 to 46 MB zipped,
        47 MB of it Open Image Denoise's core. `"libraries"` in the pin's check lists Open
        Image Denoise's three, and `check_usd_plugin.py` fails on any other library.*
      - *Windows arm64 isn't in the pin: Cycles' `util/half.h` uses NEON intrinsics in a
        form MSVC for ARM64 doesn't compile (Blender builds that platform with clang-cl).
        Linux arm64: Blender has no libraries for it.*
      - *In usdcc's viewport on Windows, from the pack (not the build): the same as §7.1,
        each step converged in about 2 s. Linux and macOS in the app aren't checked.
        Implicit shapes are tessellated coarsely (the scene index's defaults).*
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
     renderers. Switching to it ends the app. In the dump's stack, `aurora.dll`, called from
     hdAurora inside `CreateDelegate()`, reports a failure to hdAurora's log callback, which
     makes it a `TF_FATAL_ERROR`. **The message and the cause aren't known:** neither DLL had
     symbols. The next thing to look at is that message, which the callback also writes to
     Aurora's console log (`AU_DEV_ALWAYS_DUMP_LOGS`).*
   - *Not done: the pin. Aurora's build isn't plain CMake (its externals script first), so
     it needs a hook (§4's `hook`, as Cycles and MoonRay have); but there's no point
     publishing a pack that can't start its renderer, so the crash comes first.*
7. **Arnold**, as a `local` pack.
   *Built, and checked locally on Windows, then macOS. Not built by this repository's CI
   (`local`); since 2026-11-26 built and checked for Windows x64, macOS arm64 and Linux x64 by
   a private repository's ([ARNOLD_CI.md](ARNOLD_CI.md)).*
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
     ([ARM64_RELEASES.md](ARM64_RELEASES.md)). Its one patch,
     `adobe-fileformats-arm64.patch`: Adobe's `cmake/compiler_config.cmake` gives GCC
     `-msse3 -m64` on every machine, which GCC for arm64 doesn't know; the patch keeps
     them to x86. Not upstream.* The RUNPATH step, tried on
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
   4. The licence list read (§5), then `"publish": true`, CI publishes the pack, and
      `release.yml`'s Linux job attaches it as a download of its own (`optional`, §5).

   Done when a Linux release has MoonRay and renders with it in the viewport.
   *Steps 1 to 4 done ([MOONRAY.md](MOONRAY.md) §7); left: the first release with it.*
10. **MoonRay on macOS**, after usdcc itself builds and releases on macOS (CI.md §7): an
    OpenUSD asset for `macos-arm64`, a usdcc build and release job on a macOS runner,
    PySide6 for macOS, the app bundle, and signing and notarisation, which need an Apple
    Developer ID. Then the MoonRay pack is one more platform in its pin, and its Metal
    XPU mode needs no extra SDK.
11. **MoonRay's GPU mode on Linux** (CUDA/OptiX), once its licences are read (§5), checked
    on the self-hosted RTX runner.


### 7.1 Cycles in usdcc's viewport (2026-11-26)

Checked on Windows with `tests/renderer_viewport_check.py`, which writes a scene (a textured ground
mesh; a sphere, cube, cylinder and cone, which are implicit shapes; a distant and a dome light; a red
UsdPreviewSurface; display colors) and takes the viewport's own picture after each step with
`Viewport.grab()`, new for this. Cycles from `blender/cycles` at `3eb35f859` with
`patches/cycles-usdcc.patch`, CPU only, against the local OpenUSD of the pin.

| Step | Cycles | Embree (the same check) |
|---|---|---|
| First frame, converged | 2.1 s; every shape, the texture, both lights' shadows, the red material | 9.7 s |
| Camera moved, an edit (a color, a translate), the window resized | each redrawn, 0.9 to 2.3 s | 4 to 11 s |
| To Storm and back | yes | yes |
| The app quits | yes, exit code 0 | yes |

What the patch changes in Cycles:

- **Implicit shapes.** hdCycles draws meshes only, and nothing turned USD's sphere, cube, cylinder,
  cone, capsule and plane into meshes, so they weren't drawn (cycles.exe has its own chain of scene
  indices). `src/hydra/implicit_surfaces.cpp` registers OpenUSD's `HdsiImplicitSurfaceSceneIndex` for
  the Cycles renderer, as hdEmbree and the Gatling patch do.
- **The white picture.** hdCycles multiplied every light but the distant one by π, "from intensity to
  radiant flux". A dome light is Cycles' background, whose strength is radiance, as UsdLux's
  intensity is: a sky of 0.4 was 1.26, white. The dome light isn't multiplied now, and the sky has
  Embree's grey. The distant light's ×4 ("approximately matches Karma") and the area lights' ×π are
  upstream's choices and stay.
- **Embree.** Cycles' standalone build tests `WITH_EMBREE`, which only Blender's build sets; its own
  option is `WITH_CYCLES_EMBREE`. So Embree was never compiled in and Cycles used its BVH2. The patch
  tests the right one, and lets `-DEMBREE_ROOT_DIR` name another Embree on Windows, as the other
  platforms could. Cycles is built against **OpenUSD's Embree 4.3.3**, which its code supports (it
  checks `RTC_VERSION`), so one `embree4.dll` serves both. CMake searches Cycles' own library folders
  before that hint, so the build names `EMBREE_INCLUDE_DIR` and `EMBREE_EMBREE4_LIBRARY` too.

**Not checked:** textures other than a PNG, MaterialX, Cycles' own nodes (the Material Editor has none
yet, MATERIAL_EDITOR.md §3.4), volumes, Linux and macOS in the app (their packs pass their checks, §7 phase 5), the GPU.

## 8. Open questions

| # | Question | Assumption until decided |
|---|---|---|
| 1 | Do the plugins build against 26.08 from `typhoon/main` at all? Gatling and Adobe's are proven; arnold-usd names 25.11 as its newest | Try each locally before its phase; carry small fixes as patches in `deps/usd-plugins/` |
| 2 | May usdcc publish a GPL-3.0 Gatling pack as a separate download? | No, until it has had the Blender configuration's review. Then `"publish": true` in `deps/usd-plugins/gatling.json` is all it takes |
| 3 | May a `usdfbx` or hdArnold built against the vendor's SDK be published without the SDK? | No: `local` only. For Arnold, read on 2026-11-26 ([ARNOLD_CI.md](ARNOLD_CI.md) §6): its EULA and Autodesk's Terms of Use don't forbid the plugins without the core, Gaffer publishes its own so, and only the Terms' §13(b) is worth Autodesk's written word before publishing. The FBX SDK's licence is still to read |
| 4 | Does hdAurora build against a stock OpenUSD? | **Yes** (2026-11-23), with a three-change patch (§7 phase 6). Whether it renders in usdcc is still open |
| 5 | Switch OpenUSD to oneTBB? | **Yes** (2026-11-23): usdcc built against it and all its tests pass (§7 phase 4) |
| 6 | Are the Releases page's `deps/...` entries getting too many (RELEASE.md §7 item 2)? | Packs add assets to existing releases, not new releases, so no |
| 7 | Should usdcc set `USD_FILEFORMATS_WRITE_USDPREVIEWSURFACE=0`? It ends the warning at the first file read, but then an imported material is only an OpenPBR MaterialX network, which a delegate without MaterialX can't show | No: keep Adobe's default and the one warning in the log. A setting in Settings > USD when someone wants it |
| 8 | Does HdMoonRay build against 26.08 from `typhoon/main`, with the asset's Boost, TBB, Embree and OpenImageIO? | MoonRay itself does, oneTBB, OpenVDB and Embree included ([MOONRAY.md](MOONRAY.md) §7.1); HdMoonRay too, with a patch (§7.2) |
| 9 | Does HdMoonRay render in-process without Arras? | Yes by its source: `hd_moonray_debug` does, `hd_moonray` doesn't. Both render in usdcc's viewport; Arras is the safer one ([MOONRAY.md](MOONRAY.md) §7.2, §7.3) |
| 10 | MoonRay on macOS before usdcc runs there? | No: phase 10 follows the macOS build, not the other way round |

## 9. Later

- **Optional packs downloaded in the app:** built, §10. It began here as a decision of
  2026-11-26, with the trials that section now has.
- **More render delegates.** AMD's Radeon ProRender (`hdRpr`) and Intel's OSPRay
  (`hdOSPRay`, a CPU renderer, so testable on hosted runners) are open source. Whether
  either still follows current OpenUSD has to be checked first. MoonRay, which doesn't
  build on Windows, is planned for Linux and macOS (§3, §7 phases 9–11). Karma, Redshift, Octane and V-Ray's delegate ship inside their own products,
  built for those products' USD, and can't be loaded here.
- **A canary run.** A weekly job that builds the packs against OpenUSD's newest release,
  without publishing, so a plugin that breaks on the next OpenUSD is known before the bump.
- **A symbols asset** for OpenUSD and the packs (`relwithdebinfo`), CI.md §6 item 3.
- **macOS**, with CI.md §7.

## 10. Optional packs in the app

An optional pack stays a separate download (§5), but usdcc fetches the one for its own version
itself, in **View > Plugins > Packs**, in place of an archive the user unpacks over it, and of
the second disk image on macOS. Not through a plugin registry:
[PLUGIN_REGISTRY.md](PLUGIN_REGISTRY.md) is parked.

**Status (2026-11-26):** built, and checked on Windows with a test pack, in the app and by the
tests of §10.6; nothing is released with it. On Linux and macOS none of it has run yet: its
parts were tried by hand before (§10.6). The release workflow's new steps haven't run either;
a dry run is next. The archives to unpack over usdcc and the second disk image (§5) stay
until a release has shown that this works, and no Windows optional pack is published yet
(Cycles and Gatling say `"publish": false`), so Windows users see an empty list until one is.

### 10.1 Decisions

- **A pack is plugins of any kind,** not only a renderer. The first ones are render delegates
  (MoonRay; Cycles and Gatling after it) because those are the big ones, but the same download
  serves whatever is too large or too special to ship with usdcc: file formats, asset resolvers,
  schemas, and usdcc's own plugins, in Python or C++ ([PLUGINS.md](PLUGINS.md)).
- **A pack is for exactly one usdcc version.** It is built against one OpenUSD, and one with a
  C++ plugin of usdcc's loads into one usdcc build only (PLUGINS.md §3.2). There is no "needs
  update" state: a version has its packs or it doesn't.
- **Packs live in the user's data folder, on every platform,** one folder per usdcc version
  (decided 2026-11-26; first it was "unpacked over usdcc's own folder" where that could be done).
  One mechanism in place of two; the app's folder is never written, so a signed macOS bundle, an
  AppImage and an install the user can't write all work; and the next version's packs can be
  put beside this one's before an update, which a folder Velopack replaces can't hold.
- **Packs update with the app** (decided 2026-11-26): the packs the user chose are fetched for
  the new version before the restart (§10.5).
- **A chosen pack the new version doesn't have is asked about** before the update, with Update
  Anyway and Later. usdcc doesn't hold an update back for a pack, nor go ahead without saying.
- **In View > Plugins,** where an installed pack's plugins show anyway, not in a dialog of its own.
- **Not an update channel per set of packs** (usdcc with the pack, as the second disk image is):
  with several packs it is a channel per combination, or one that gives everyone everything.

### 10.2 The store

`<user data>/packs` (`%LOCALAPPDATA%\usdcc_config\packs` on Windows), `PackStore` in
[pack_store.h](../src/cpp/scripting/include/usdcc/scripting/pack_store.h):

```text
packs/
  wanted.json                      the packs the user installed ("packs") and removed
                                   ("removed"); kept over every update
  26.12.1-linux-x64/               one per usdcc version and platform
    moonray/                       a usdcc plugin folder, laid out as an install
      usdcc-plugin.json            id org.usdcc.pack.moonray, "usd": ["plugin/usd"]
      pack.json                    what was installed, and its content hash
      plugin/usd/plugInfo.json     includes */resources/
      plugin/usd/<its folders>/    as the pack is built
      bin/                         Windows only: the pack's own DLLs
      lib -> <usdcc>/lib           Linux and macOS: a link, made again at every start
      licenses/  .in-use
  26.12.2-linux-x64/moonray/       fetched before the update to 26.12.2
  .downloads/                      archives, and packs being unpacked
```

- **Found as a plugin.** This version's folder is one of the plugin manager's, after the user's
  own and the added folders and before the shipped one, so a pack's USD folder is registered
  before the first stage, in usdcc and in usdcc_node, with no loader of its own. View > Plugins
  lists it among the plugins, with what it added. A development build uses the folder of the
  release it follows (`26.11.10` for `26.11.10+8.75b11df`).
- **Made loadable at the start,** before the plugins are scanned (`PackStore::prepare()`).
  OpenUSD loads a plugin's library by its full path and nothing more, so what that library
  needs has to be found:
  - *Windows:* the pack's `bin` goes on the process's `PATH` (tried, §10.6), in front of what
    the machine has there, so that a DLL of the same name somewhere on the user's `PATH` can't
    take a pack's place. usdcc's own DLLs are found before `PATH`, in its own folder.
  - *Linux and macOS:* the pack's `lib` is a link to usdcc's own `lib`, where the files' RUNPATHs
    lead as they did in the install the pack was built in. Made again at each start, as an
    AppImage is mounted somewhere else each time. While another usdcc uses the pack the link
    stays where that one loads from.
  - A pack that can't be made loadable, or was made for another version, is listed as Not
    loaded with the reason, and nothing of it is registered. On macOS a pack loaded without
    its link doesn't fail, a render with it never ends (§10.6), so this is checked, not assumed.
- **Never changed in place.** An install unpacks in `.downloads`, writes `pack.json` last and
  renames the finished folder into place; a failed or cancelled one leaves the store as it was.
  Each running usdcc holds `.in-use` open in the packs it loaded
  ([in_use_marker.h](../src/cpp/scripting/include/usdcc/scripting/in_use_marker.h), the rule of
  PYTHON_PACKAGES.md §3), and nothing in use is deleted. Installing and removing take effect at
  the next start.
- **Other versions' folders** are deleted at a start once this version is the newer one and no
  usdcc uses them. A newer version's stay: fetched for an update that isn't installed yet.

### 10.3 What a release publishes

Per platform, as files of the release itself, beside the usdcc archives:

- `usdcc-<version>-<platform>-pack-<name>.zip` on Windows, `.tar.xz` elsewhere (links and
  executable bits), whose one top folder is the pack's folder as above, without `lib`.
- `usdcc-<version>-<platform>-packs.json`, the index: per pack its name, title, description,
  version, licence, homepage, renderers, the archive's name, size and SHA-256, the installed size,
  and `content`, a SHA-256 over the pack's file names and each file's hash. Two releases' packs
  with one `content` are the same files. A platform without packs publishes an empty list; a
  release without an index (an older one, or a failed job) counts as having no packs.
- usdcc reads both by their plain download addresses
  (`.../releases/download/<tag>/<file>`), not GitHub's API, so its hourly limit doesn't count.

How they are made:

- `tools/deploy.py --pack-folders DIR` writes each optional pack as the folder usdcc installs,
  `DIR/<pack>/`: from the files `--optional-packs` deployed and checked (Linux; on macOS from the
  signed copy of the bundle, so each binary has its own signature), and on Windows from the
  pack itself. Title and description come from [deps/packs.json](../deps/packs.json), not the
  pins: an edit to a pin, or to `usd_plugin_asset.py`, renames and rebuilds packs.
- **On Linux and macOS a pack may have nothing in `lib/`,** where the installed pack has its link:
  it keeps its own libraries under `plugin/usd/`, as MoonRay's does. `deploy.py` fails on one
  that doesn't. **Cycles' pack doesn't yet** on those two (Open Image Denoise is in `lib/`, §7
  phase 5): its hook has to change before it is published there.
- `tools/ci/pack_index.py --packs DIR --version V --out DIR` archives them and writes the index.
- `tools/ci/check_pack_install.py --deploy D --packs DIR` installs each with the deploy's own
  usdcc (`--install-packs`, into a test profile), starts it and checks that OpenUSD registered
  the pack from the data folder, that the plugins its pin's `check` names load and its
  renderers are listed, and removes it again. `release.yml` runs the three in each platform's job.
- `tools/make_test_pack.py <folder> --version V ...` makes a tiny pack without binaries (a
  codeless schema, the prim type `UsdccHelloThing`) with archives and indexes for the versions
  given, to try all of this without a release.

### 10.4 In the app

`AppPacks` ([app_packs.h](../src/cpp/app/app_packs.h)), the feature `USDCC_BUILD_PACKS`
([BUILD_OPTIONS.md](BUILD_OPTIONS.md)). Off, usdcc downloads nothing and has no Packs section;
a pack that is in the store all the same is still loaded.

- **View > Plugins > Packs** lists what this version's release offers and what is installed:
  title, size and state, and under each its description, renderers, version, licence, and why
  an install failed or a pack isn't loaded. For the pack selected:

  | State | Buttons |
  |---|---|
  | Not installed | Install |
  | Downloading 42%, Unpacking... | Cancel |
  | Restart to use | Restart Now, Remove... |
  | Installed | Remove... |
  | Removed at the next start | Restart Now, Keep |
  | Not loaded (why) | Remove... |
  | Not for this version (chosen, and this release has none) | Remove... |

- **The list is read when the panel is first opened,** and with its refresh button: nothing asks
  the network for a panel nobody looked at.
- **Install:** a check that the drive has room, the download with Qt Network and the system's
  proxy, its size and SHA-256 against the index, unpacking in a process of usdcc's own Python
  (`zipfile` / `tarfile` with the `data` filter: no path or link may lead out of the folder), a
  check that it is one folder with a `usdcc-plugin.json`, the rename, and the pack joins
  `wanted.json`. Then "MoonRay is installed" with **Restart Now**.
- **Remove...** asks once, naming the pack and the room it frees. The pack leaves `wanted.json`'s
  `packs` at once, so no update and no start fetches it again, and joins its `removed`, so a
  session saved with it loads without it quietly ([SESSIONS.md](SESSIONS.md) §4.3); installing it
  again takes it out. One this usdcc hasn't loaded is deleted at
  once; a loaded one is marked, goes at the next start, and **Keep** takes that back.
- **Nowhere else.** The viewport's Renderer menu has no entry for packs: a pack is plugins of any
  kind, and View > Plugins is where they are managed.
- **Trust** is that of the app's own updates: HTTPS and GitHub ([UPDATES.md](UPDATES.md) §2).
  The index decides what is downloaded; its hash protects the archive, not the index.
- **A development build, or a test profile,** asks no release. `USDCC_PACKS_URL` names a folder,
  or a URL of one, with the index and archives to read instead; a studio can point it at a copy
  of its own.
- **`usdcc --install-packs <names>`** and **`--remove-packs <names>`**, names separated by commas,
  for farm machines, as `--install-plugin-packages`: no window; 0 when done, 2 when a pack isn't
  offered for this version (or isn't installed), 1 when something failed. `--install-packs chosen`
  installs those of `wanted.json`. A render node loads the packs the app installed.

### 10.5 With an update of usdcc

**Settings > Updates > Keep installed packs up to date** (on). With it, **Download and Install**
([UPDATES.md](UPDATES.md) §4) does this first, so that an update which installs itself at the
next start never does so with a question open:

1. Reads the new version's index. A chosen pack it doesn't offer: "MoonRay isn't available for
   usdcc 26.12.2 yet", with **Update Anyway** and **Later**. The "is available" notification
   says so already, and counts the packs' download in its size.
2. Puts each chosen pack into the new version's folder, beside this one's, which stays in use:
   copied when the index's `content` is the installed pack's (hard links where the drive allows),
   downloaded otherwise. A failure asks as in 1, with **Retry**.
3. Downloads the app's own package. After Restart Now the new version starts with its packs: one
   restart, and nothing to fetch or unpack at the start.

Why the content can be the same: a usdcc release doesn't build packs. `deps-usd-plugins.yml`
builds one per pin and OpenUSD asset and skips what is published (§4); `release.yml` downloads
that zip and `deploy.py` repackages it. It differs with a new OpenUSD pin, an edit to
`build_usd_plugin.py` or `usd_plugin_asset.py`, a pack with a C++ plugin of usdcc's, and on
macOS once a Developer ID signs each release's binaries anew. It is never assumed: only an equal
hash copies.

**At a start that lacks a chosen pack** (a new 7z or tar.xz, or Update Anyway), a few seconds
after the window shows and after the update check: the pack is fetched once this version's index
has it, then "MoonRay is ready" with **Restart Now**.

**Only packs.** Plugins the user put into `usdcc_config/plugins` are left alone, as before:
Python and schema plugins keep working, and a C++ plugin built for the old usdcc stops loading.
That is what the parked registry is for.

### 10.6 Tried

**In the app on Windows (2026-11-26),** a development build with a test profile, the test pack
and `USDCC_PACKS_URL`, a script pressing the panel's own buttons:

- Install: Not installed, then Restart to use, the notification, the pack in the store. At the
  next start Installed, and `UsdccHelloThing` exists; before, it didn't.
- Remove..., cancelled: nothing. Remove: Removed at the next start; Keep: Installed again; Remove
  again, and at the next start the type and the folder are gone and nothing fetches it.
- Install and Remove before any restart: deleted at once.
- `--install-packs`: 0, and 2 with a name that isn't offered; without a source, 1 and why.
  `--remove-packs`: 0, then 2 for a pack that isn't there.
- `tools/ci/check_pack_install.py` against that build: installs, finds the pack registered from
  the data folder, removes it; with one byte added to the archive it fails.

**A pack outside the app on Windows (2026-11-26):** the local build of hdCycles (§7.1) laid out
as a pack folder in a temporary folder, `hdCycles.dll` in `plugin/usd` and Open Image Denoise's
three DLLs in the pack's `bin`, registered with `--plugin-path`; a script loads the plugin and
records a frame of `test_data/cycles/check.usda`:

| What is done about the pack's `bin` | The plugin | A frame |
|---|---|---|
| Nothing | "The specified module could not be found" | |
| Appended to the process's `PATH` | loads | renders; Open Image Denoise from the pack's `bin` |
| `AddDllDirectory` alone | "The specified module could not be found" | |
| Its DLLs loaded by full path first | loads | renders |

So `PATH`, which loads nothing until a pack is used. `AddDllDirectory` isn't enough, as
PLUGIN_REGISTRY.md §7 expected: OpenUSD's plain `LoadLibraryW` doesn't look there.

Then the same folder in the store itself, with a `pack.json`, in a test profile's
`packs/26.11.10-windows-x64/cycles`, and nothing else done: usdcc's own start put its `bin` on
`PATH`, the plugin loaded from the store, Open Image Denoise from the pack's `bin`, and the
frame rendered. That folder was laid out by hand from a local build, not made by `deploy.py`
from a pack.

**The link on Linux (2026-11-26; WSL, Ubuntu 24.04),** with the deploy and the MoonRay pack of
2026-11-24, the deploy without PySide6. The pack's folders were moved to a folder of their
own, laid out as an install, with a `plugin/usd/plugInfo.json` and a `usdcc-plugin.json`:

- *Without the link* the renderers are listed but no plugin loads: `libusd_pegtl.so: cannot open
  shared object file`, a library of OpenUSD that usdcc hadn't loaded by then.
- *With the link* all three plugins load from the data folder and both delegates render, the
  Arras one too, whose `execComp` is a process of its own and finds everything through the link.
  The pictures are the control's (the pack inside the deploy). The process has 57 MoonRay
  libraries from the data folder and the rest from the app or the system; none is mapped twice,
  none is there under one name from two places, and libfreetype is the app's.
- *With the app's folder renamed and the link made again,* as an AppImage's next start would
  have it: the same.

**On macOS (2026-11-26; Apple silicon),** with usdcc 26.11.10 from `/Applications` and its
MoonRay pack, which is older than the shader nodes (MOONRAY.md phase 5) and so has three of the
check's five plugins:

- *With the link* the three load from the data folder and both delegates render, as in the
  control; the pack's `@loader_path/../../../../lib` leads through the link as the RUNPATH does on
  Linux. 57 MoonRay libraries come from the data folder; none is loaded twice or under one name
  from two places. *With the app's folder renamed* the same.
- *Without the link* it doesn't fail, it hangs: the debug delegate records the right picture,
  then the Arras one never finishes (killed after 5 minutes). dyld finds a library by install
  name when the process already has it, so more loads than on Linux, and what is missing shows
  only later.
- *Signing:* taking the pack out of the copied bundle breaks its seal (`codesign --verify`: a
  sealed resource is missing), as expected; the programs and libraries keep their own signatures
  and run. A pack in the data folder leaves the bundle whole.

To run the Linux and macOS trials again: `python tools/try_pack_outside_app.py --deploy <a deploy
with the pack in it>`. It does the four runs (the pack inside, outside without the link, with
it, and with the app moved) with the pin's own check scene, and says where each library of the
process came from. Those trials had one folder for all packs; the store has a folder and a link
per pack, which leads to the same place.

**Tests:** `pack_store_test.cpp` (the store, what a start does to it, a pack as a plugin),
`pack_index_test.cpp` (the index: names, addresses, what is left out), `pack_installer_test.cpp`
(from archives in a folder: a wrong hash, a wrong size, a path out of the folder, a file that is
no pack, a cancel, a copy in place of a download), `app_packs_test.cpp` (the list, install, remove
and keep, what a new version means for the chosen packs, the unattended install), and
`pack_tools_test.py` (the folder `deploy.py` writes, the archive, the index, the test pack).

**Not tried, and not done:**

- **Anything on Linux or macOS** as built: the link made by `PackStore`, unpacking a `tar.xz`
  with the deploy's Python (it needs `lzma`), a real AppImage (read-only, mounted by FUSE), a
  notarized bundle beside a data folder Gatekeeper may treat as downloaded, and whether a file
  usdcc downloads is quarantined.
- **An update of an installed usdcc with packs:** UPDATES.md §6's two packages and a
  `make_test_pack.py` folder for both versions. The packs' side of it is in `app_packs_test.cpp`;
  the questions before the download, in an install, weren't seen. It needs `vpk`.
- **A real pack through the whole route,** and a `release.yml` run with the new steps.
- **A pack that isn't render delegates or a schema:** file formats, a usdcc plugin in Python or C++.
- **Two usdcc at once from one AppImage** are mounted in two places and share one link. While
  the first runs, the second loads through the first's mount, which is the same version; when
  the first quits, what the second loads afterwards (a shader, a new `execComp`) is missing. A
  link folder per process doesn't help as it is, since `..` in a RUNPATH is followed from the
  pack's real folder; hard links of the pack per process would.
- **A new pack takes effect at the next start,** a renderer's too, though OpenUSD could register
  one while usdcc runs.
- **macOS packs carry their debug symbols:** the folder is made before `split_dsym.py` strips
  the bundle they come from.
- **Retiring the old route:** the archives to unpack over usdcc, and the second disk image.
