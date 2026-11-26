# MoonRay (plan)

Building DreamWorks' MoonRay as a USD plugin pack, shipping it, and its nodes in the
Material Editor. The pack framework is [USD_PLUGINS.md](USD_PLUGINS.md).

**Status (2026-11-24):** phases 1 to 4 passed (§7.1 to §7.4): MoonRay and both of its render delegates,
in-process and through a local Arras session, build against the published OpenUSD asset as a pack,
`deps/usd-plugins/moonray.json`, which passes its check and renders in usdcc's viewport on Ubuntu 24.04 in WSL.
Its licences are read (§3.1), and a Linux release attaches it as a download of its own (§7.5), once CI
has published the pack. The pack builds on macOS (Apple silicon) too and renders there in usdcc (§7.6); CI
hasn't built that one. Its nodes are in the Material Editor (§7.7), and final renders are checked (§7.8). **Linux x64 and macOS: no Windows, by decision.**

## 1. Short answers

| Question | Answer |
|---|---|
| Can a release have it? | Yes by licence, on Linux. As a **separate download** (`optional` set), because of its size |
| Licence | Apache-2.0, as is every MoonRay repository it needs. usdcc stays BSD-3-Clause (§3) |
| Size | CPU only, 245 MB installed and 165 MB as `tar.xz`, measured without hdMoonray and Arras (§7.1). The Linux release is a 166 MB `tar.xz` (26.11.8), so the release would about double. Open Image Denoise adds 46 MB |
| Nodes in the Material Editor | Yes, about 100 materials, maps, displacements, volumes and normal maps, after a port of MoonRay's Sdr plugins (§5) |
| MaterialX | **Only its pattern nodes, so not usable yet.** No BSDF or surface shader; `standard_surface` and `open_pbr_surface` don't render (§6). UsdPreviewSurface does |

## 2. The build

Baseline: OpenMoonRay `v2026.29.1` (2026-07-17, the newest release).

**Platform: Linux x64.** Upstream builds and tests Rocky 9 with GCC 11; ours is Ubuntu 24.04
with GCC 13. On Windows MoonRay isn't in the renderer list. Third-party Windows ports exist and
aren't used. macOS on Apple silicon came after usdcc's own macOS build (§7.6).

### What we write ourselves

1. **hdMoonray on OpenUSD 26.08.** Done for the in-process plugin (§7.2).
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
  service. It is what upstream uses. **It is the one to recommend:** two of the crash tests
  (§7.3) take usdcc down in-process and not through Arras.
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

Both plugins say `priority` 100. The order is ascending (Storm 0, Embree 99): with 0, or less, MoonRay
became the default renderer of a fresh profile.

### Dependencies

| Library | OpenUSD asset | MoonRay | Plan |
|---|---|---|---|
| TBB | oneTBB 2021.12 | 2020.3.3 on Rocky 9 | The asset's. MoonRay's CMake sees the version and defines `TBB_ONEAPI`; no patch |
| Embree | 4.3.3: ray masks on, AVX-512 on, ISPC support off | 4.2.0 with ISPC support, ignore-invalid-rays, AVX-512 | **The asset's, no rebuild** (§7.1): it has the ISPC headers, and exports `rtcIntersect4/8/16`, all that MoonRay's own ISPC code calls |
| OpenImageIO, OpenEXR, OpenColorIO | 2.5, 3.1, 2.2 | 2.4.8, 3.1.8, 2.2.1 | The asset's |
| OpenSubdiv | 3.6.1 | 3.5.0 | The asset's |
| Boost | 1.80, without program_options | program_options too | The asset's, and `libboost_program_options` 1.80 in the pack |
| Lua, JsonCpp, log4cplus, Random123 | none | needed | Upstream's `building/` scripts, static where possible |
| OpenVDB | 10.1 | 9.1 on Rocky 9 | The asset's, blosc included |
| Open Image Denoise | none | 2.3.3 | Needed to build. Only `libdenoiser.so` and the `denoise` tool link it; the renderer didn't load it. Whether hdMoonray or Arras needs `libdenoiser` is phase 2 and 3 |

CPU only (`-DMOONRAY_USE_OPTIX=NO`), no Qt apps. The compile options ask for AVX2, so the
renderer stays hidden on an older CPU. Linux on Arm is out of scope.

