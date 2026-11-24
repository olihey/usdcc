# MoonRay (plan)

Building DreamWorks' MoonRay as a USD plugin pack, shipping it, and its nodes in the
Material Editor. The pack framework is [USD_PLUGINS.md](USD_PLUGINS.md).

**Status (2026-11-24):** phase 1 passed (§7.1) **against the OpenUSD before phase 4 of USD_PLUGINS.md**:
the renderer and its shaders build and render on Ubuntu 24.04. It has to be repeated against the
current one, with oneTBB and OpenVDB. hdMoonray and the rest are a plan. **Linux x64 only:
no Windows, by decision.**

## 1. Short answers

| Question | Answer |
|---|---|
| Can a release have it? | Yes by licence, on Linux. As a **separate download** (`optional` set), because of its size |
| Licence | Apache-2.0, as is every MoonRay repository it needs. usdcc stays BSD-3-Clause (§3) |
| Size | CPU only, 255 MB installed and 166 MB as `tar.xz`, measured without hdMoonray and Arras (§7.1). The Linux release is a 166 MB `tar.xz` (26.11.8), so the release would about double. Open Image Denoise adds 46 MB |
| Nodes in the Material Editor | Yes, about 100 materials, maps, displacements, volumes and normal maps, after a port of MoonRay's Sdr plugins (§5) |
| MaterialX | **Only its pattern nodes, so not usable yet.** No BSDF or surface shader; `standard_surface` and `open_pbr_surface` don't render (§6). UsdPreviewSurface does |

## 2. The build

Baseline: OpenMoonRay `v2026.29.1` (2026-07-17, the newest release).

**Platform: Linux x64.** Upstream builds and tests Rocky 9 with GCC 11; ours is Ubuntu 24.04
with GCC 13. On Windows MoonRay isn't in the renderer list. Third-party Windows ports exist and
aren't used. macOS follows usdcc's own macOS build.

### What we write ourselves

1. **hdMoonray on OpenUSD 26.08.** Its code has version checks up to 23.11;
   upstream's superbuild pins USD 23.08, with later fixes for 25.5.
   `IsSupported()`'s signature and scene index emulation are known changes. usdcc feeds delegates
   through scene indices, so implicit shapes (Sphere, Cube) may need the small plugin Gatling
   needed (`gatling-implicit-surfaces.patch`).
2. **Both render delegates** (§2.1), sharing `hydramoonray` and one MoonRay. In-process first
   (phase 2), Arras, local only, after it (phase 3). `hd_moonray_debug` is shown as "MoonRay
   (in-process)", `hd_moonray` as "MoonRay".
3. **The Sdr plugins** (§5).
4. **`moonray_runtime`** beside `arnold_runtime`, setting in-process, before any renderer is asked for,
   `RDL2_DSO_PATH`, `REZ_MOONRAY_ROOT` and the class folder.
5. **The build hook** that USD_PLUGINS.md §4 lists as still to come: several repositories
   in order, their dependencies, ISPC. After it, `rdl2_json_exporter` writes the node descriptions.

### 2.1 In-process or through Arras

hdMoonray translates Hydra's scene into MoonRay's. Two plugins share that code
(`hydramoonray`) and differ in where the rendering then happens:

