# USD plugins in CI

How usdcc's CI builds OpenUSD's third-party plugins (render delegates, file formats) the
way it already builds OpenUSD itself, and which of them a release ships. This is the
"USD plugins" item of [CI.md](CI.md) §7.

**Status:** phase 1 is built and checked locally; its pack isn't published yet, which
needs the workflow on `master` (§7). The rest is a plan.

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
  `usdplugin-<name>-<short commit>-<hash>-<platform>-py312.zip`. Packs are attached to
  the OpenUSD asset's own release (`deps/usd-<commit>-<hash>`), so one release holds an
  OpenUSD and everything built against it.
- **A pack never brings a DLL the OpenUSD asset already has.** Two DLLs with one name can't
  both load in a process; the first one wins, and the other plugin gets a version it wasn't
  built with. A pack builds against the asset's copy or links its own statically. The pack
  check fails on a duplicate name (§4).
- **Hosted runners have no GPU,** so a pack's check there is "it loads and registers".
  Rendering is checked on the self-hosted RTX runner, by hand, as `rtx-gpu.yml` does.

## 3. The plugins

| Pack | Source and licence | Needs | Against our OpenUSD (26.08, `typhoon/main`) |
|---|---|---|---|
| `adobe-fileformats` | `adobe/USD-Fileformat-plugins` 2026.07, Apache-2.0 | Fetches its own small dependencies (TinyGLTF, fmt, Happly, spz, ...). FBX needs Autodesk's FBX SDK, SBSAR needs Adobe's Substance SDK | **Proven:** builds unpatched against the published asset and passes the pack check (§7 phase 1), though its README only names USD 23.08–25.11 |
| `gatling` | `pablode/gatling`, **GPL-3.0** | NVIDIA's MDL SDK binaries; many submodules. A Vulkan ray tracing GPU to run | **Proven locally:** commit `945f47d` with MDL SDK 2026.0.2. 110 MB installed, 109 of them `libmdl_sdk.dll` |
| `cycles` | `blender/cycles`, Apache-2.0 | Blender's precompiled libraries (`make update`, Git LFS, several GB). `-DPXR_ROOT=<usd>`; USD 25.11 or newer. With an external USD it builds without OSL and NanoVDB | Not proven. The local build is against the older USD |
| `aurora` | `Autodesk/Aurora` v25.08-beta, Apache-2.0 | Its `installExternals.py` builds its own USD, an Autodesk fork (`autodesk-forks/USD`, tag `v25.08-Aurora-v25.08`); Slang, the DirectX shader compiler. A DXR GPU to run | Not proven. The last commit is from August 2025; an earlier local attempt needed `/WX` turned off |
| `arnold` | `Autodesk/arnold-usd`, Apache-2.0 | The Arnold SDK (7.0 or newer), which is proprietary. Its docs name USD 20.08–25.11 and the dev branch | Not proven |

Notes:

- **Adobe.** `usdgltf`, `usdobj`, `usdply`, `usdstl` and `usdspz` need nothing proprietary.
  OpenUSD's own `usdObj` example would clash with `usdobj`; `--no-examples` is already in
  `deps/usd.json`. `usdfbx` links the FBX SDK statically, so Autodesk's code ends up in the DLL.
  The plugins' names in OpenUSD are `usdGltf_plugin`, `usdObj_plugin` and so on, and they
  share three DLLs that go in `bin/` (`fileformatUtils`, `usdIpc`, `usdSerialization`).
  `usdGltf` also registers a package resolver for `.gltf` and `.glb`. The first glTF read
  in a process prints a warning that `USD_FILEFORMATS_WRITE_USDPREVIEWSURFACE` is
  deprecated; usdcc will have to set it (phase 2).
- **Gatling.** Its Sdr plugins also give the Material Editor MDL nodes
  ([MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) §3.5).
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
- **Arnold.** Its Sdr plugin starts the Arnold core at discovery (`AiBegin`), which slows
  every start, so this pack must never be on the plugin path unless the user turned it on.

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
- Still to come, with the packs that need them: `submodules` and `downloads` (an SDK's URL
  and SHA-256) for Gatling, `renderers` in `check`, and a small Python hook next to the
  JSON for a build that isn't plain CMake (Cycles' `make update`, Aurora's externals script).

### `tools/build_usd_plugin.py`

`python tools/build_usd_plugin.py <pack> --usd-install <folder>`, with the pin's Python:

1. Fetch the pinned commit (shallow) and apply the patches.
2. Configure with the OpenUSD install on `CMAKE_PREFIX_PATH`, Release, `/MD`, Visual
   Studio 2022, the Python it runs on: the same rules as usdcc plugins (CI.md §2).
3. Build and install into an empty **pack folder** laid out like the OpenUSD install
   (`plugin/usd/...`, `bin/` for DLLs the plugins need).
4. Drop what only a build needs: `include/`, CMake and pkg-config files, import libraries.
5. Collect licences into `licenses/usd-plugins/<pack>/<library>/`: the plugin's own and
   those of what CMake fetched into it (`tools/ci/usd_licenses.py`).
6. Write `usd-plugins/<pack>.json`: the pin, the OpenUSD it was built against, and the
   list of the pack's files, which `deploy.py` and the check go by.

`--install` copies the pack over the OpenUSD install, which is what `build.ps1` did.
Linux, with RUNPATHs relative to `$ORIGIN`, is phase 8.

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
   - Still to come: no DLL name shared with another pack (with the second pack), and a
     render delegate listed by `HdRendererPluginRegistry` without creating it, which
     needs the GPU (phase 3).
5. Attach the zip to the OpenUSD asset's release, and delete the pack's older zips from it.

A pack marked `local` (§5) is built only when its SDK is there (a secret URL, or the
self-hosted runner) and is kept as a workflow artifact, never published.

### `build.yml`, `release.yml`, `tools/deploy.py`

- After unzipping OpenUSD, download the `standard` packs and unzip them over it. A missing
  pack is a warning in `build.yml` (it may still be building) and an error in `release.yml`.
- `deploy.py` copies only the plugins of the packs it was told to, by name, not all of
  `plugin/usd`. Its Blender configuration already works from such a list
  (`_mesh_usd_plugins`).
- `tools/ci/smoke_test.py` also checks that the deploy lists the expected renderers and
  opens a `.gltf`.
- `usdcc_tests` gets a test that opens one sample file per file format, skipped when the
  plugin isn't installed.
- New: `tools/fetch_usd.py <folder>` downloads the OpenUSD asset and the packs for the
  current pins. A new machine then needs no OpenUSD build, and a local build uses exactly
  what CI does.

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
  licence in it. Until then it is built and checked in CI and used locally.
- **Arnold, FBX and Substance** need SDKs that can't be republished. Whether a plugin
  built against them may be published without the SDK is a question for their licence
  texts (§8).
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
3. **Gatling**, as the `optional` set's first pack, from the commit and MDL SDK that work
   locally. Done when the pack is published and a deploy with it unzipped over lists
   Gatling and renders on the RTX runner.
4. **The OpenUSD flags** of §6, tried locally first, then one `deps-usd.yml` run. Done when
   the new asset and every pack are published and `build.yml` is green.
5. **Cycles, CPU only.** Settle the Embree clash, then the pack. Done when it renders a
   frame on a machine without a GPU. Then the GPU kernels, one device at a time.
6. **Aurora.** First a local build against our OpenUSD. If it needs Autodesk's fork, stop
   and drop it.
7. **Arnold**, as a `local` pack on the self-hosted runner.
8. **Linux** for each pack that supports it (all but Aurora's DirectX backend), as
   `linux` jobs in the same matrix.

## 8. Open questions

| # | Question | Assumption until decided |
|---|---|---|
| 1 | Do the plugins build against 26.08 from `typhoon/main` at all? Gatling and Adobe's are proven; arnold-usd names 25.11 as its newest | Try each locally before its phase; carry small fixes as patches in `deps/usd-plugins/` |
| 2 | May usdcc publish a GPL-3.0 Gatling pack as a separate download? | No, until it has had the Blender configuration's review |
| 3 | May a `usdfbx` or hdArnold built against the vendor's SDK be published without the SDK? | No: `local` only. Read the FBX SDK and Arnold SDK licence texts before changing that |
| 4 | Does hdAurora build against a stock OpenUSD? | Unknown; phase 6 answers it |
| 5 | Switch OpenUSD to oneTBB? | Yes if a local build of usdcc and its tests pass with it, since Cycles and Adobe expect it |
| 6 | Are the Releases page's `deps/...` entries getting too many (RELEASE.md §7 item 2)? | Packs add assets to existing releases, not new releases, so no |

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