## 3. Licence

- All of it is Apache-2.0: the release needs `LICENSE`, upstream's `THIRD-PARTY.md` and our
  changed files marked as changed. There is no `NOTICE` file in the top repository.
- `THIRD-PARTY.md` names one text once under GPL or MIT, now public domain, and the JSON
  License ("good, not evil"). §3.1 has which code that second one covers: none that ships.
- Shipped libraries are permissive (Apache-2.0, MIT, BSD, zlib, MPL-2.0 for older OpenVDB
  versions). libmicrohttpd (LGPL) is for Arras's HTTP server, which a local session shouldn't need (§2.1); if it is needed, it ships as a replaceable shared library.
- GPU mode later: CUDA runtime and OptiX terms are read first.
- Gate: the pack check fails on a library the pin doesn't list (USD_PLUGINS.md §5), and
  nothing is published until the collected list is read.

### 3.1 The pack's licences, read (2026-11-24)

What `licenses/usd-plugins/moonray/` holds, what it covers, and what it lacks. Read: the texts, and
MoonRay's and Arras's C++ and ISPC sources searched for other owners, licences and web addresses. A search, not
a legal review. No copyleft library ships. The three points it found are fixed in the
hook and a patch, below; the first pack published (from PR #85) predates that.

| Folder | Licence | Covers |
|---|---|---|
| `moonray`, `moonray-*`, `arras-arras4_core`, `cmake_modules` | Apache-2.0, one text in all 11 | MoonRay, Arras, hdMoonray. No `NOTICE` file in any of them |
| `moonray/LICENSE-THIRD-PARTY.md` | Apache-2.0, BSD-3/2-Clause, public domain; Kensler's has no licence text | Code MoonRay took from Embree, OpenImageIO, OSL (LPE), pbrt, Gustavson's noise, Kensler's sampling, OpenUSD |
| `moonray/LICENSE-OSL-LPE` | BSD-3-Clause | The OSL release the LPE sources are from |
| `moonray/LICENSE-xoolive-geodesy` | MIT | `atan` in scene_rdl2's `ssef.h` and `avxf.h` |
| `jsoncpp` | MIT, or public domain | JsonCpp |
| `log4cplus` | Apache-2.0 or BSD-2-Clause, by file; its thread pool zlib-style | log4cplus |
| `lua` | MIT (from the end of `lua.h`: Lua has no licence file) | Lua, linked into scene_rdl2 |
| `random123` | BSD-3-Clause | Random123, headers |
| `openssl` | Apache-2.0 | OpenSSL 3, static, for libcurl |
| `curl` | curl licence (MIT-style) | libcurl |
| `oidn` | Apache-2.0, the same text as its weights' | Open Image Denoise, CPU only, and its weights |
| `boost` | Boost Software License 1.0 | Boost.ProgramOptions |

Fixed (the first pack published lacks these):

- **Sony Pictures Imageworks' BSD-3-Clause notice**, at the top of the LPE sources from OSL
  (`rendering/lpe/osl`): `THIRD-PARTY.md` has only OSL's newer notice, with another copyright line. The hook
  takes it from those sources into `moonray/LICENSE-OSL-LPE`, and stops if they don't all carry the same one.
- **xoolive/geodesy, MIT:** the `atan` of `scene_rdl2/lib/common/math/ssef.h` and `avxf.h` is from it, says each.
  Its notice, in neither the sources nor `THIRD-PARTY.md`, is `moonray/LICENSE-xoolive-geodesy`.
- **Stack Overflow 25622745:** `MathUtil.h` cited it at one 8x8 transpose, and its inverse had the same
  instructions. Posts there are under CC BY-SA, a share-alike licence, so
  `patches/moonray-own-transpose.patch` replaces both with our own.

Less clear-cut, left as it is: Kensler's `randfloat` (from a Pixar paper) has no licence text, and geodesy's
`sse2_math.h` says it is inspired by Cephes, whose terms haven't been looked up.

Settled:

- **skeeto/hash-prospector**, where the sampler's hash functions are from, is under the Unlicense (public domain).
- **`sse_special.h`** has Cephes-named logarithm constants and no origin. Nothing compiled includes it.
- **oneDNN isn't in the pack:** no object of it was compiled and no `dnnl` or `xbyak` string is in the three
  libraries. Open Image Denoise's own third-party texts ship all the same; what else they name (PyTorch, Catch2,
  CUTLASS, composable_kernel, DPC++) is for training, tests and other devices.