| | `hd_moonray` | `hd_moonray_debug` |
|---|---|---|
| Renderer class | `ArrasRenderer` | `RndrRenderer` |
| Where | An Arras session: other processes, here or on other machines | usdcc's own process and threads |
| Scene changes | Serialized (RDL messages) | Written straight to the renderer's scene |
| Pictures back | Encoded messages, decoded by the client | Copied from the render buffer |
| A MoonRay crash | The session ends | **usdcc ends too** (upstream's README says so) |
| Several machines | Possible; not shipped | No |
| To ship | Arras core, messages, computations, session files | Neither |

**Both ship**, as two entries in the renderer list. They are separate plugins with their own
names, and the MoonRay libraries and shaders, almost all of the size, are there once.

- **In-process** has the fewest parts, and is how Storm and Embree run. Upstream's README warns that its
  Restart setting does not work and that it **locked up Houdini 19 until the render finished**.
- **Arras, local** starts MoonRay as a child program, `execComp`, with no server or node
  service. It is what upstream uses, and should be the default if it works as well as expected.
- Either can be dropped if it fails its phase, without changing the other.

Arras adds:

- arras4_core, mcrt_messages, mcrt_dataio, mcrt_computation, `execComp` and the session files.
  A few MB, estimated; their libcurl, OpenSSL and libuuid are read for §3.
- `bin` on `PATH` and `ARRAS_SESSION_PATH`, set by `moonray_runtime`; the child inherits the
  environment, MoonRay's own variables included.
- libmicrohttpd (LGPL) is needed to build arras4_core, whose `httpserver` library is the part that
  links it. Ship neither if nothing else in the pack loads them.
- A lifetime check: no `execComp` is left when usdcc quits or crashes.

Remote hosts and the farm stay out: they need Arras's coordinator and node services. The
"Use Remote Hosts" settings are hidden.

Both plugins say `priority` 99. Check which renderer USD then picks as its default, and lower it, so
that installing the pack doesn't change usdcc's.

### Dependencies

| Library | OpenUSD asset | MoonRay | Plan |
|---|---|---|---|
| TBB | oneTBB 2021.12 since phase 4; 2020.3 in the one tried | 2020.3.3 on Rocky 9 | **Open.** Three source files outside the tests include TBB headers that oneTBB removed, so patches are needed |
| Embree | 4.3.3: ray masks on, AVX-512 on, ISPC support off | 4.2.0 with ISPC support, ignore-invalid-rays, AVX-512 | **The asset's, no rebuild** (§7.1): it has the ISPC headers, and exports `rtcIntersect4/8/16`, all that MoonRay's own ISPC code calls |
| OpenImageIO, OpenEXR, OpenColorIO | 2.5, 3.1, 2.2 | 2.4.8, 3.1.8, 2.2.1 | The asset's |
| OpenSubdiv | 3.6.1 | 3.5.0 | The asset's |
| Boost | 1.80, without program_options | program_options too | The asset's, and `libboost_program_options` 1.80 in the pack |
| Lua, JsonCpp, log4cplus, Random123 | none | needed | Upstream's `building/` scripts, static where possible |
| OpenVDB | 10.1 since phase 4; none in the one tried | 9.1 on Rocky 9 | The asset's, untested; the trial built 10.0.1 |
| Open Image Denoise | none | 2.3.3 | Needed to build. Only `libdenoiser.so` and the `denoise` tool link it; the renderer didn't load it. Whether hdMoonray or Arras needs `libdenoiser` is phase 2 and 3 |

CPU only (`-DMOONRAY_USE_OPTIX=NO`), no Qt apps. The compile options ask for AVX2, so the
renderer stays hidden on an older CPU. Linux on Arm is out of scope.

## 3. Licence

- All of it is Apache-2.0: the release needs `LICENSE`, upstream's `THIRD-PARTY.md` and our
  changed files marked as changed. There is no `NOTICE` file in the top repository.
- `THIRD-PARTY.md` names one text once under GPL or MIT, now public domain, and the JSON
  License ("good, not evil"). Which shipped code that second one covers has to be checked.
- Shipped libraries are permissive (Apache-2.0, MIT, BSD, zlib, MPL-2.0 for older OpenVDB
  versions). libmicrohttpd (LGPL) is for Arras's HTTP server, which a local session shouldn't need (§2.1); if it is needed, it ships as a replaceable shared library.
- GPU mode later: CUDA runtime and OptiX terms are read first.
- Gate: the pack check fails on a library the pin doesn't list (USD_PLUGINS.md §5), and
  nothing is published until the collected list is read.

## 4. Size

The estimate made before phase 1; §7.1 has the measured size, which agrees. Sample tables measured from upstream's files. The rest is taken from the zip listing of a third-party
Windows build (MoonRayForModo 0.5.6), for want of a Linux binary to measure, and reduced to what a CPU pack needs. 7z figures are estimated; `tar.xz` compresses about the same.

| Part | Installed | Zip | 7z |
|---|---|---|---|
| **Sample tables** embedded in `rendering_rndr`; measured with LZMA | 196 MB | 171 MB | 154 MB |
| MoonRay and scene_rdl2 libraries, without the tables | ~36 MB | ~9 MB | ~6 MB |
| Shader DSOs (145) | 27 MB | 8 MB | ~4 MB |
| OpenVDB and small libraries | ~6 MB | ~2 MB | ~1 MB |
| **CPU pack** | **~265 MB** | **~190 MB** | **~165 MB** |
| + Open Image Denoise | 52 MB | 47 MB | ~45 MB |
| + own Embree, if the asset's can't be shared | 40 MB | 16 MB | ~10 MB |
| + MaterialX nodes (598 DSOs, not worth it yet) | ~110 MB | ~33 MB | ~15 MB |

- For comparison, 26.11.8's Linux `tar.xz` is 166 MB.
- **Three quarters of it is noise tables**, which hardly compress. The largest (134 MB,
  `pmj02`) sits behind a compile option, but the other choices' tables aren't in the
  repository (`#error`), except plain random, which is noisier. Cutting the 4096-sample table
  down would change pictures. Not now.
- So: **`optional`**, its own download. This needs a place in Settings > Renderers to install it.

## 5. Nodes in the Material Editor

MoonRay has Sdr plugins (`moonrayShaderDiscovery`, `moonrayShaderParser`), so nodes come
from the registry as Arnold's do, with no generated definitions. They read one JSON file
per class from `MOONRAY_CLASS_PATH`.

What exists: 24 materials (DwaBaseMaterial, DwaMetalMaterial, glass, skin, hair, velvet,
toon, ...), 82 maps, 5 displacements, 3 volumes and 12 normal maps by folder count, with lights,
cameras and geometry beside them. A few folders are example or base classes.

Work:

1. **Port both to Sdr.** They include `pxr/usd/ndr`, which our OpenUSD doesn't have, so they
   don't compile. Small: about 700 lines.
2. **A profile**, `moonrayRendererProfile()`, added by `enableMoonray()`: system
   `moonrayClass`, terminals surface, displacement and volume, default
   `DwaBaseMaterial`. The parser gives contexts for Surface, Pattern, Displacement, Volume,
   Light and LightFilter; hide the last two and everything else that isn't a shader.
   Map groups aren't in the files, so a `nodeGroups` table fills them (Noise, Texture, Color,
   Math, Utility).