- **The JSON License** in `THIRD-PARTY.md` is inside its OpenUSD part, which repeats OpenUSD's
  `LICENSE.txt` (not its `NOTICE.txt`). RapidJSON says there that only its `bin/jsonchecker` is under it.
  usdcc ships that `LICENSE.txt`, and no RapidJSON was found in MoonRay's sources.
- **libmicrohttpd (LGPL-2.1) isn't shipped:** only Arras's HTTP server, `libhttpserver.so`, links
  it, and nothing in a local session loads that. The hook ships only what the plugins, `execComp`,
  the shaders and the sessions' computations load, which also leaves out the library linked with CppUnit
  and Arras's test computations (8 of MoonRay's 72 libraries, and the 13 `Test*` shaders).
- **From the system, not shipped:** libuuid, libatomic and the GCC runtime, zlib; libfreetype (§7.5).
  libatomic isn't on every install.
- **Apache-2.0 §4(b)**, changed files marked: the pack's `SOURCES.txt` names each patch and where it
  applies. Of the files the patches change only `MathUtil.h`
  says so itself.

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
| **2. hdMoonray** | Port to 26.08, in-process renderer, scene index plugin | Renders in usdcc's viewport on Linux: meshes, implicit shapes, lights, UsdPreviewSurface, responsive while rendering, camera moves, edits, resize, switching away and back, exit |
| **3. Arras, local** | Its libraries, `execComp`, the session files, the environment | The same list of checks; killing `execComp` leaves usdcc running; none left after quitting |
| **3b. Crash test of both** | Bad textures, huge meshes, rapid edits, quitting mid-render | The default is chosen; one that fails is left out |
| **4. Pack** | `moonray.json` (`linux-x64`, `optional`, `"publish": false`), the build hook, a pack check that renders a frame on the CPU, the library list check | Built by CI and kept with the run |
| **5. Nodes** | §5, MATERIAL_EDITOR.md §3.3, tests | A DwaBaseMaterial network made in the editor renders in MoonRay |
| **6. Settings and final render** | hdMoonray's render settings under Settings > Renderers, FINAL_RENDER.md checks | A final frame is written |
| **7. Publish** | Licence list read, real size noted, download offered in Settings | On a release as its own asset |
| **Later** | Open Image Denoise, XPU (CUDA/OptiX, Metal), MaterialX surfaces | |

Phase 1 decides the rest and is the cheapest. Phase 2 is the largest risk.

### 7.1 Results of phase 1 (2026-11-24)

WSL, Ubuntu 24.04, GCC 13.3, OpenMoonRay v2026.29.1, CPU only, against
`usd-70c45e83b-76ce049f-linux-x64-py313` (oneTBB 2021.12, OpenVDB 10.1). Under `~/mr` there:
`env2.sh`, `b2`, `inst2`. A first try against an OpenUSD from before oneTBB passed too.

- **Passed.** `rectangle.rdla` and a scene with DwaBaseMaterial, UsdPreviewSurface, a map, an EnvLight
  and a RectLight, in scalar and vector mode: no NaNs, and vector mode the same as against the
  earlier OpenUSD (mean error 0.00001 at most). A textured sphere and a blosc-compressed
  VDB fog render.
- **No conflicts.** It loads the asset's TBB, Embree, OpenVDB, OpenImageIO, OpenEXR, OpenColorIO,
  OpenSubdiv and Boost. No library is loaded twice, and no library of the pack has the name of one
  in the asset.
- **Embree as it is:** the asset has its ISPC headers and exports `rtcIntersect4/8/16`.
- **Time.** About 5 minutes on 32 threads, dependencies apart.
- **Size**, stripped: 245 MB (`librendering_pbr.so` alone 199 MB), 165 MB as `tar.xz`.
  Open Image Denoise: 51 MB, 46 MB as `tar.xz`.
- **Two source changes**, to carry as patches:
  - scene_rdl2 without its Python module (`add_subdirectory(mod)`), which wants Boost.Python.
  - **Plain textures.** As released, MoonRay accepts only tiled, mip-mapped textures
    (`.tx`): a PNG shades black, with "unknown oiio sample failure". `TextureSampler.cc` has the
    other choice in an `#else`; with it a PNG renders exactly as its `.tx`. USD scenes have PNG and JPEG, so we
    need it. The texture system is OpenImageIO's shared one, so in-process these settings apply to
    anything else in usdcc that uses it: check in phase 2.
- **What the build needs:**
  - Unix Makefiles: Ninja stops at the ISPC headers.
  - `-include functional -include cstdint -include memory -include string`: headers GCC 13 no longer
    includes indirectly.
  - `-Wl,--no-as-needed`: MoonRay's libraries depend on each other in circles and Ubuntu's GCC drops
    the "unused" ones.
  - `luac` on `PATH`: without it the build goes on with an empty embedded Lua library. The build
    script checks that `rdlalib.cc` isn't empty (32 KB).
  - `rdl2_json_exporter` on `PATH` for moonray and moonshine; the 174 DSOs come with 186 files
    in `coredata/`, for §5.
  - Boost.program_options 1.80, which the asset lacks. The trial put it into a copy of it; the pack
    brings it.
- **Dependencies** at upstream's versions (Lua 5.4.4, JsonCpp 1.9.5, log4cplus 2.0.5, Random123 1.14.0,
  ISPC 1.21, Open Image Denoise 2.3.3), and static OpenSSL 3.0.15.
- **For the in-process delegate:** MoonRay sets a `tbb::global_control` for its thread count, which is
  for the whole process: it can limit OpenUSD's own threads while a MoonRay renderer exists.
- **Open:** the VDB's left edge looks cut off in the picture; not looked into.
- **Not covered:** hdMoonray, moonshine_usd, Arras, the unit tests.

### 7.2 Results of phase 2 (2026-11-24)

hdMoonray of v2026.29.1 with `deps/usd-plugins/patches/moonray-hdmoonray-26.08.patch`, beside the
two small patches of §7.1 (`moonray-plain-textures.patch`, `moonray-no-boost-python.patch`). usdcc
was built in WSL from this branch against the same asset and driven by a script under Xvfb, with the
plugin and its folders given through the environment.

**What the patch does:**

- `IsSupported()` with 26.08's arguments, the only compile error.
- CMake: `HDMOONRAY_ARRAS` (off here) for the Arras plugin and the command line tools; no Vulkan then, and no Boost.Python.
- A scene index plugin (`plugin/common`): implicit shapes to meshes, as Gatling needed, and
  OpenUSD's light linking scene index, without which light linking was ignored.
- Light link categories that aren't there are read as empty, not as an error.
- The "requires emulation" coding errors of the change tracker are held back where hdMoonray marks prims
  dirty itself. OpenUSD still marks them, so a light added later lights the scene.
- A subdivision surface drawn at refine level 0 gets smooth normals, as in Storm.
- Names "MoonRay" and "MoonRay (in-process)", `priority` 100.

**In usdcc's viewport,** each looked at in a picture: meshes, implicit shapes, UsdPreviewSurface with a PNG texture,
dome and rect lights; editing a transform and a material, adding a light and a
shape, deleting prims, moving the camera, 40 camera moves as in a drag, resizing, to Storm and back, and six quick
switches. Grid and axes are hidden by objects as with Storm, to within a pixel or two. Quitting: exit
code 0 in the long run and three times while MoonRay was rendering.

**Responsive.** The longest pause of the UI thread was 0.42 s with the final build (0.54 s in
an earlier run), most under 0.2 s. Houdini's lock-up (§2.1) wasn't seen.

**Through UsdImaging without the app:** light linking, a point instancer, native instances and
curves.

**Different from Storm, or open:**

- The dome light isn't drawn in the background, only its light.
- Curves at refine level 0 are polylines.
- Not tried: scenes of real size, animation and motion blur, an orthographic view, geom subsets,
  displacement, volumes through Hydra, AOVs and selection picking, `doubleSided` and transparency, a GPU other than
  the one here, native Linux outside WSL.