3. **Identifiers.** They are bare class names (`ImageMap`, `UsdPreviewSurface`), which can
   collide: MoonRay has its own UsdPreviewSurface and UsdPrimvarReader classes. The catalog
   keeps USD's and hides MoonRay's. A `moonray:` prefix would need a matching strip
   in hdMoonray; not needed if hiding works.
4. **Terminal.** hdMoonray overrides neither `GetMaterialRenderContexts()` nor
   `GetShaderSourceTypes()`, so it reads only the universal `outputs:surface` and the catalog
   can't filter by delegate. A patch returns `{"moonray", ""}` and `{"moonrayClass"}`: then a
   material can have `outputs:moonray:surface` beside its UsdPreviewSurface, and Storm still
   shows the latter.
5. **Multiple outputs** (`.r`, `.x`): hdMoonray makes one object per output channel. Check
   that the parser lists them; it adds only `out`.

## 6. MaterialX

- v2026.29 added `materialx_shaders` as work in progress, off by default
  (`-DBUILD_MATERIALX_SHADERS=ON`). Its status table: 547 of 656 listed nodes exist, all
  patterns (image, noise, math, color, ...). Many of the image and tiling variants are missing.
- **There is no BSDF, EDF, VDF or surface shader**, and the table lists none. Upstream says
  BSDFs are in progress. The "Next Release" pull request (open since 2026-11-12) has none either.
- hdMoonray doesn't ask for the `mtlx` context, so `outputs:mtlx:surface` is ignored.
- What works: **UsdPreviewSurface** with its primvar readers, and MaterialX patterns
  feeding a MoonRay material, as far as the types match (untested).