- The process-wide thread limit and shared texture system (§7.1): no effect seen, not measured.

### 7.3 Results of phase 3 (2026-11-24)

The Arras libraries of v2026.29.1 (arras4_core, mcrt_messages, mcrt_dataio, mcrt_computation) build against
the same asset unchanged. The hdMoonray patch adds `HDMOONRAY_TOOLS` (off) beside `HDMOONRAY_ARRAS` (on),
so that only the plugin and the session files are built, and an `IsSupported()` for `hd_moonray` that says no, with the
reason, when `ARRAS_SESSION_PATH` has no MoonRay session definitions or `execComp` isn't on `PATH`.
Without that the first broke the renderer's creation with an exception and the second left it
retrying to connect.

**What it needs:** libcurl (8.11.1 here, with static OpenSSL), libuuid (the system's), FreeType
(the system's, for mcrt_dataio; to check against the copy usdcc's Qt has), Boost headers the asset leaves out
(`boost/algorithm`). libmicrohttpd is needed to build arras4_core but nothing shipped links it. The
session files are 40 KB, the plugin, `execComp` and the computation library under 1 MB together.

**In usdcc's viewport,** the whole run of §7.2 again, through `MoonRay`: same pictures, longest UI pause 0.32 s.
Its settings are 24, the 14 of the in-process one and Arras's own (hosts, denoising, ...).

**Its process:**

- Killed with SIGKILL while rendering: usdcc goes on, the picture stays, and the next edit starts a new
  one. Killed five times in a row: the same.
- None is left after quitting, after switching to Storm, nor when usdcc ends with `os._exit()` while
  it renders. It goes within a second, once its socket closes.

**Crash tests,** 19 scenes, each opened with both delegates (`~/mr/bad`):

| Scene | In-process | Arras |
|---|---|---|
| A face with 2 points (`faceVertexCounts` 2) | **usdcc aborts** | usdcc goes on; picture blank |
| Two shaders connected to each other in a loop | **usdcc crashes** (SIGSEGV) | usdcc goes on; picture blank |
| Missing, broken and wrong-format textures; indices out of range or negative; counts that don't add up; an empty mesh; NaN and infinite points; primvars too short; an unknown shader; a MaterialX-only material; zero scale; a 2-point cubic curve; an orthographic camera | fine | fine |
| 4 million quads; 5,000 prims | renders | renders |

The blank picture says nothing about why; usdcc's log didn't show a reason that I found.

### 7.4 Results of phase 4, the pack (2026-11-24)

`deps/usd-plugins/moonray.json` pins OpenMoonRay v2026.29.1, the ten submodules it needs (not rats' test
images, the GUIs or the DCC plugins), the four patches, each in its submodule, and the dependencies'
archives with their SHA-256. `"set": "optional"`, `"publish": false`, `linux-x64` only.

**What changed in the tools:**

- `tools/build_usd_plugin.py`: a pin's `"hook"`, a Python file beside it
  (`deps/usd-plugins/moonray.py`), builds instead of its one CMake project and returns the licences of
  what it put in; `"submodules"` can list the folders to check out, without Git LFS files; a patch can
  name the submodule it applies in (`{"patch": ..., "in": ...}`). The hook and those patches are in the
  pack's name hash. Editing this script renames every pack, as before.
- `tools/ci/check_usd_plugin.py`: `"render"` renders a file with a delegate through UsdImaging without
  a GPU and checks the picture's mean, read with the standard library.

**Layout,** found by OpenUSD with nothing set:

- `plugin/usd/moonray/`: `bin/execComp`, `lib/` (MoonRay, Arras, and JsonCpp, log4cplus, libcurl,
  Open Image Denoise, Boost.ProgramOptions), `rdl2dso/` (the shaders, without their `.proxy` files), `sessions/`
  and `plugin/` (the three plugin libraries).
- `plugin/usd/hd_moonray`, `hd_moonray_debug` and `hdMoonrayAdapters`: a `resources/plugInfo.json` each.
  One file with all three registered only the first.
- RUNPATHs relative to `$ORIGIN`, to its own `lib` and the install's.
- The plugins set `RDL2_DSO_PATH`, `ARRAS_SESSION_PATH` and `PATH` (for `execComp`) from where they are, when
  they aren't set (`plugin/common/Environment.cc` in the hdMoonray patch). So `moonray_runtime` in usdcc
  isn't needed. It does this with `setenv()` when the plugin library loads, which isn't thread-safe
  against another thread reading the environment at that moment.

**Built from scratch** in WSL in 9.5 minutes on 32 threads, the archives already downloaded.

**The check,** over a fresh copy of the asset in a bare environment: 291 files, none in the asset and
no library name twice; the three plugins load; both delegates are listed; `test_data/moonray/check.usda`
renders with each, through Arras in 1.1 s. No `execComp` was left.

**In usdcc,** the pack copied over a clean asset and no MoonRay variable set: "MoonRay" and "MoonRay
(in-process)" are listed and render. usdcc was the development build, not a deploy.

**Size:** 310 MB installed, 246 MB zipped, 214 MB as `tar.xz`. Open Image Denoise is 51 MB of it, for the
Arras client only (§2.1).

**In CI:** the USD plugins workflow (`deps-usd-plugins.yml`) builds and checks the pack on `ubuntu-24.04`
like the other packs, with `uuid-dev`, `libfreetype-dev`, `bison` and `flex` installed for it, and
publishes it beside the OpenUSD asset (`"publish": true`). A run started by hand can name one pack
(`gh workflow run deps-usd-plugins.yml --ref <branch> -f pack=moonray`), to try it on a branch
without building the others.

### 7.5 Shipping it (2026-11-24)

- **A download of its own** (§8 item 2): `usdcc-<version>-linux-x64-moonray.tar.xz`, about 214 MB,
  beside usdcc's 154 MB, unpacked over the usdcc folder. `release.yml`'s Linux job makes it with
  `tools/deploy.py --optional-packs` (USD_PLUGINS.md §5).
- **Licences read** (§3.1). The pack now ships only the MoonRay libraries something loads: 64 of 72,
  without Arras's HTTP server (and so without libmicrohttpd), the test libraries and the 13 test
  shaders. It also carries Lua's notice, MoonRay's `THIRD-PARTY.md` and Open Image Denoise's
  third-party notices, and the two of §3.1. 276 files.
- **FreeType:** only Arras's client (`libclient_receiver.so`, its telemetry overlay) links
  libfreetype. In a deploy, the pack's RUNPATH finds the deploy's, which is Qt's. Checked: the
  process maps one libfreetype, the deploy's `lib/libfreetype.so.6.20.6`.
- **Checked in WSL:** a deploy with `--optional-packs` passes its own checks with the pack in it,
  so every MoonRay library loads from the deploy or the system. Afterwards none of its files is
  left in the deploy, and the separate folder keeps the library links. Unpacked over the deploy,
  inside the started usdcc:
  - the three plugins load from it;
  - both delegates are listed, and both render `test_data/moonray/check.usda` (0.8 s in-process,
    1.3 s through Arras);
  - nothing outside the deploy and `/usr/lib` is mapped, and no `execComp` is left.

  The WSL build was an older usdcc and had no PySide6 module, so the release's smoke test itself (which needs it) didn't
  run there.

**Not done:**

- Publishing the pack with §3.1's fixes: it gets a new name, so a release takes it once CI has built it.
- A release with it: the pack has to be published first (`deps-usd-plugins.yml`, after the merge).
  Until then `release.yml`'s Linux job fails, as it does for a missing standard pack.
- Boost's headers came from the Boost download's install, not the asset's (same version, 1.80).
- Install notes for users beyond the release notes, and a way to get the pack from inside usdcc.

### 7.6 macOS, Apple silicon (2026-11-24)

The same pin, hook and check (`"platforms": ["linux-x64", "macos-arm64"]`), CPU only
(`-DMOONRAY_USE_METAL=NO`), with Xcode 27.2 on macOS 27.2. From scratch in 6.5 minutes on 18 threads.

**What differs in the build** (`MACOS` in `moonray.py`, `patches/moonray-macos-*.patch`):

- MoonRay's own ISPC commands, which upstream's Xcode build uses, not CMake's ISPC language: CMake hands ISPC
  Clang's flags. `neon-i32x4`, so 4 lanes where Linux has 8.
- No `-ld_classic`: Xcode 27 has no classic linker.
- The sample tables go in with the assembler's `.incbin`. Upstream writes them out as C arrays, 134 MB of it in
  one, which takes a compiler gigabytes.
- `common/arm`, headers scene_rdl2's own headers include and its install leaves out, is copied.
- Boost 1.82, as the macOS asset's. FreeType is built, static. libuuid and the atomics are in libSystem.
- What links a library by a link's name links it by its own, and no links ship: the hardened runtime
  refuses the other (MACOS.md §3).

**Bugs of upstream's found on the way:**

- **Half floats on Arm** (`PackTiles.cc`, `RenderOutputWriter.cc`, marked "TODO: Verify this"): the
  conversion returned the number as an integer, not the half's bits. Through Arras the depth arrived as zeros, and the
  viewport's grid was drawn over objects. Fixed in the macOS patches.
- **Closing an Arras session** (`moonray-arras-shutdown.patch`, on Linux too): the socket was closed and the endpoint
  deleted before the thread reading them had ended, and an exception in it ended usdcc. Seen once, at a switch away
  after `execComp` had been killed. Afterwards 80 switches, with `execComp` killed at every third, passed.

**In usdcc's viewport,** by script, in the development build: the edits, camera moves, resizing and renderer switches of
§7.2, both delegates the same picture, with a PNG texture, metal, glass, instances and curves; the grid hidden by
objects; the longest pause of the UI thread 0.33 s. `execComp` killed three times: usdcc goes on and the next edit starts a new
one. None is left after quitting. Direct light and shadows agree with Embree's.

**In the app:** `usdcc.app` with the pack, signed ad hoc, with the build and OpenUSD moved away: the smoke test
passes, both delegates render, and the signature is still whole afterwards. With the hardened runtime on
too, through Arras.

**Shipping:** files can't be unpacked over a signed bundle, so the release has a second disk image,
`usdcc-<version>-macos-arm64-moonray.dmg`: usdcc with the pack (USD_PLUGINS.md §5).
It was 371 MB here, and 160 MB without; the pack is 291 MB installed, 224 MB zipped.

**The first start** of a newly installed MoonRay can take several seconds (5 to 17 s seen), while macOS
checks its libraries, and blocks the UI. Later ones take 0.3 s.

**Licences** beyond Linux's: FreeType's (FTL), and sse2neon's MIT notice, whose header is compiled in.

**Not done:**

- CI hasn't built it: Xcode 16 on `macos-15` is untried.
- The published macOS OpenUSD asset links Homebrew libraries of the machine it was built on, so MoonRay doesn't load over
  it. Tested over a locally built OpenUSD instead. It needs the asset's fix (pull request 87).
- The crash scenes of §7.3, light linking, VDB, and real scenes haven't been tried here. No Metal XPU mode.

### 7.7 Results of phase 5, nodes (2026-11-25)

In WSL, over the published Linux asset. **A DwaBaseMaterial network made with the Material Editor's
actions renders in MoonRay**, through Arras and in-process; a renderer without MoonRay's nodes (Embree)
shows the same material's UsdPreviewSurface.

- **The port,** `patches/moonray-sdr-26.08.patch` over `moonray_sdr_plugins`, a project of the hook now.
  Ndr's names became Sdr's, and no Boost. Beyond that, it changes what the editor would have got wrong:
  - A bool's default was the opposite of the class's. Bools are USD's bool, colors are `color3f`, the double
    vectors and Mat3f have USD's types, and RgbaVector is read as such.
  - Fifteen Dwa and five hair materials have a class type of their own (`DwaBaseLayerable`,
    `DwaBaseHairLayerable`): they had no surface context and no output. They are surfaces now, and normal maps
    are patterns with a terminal output.
  - A material, normal map, displacement, volume or map given as an object (`SceneObject*`) couldn't be connected.
    It is a terminal input now. Cameras, light sets, trace sets and geometry still can't.
  - The classes are found beside the plugin (`<root>/coredata`) when `MOONRAY_CLASS_PATH` is empty: Sdr may look
    for them before hdMoonray, which sets MoonRay's other variables, is loaded.
- **What ships:** 102 of the 186 class descriptions: materials, maps, normal maps, displacements, volumes. Not the
  lights, cameras and the rest, the tests', nor **MoonRay's own `Usd*` classes** (§5.3): they have USD's names and
  Sdr could have given them for USD's. hdMoonray makes them by name and doesn't need a description.
- **Contexts and source types** (§5.4): the hdMoonray patch gives `{"moonray", ""}` and `{"moonrayClass"}`.
- **Multiple outputs** (§5.5): none. Every class has `out`; hdMoonray's one object per output is for
  UsdUVTexture's channels.
- **The profile,** `moonrayRendererProfile()` in
  [shader_node_catalog.cpp](../src/cpp/usd/src/shader_node_catalog.cpp), added at start where the parser plugin
  is. Displacements and volumes have no groups: there are few.
- **The check** asks Sdr for five of the nodes and renders `test_data/moonray/nodes.usda`.
- **Tests:** `shader_catalog_*`, and `shade_actions_moonray_network`, which runs where the pack is (not in CI's test
  jobs: they have no optional packs). `USDCC_TEST_MOONRAY_SCENE` keeps the scene it makes.

**Not tried:** the editor's panel by hand, macOS (CI builds and checks the pack there), each of the 102 classes (two
were rendered: `DwaBaseMaterial` and `CheckerboardMap`), hair, volumes and displacement.

### 7.8 Results of phase 6, settings and final renders (2026-11-26)

In WSL, usdcc built from this branch over the published Linux asset and the pack. **A final frame is
written**, with each delegate, through `tests/moonray_final_render_check.py` (§7.7's scene, by hand):

| | MoonRay (Arras) | MoonRay (in-process) |
|---|---|---|
| A frame, color and depth AOVs, EXR and PNG | yes, 1.4 s at 320 × 200 | yes, 1.3 s |
| 2 × 2 tiles | a whole frame (below) | the same |
| Frames 1 to 3, the plane turning | yes, three files | yes |
| Time limit 3 s, 4096 pixel samples | stopped at 3.1 s, with an image | **ended at 0.6 s**: its progressive render says it's done after its passes, whatever the samples |
| Cancel after 1 s | cancelled, no file | finished first, as above |
| A render after the cancel | yes | yes |

No `execComp` was left after the app quit.

- **Tiles:** hdMoonray renders the whole view at the size of the framing's data window (`RenderPass.cc`),
  which a tile can't use: the read-back failed ("The renderer gave no image back"). Both MoonRay delegates
  say they don't render regions (`rendersRegions()` in
  [hydra_final_renderer.cpp](../src/cpp/usd/src/render/hydra_final_renderer.cpp)), so a job renders their
  frames whole, as it does for any renderer without tiles.