- Plan: leave it off. Most materials with a `mtlx` output have a UsdPreviewSurface too
  (Adobe's importers write both). Those that don't get MoonRay's default material, and the
  Render settings say so. Re-check at each MoonRay release. A translation of
  `standard_surface` and `open_pbr_surface` to DwaBaseMaterial in hdMoonray is possible
  and is a separate decision.

## 7. Phases

| Phase | Does | Done when |
|---|---|---|
| **1. Go or no-go** (Ubuntu 24.04 in WSL) | scene_rdl2, moonray and moonshine against the Linux asset's TBB, Embree and OpenImageIO | `moonray` renders `rectangle.rdla`, loading none of its own copies of the asset's libraries |
| **1b. Again with the current asset** | The same with oneTBB and the asset's OpenVDB, with patches for the removed TBB headers | As phase 1 |
| **2. hdMoonray** | Port to 26.08, in-process renderer, scene index plugin | Renders in usdcc's viewport on Linux: meshes, implicit shapes, lights, UsdPreviewSurface, responsive while rendering, camera moves, edits, resize, switching away and back, exit |
| **3. Arras, local** | Its libraries, `execComp`, the session files, the environment | The same list of checks; killing `execComp` leaves usdcc running; none left after quitting |
| **3b. Crash test of both** | Bad textures, huge meshes, rapid edits, quitting mid-render | The default is chosen; one that fails is left out |
| **4. Pack** | `moonray.json` (`linux-x64`, `optional`, `"publish": false`), the build hook, a pack check that renders a frame on the CPU, the library list check | Built by CI and kept with the run |
| **5. Nodes** | §5, MATERIAL_EDITOR.md §3.3, tests | A DwaBaseMaterial network made in the editor renders in MoonRay |
| **6. Settings and final render** | hdMoonray's render settings under Settings > Renderers, FINAL_RENDER.md checks | A final frame is written |
| **7. Publish** | Licence list read, real size noted, download offered in Settings | On a release as its own asset |
| **Later** | Open Image Denoise, XPU (CUDA/OptiX), MaterialX surfaces, macOS | |

Phase 1 decides the rest and is the cheapest. Phase 2 is the largest risk.

### 7.1 Results of phase 1 (2026-11-24)

WSL, Ubuntu 24.04, GCC 13.3, OpenMoonRay v2026.29.1, against a Linux OpenUSD 26.08 for
Python 3.13 **from before phase 4** (TBB 2020.3, no OpenVDB); CPU only. Under `~/mr` there.

- **Passed.** `moonray` renders `rectangle.rdla` and a scene with DwaBaseMaterial,
  UsdPreviewSurface, a map, an EnvLight and a RectLight, in scalar and vector mode, with means
  within 0.1 % of each other and no NaNs.
- **No duplicates.** It loads the asset's TBB 2020.3, Embree, OpenImageIO, OpenEXR, OpenColorIO,
  OpenSubdiv and Boost. No library name is loaded twice, and none of the pack's is in the asset.
- **Time.** About 5 minutes for MoonRay on 32 threads, dependencies apart.
- **Size**, stripped: 255 MB (`librendering_pbr.so` alone 199 MB), 166 MB as `tar.xz`.
  Open Image Denoise: 51 MB, 46 MB as `tar.xz`.
- **What the build needs:**
  - Unix Makefiles: Ninja stops at the ISPC headers.
  - `-include functional -include cstdint -include memory -include string`: headers GCC 13 no longer
    includes indirectly.
  - `-Wl,--no-as-needed`: MoonRay's libraries depend on each other in circles and Ubuntu's GCC drops
    the "unused" ones.
  - scene_rdl2's Python module off (`add_subdirectory(mod)`): it wants Boost.Python. The only
    source change.
  - `luac` on `PATH`: without it the build goes on with an empty embedded Lua library.
  - `rdl2_json_exporter` on `PATH` for moonray and moonshine; the 174 DSOs come with 186 files
    in `coredata/`, for §5.
- **Dependencies** at upstream's versions (Lua 5.4.4, JsonCpp 1.9.5, log4cplus 2.0.5, Random123 1.14.0,
  ISPC 1.21, Open Image Denoise 2.3.3), OpenVDB 10.0.1 (blosc off), and static OpenSSL 3.0.15.
- **Not covered: the current OpenUSD asset** (oneTBB, its own OpenVDB, whose copy the pack must then not
  bring); the MoonRay build is redone against it first. Also hdMoonray, moonshine_usd, Arras, textures and VDB files, the unit tests.

## 8. Decisions

| # | Question | Recommendation |
|---|---|---|
| 1 | Platform | **Decided:** Linux x64 only |
| 2 | Standard or separate download? | Separate, at this size |
| 3 | Rebuild the OpenUSD asset's Embree | **Not needed** (§7.1) |
| 4 | Before or after Cycles? | Cycles first if one renderer with MaterialX on every platform is the goal |