- **Settings** (Settings > Renderers, and the Render Settings panel's MoonRay tab):
  - added, at MoonRay's own defaults: sampling mode, pixel, light and BSDF samples, min and max adaptive
    samples and the target error, and four ray depths. hdMoonray reads any `moonray:sceneVariable:<name>`,
    but lists none, and usdcc passes on only what a renderer lists. On the prim they are
    `moonray:sceneVariable:pixel_samples` for MoonRay and `moonrayDebug:moonray:sceneVariable:pixel_samples`
    for the in-process one.
  - left out: remote hosts, their count and connect retries. The pack has no Arras coordinator.
  - fixed: Decode Normals read `HDMOONRAY_DOUBLESIDED`; labels ("RDLA Output", "Double Sided",
    "Arras Log Level (1-5)", ...).
  All of it is in the hdMoonray patch, but the hidden ones (`isUnusedRendererSetting()`).

**Not tried:** the panels by hand, a real scene, denoising (Open Image Denoise is in the pack), macOS, and
whether the in-process renderer takes the sample counts.

## 8. Decisions

| # | Question | Recommendation |
|---|---|---|
| 1 | Platform | **Decided:** Linux x64, and macOS on Apple silicon (§7.6) |
| 2 | Standard or separate download? | **Decided:** separate, `usdcc-<version>-linux-x64-moonray.tar.xz` (§7.5) |
| 3 | Rebuild the OpenUSD asset's Embree | **Not needed** (§7.1) |
| 4 | Before or after Cycles? | Cycles first if one renderer with MaterialX on every platform is the goal |
