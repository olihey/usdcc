# PySide6 on Linux, and arm64 releases

Plan, 2026-11-23. **Phase 1 is done** (olihey/usdcc#68): PySide6 for all four platforms.
**Phases 2 and 3 are written**: `release.yml` builds both architectures on both systems, and
waits for its first run (a dry run, §4). `build.yml` already builds and tests both arm64
platforms ([CI.md](CI.md) §8), and the OpenUSD assets `linux-arm64` and `win-arm64` are
published.

## 1. Decisions

- **PySide6 is built by `tools/build_pyside.py` on every platform**, against vcpkg's Qt.
  Not pip's wheels: they bring a second Qt into the process.
- **Windows arm64 is included.** Porting only Linux would leave `windows-arm64` as the one
  package without `usdcc.ui`.
- **One release asset set per platform, each from its own job.** A failed arm64 job doesn't
  stop the x64 assets.
- **x64 names don't change**, and x64 installs keep Velopack's channel `win`.
  arm64 gets a channel of its own, `win-arm64`.
- **A plugin ships in the standard package only if** its licence fits a BSD-3-Clause
  package, what it needs may be redistributed, and it starts on that platform (§5).

## 2. Phase 1: PySide6 for Linux x64 and arm64, and Windows arm64

What was done:

- **`tools/build_pyside.py`** runs on Windows and Linux, x64 and arm64. libclang 19.1.0 comes
  from a table with a build and SHA-256 per platform, unpacked by CMake. On Linux:
  - vcpkg has Qt's `qtpaths6` only in the host triplet's tree, so a small wrapper gives it
    the dynamic triplet's `qt.conf`;
  - pyside-setup runs in a venv, with its build packages in `external/pyside-pip`: Debian's
    Python allows no `pip install`, and installs into `dist-packages`, where pyside-setup
    doesn't look;
  - no Qt is copied. RUNPATHs lead from `external/pyside-install` to
    `build/vcpkg_installed`, the app's own Qt, and a Qt in the install is an error.
- **`tools/deploy.py`** puts `libpyside6` and `libshiboken6` into `lib/`, the packages into
  `site-packages`, the licences and `SOURCES.txt` lines with the others, and ships V-Ray
  Render. Not with `--with-blender-mesh-tools`, whose Linux review has no PySide.
- **`tools/ci/smoke_test.py`** requires PySide6 everywhere (`--without-pyside` otherwise),
  docks a Python panel, and on Linux fails unless the process has one `libQt6Core.so`
  mapped, the deploy's.
- **`deps-pyside.yml`** has a Windows and a Linux matrix; the jobs share one release.
  `build.yml`'s Linux jobs use the asset when it's there, `release.yml`'s requires it.
- `usdcc.ui` needed no change. V-Ray Render's test checked its helper with a Windows API,
  and its shader cache key named a DLL.

Left:

- **A CI run on all four platforms.** The asset's hash changed, so `deps-pyside.yml` has to
  publish before anything else has PySide6, Windows x64 included: until then `build.yml`
  warns, and a release fails.
- *Visual Studio 2026's headers and libclang 19* on Windows arm64. shiboken turns off the
  STL's Clang version check itself, so what's left is a header Clang 19 can't parse. Qt has
  `windows-vs2022_arm64` builds up to 23.1.1; the version in the table is then per platform.
- V-Ray Render on Linux with a real App SDK.

Done when the four `build.yml` jobs run the `usdcc.ui` tests. *Done: `deps-pyside.yml`
published all four, and PR 68's builds passed with them.*

## 3. Phase 2: the Linux arm64 release

1. *Written:* `linux` is a matrix: `linux` (`ubuntu-24.04`, `x64`) and `linux-arm64`
   (`ubuntu-24.04-arm`, `arm64`), `fail-fast: false`, with the architecture in
   the archive names: `usdcc-<version>-linux-arm64.tar.xz` and `-symbols`.
2. Try `deploy.py`, `split_debug.py` and `smoke_test.py` on the Arm runner first, with
   `release.yml`'s `dry_run` (§4): they have only run on x64.
3. *Written:* Adobe's file formats for `linux-arm64` (§5): the platform is in the pin, and
   `deps-usd-plugins.yml` has a job per pack and platform, x64 and arm64. The pin's change
   renames every platform's pack, so the workflow has to publish them all before a release,
   which requires them.

Done when a prerelease has both Linux archives and the arm64 one starts on a machine that
isn't the runner.

## 4. Phase 3: the Windows arm64 release

*Written:* items 1, 2 and 5; a dry run tries them (below).

1. **Archive first.** A `windows` matrix with `windows-11-arm`, and from `build.yml`: the
   bootstrap Python, the NuGet Python, Visual Studio 2026. `x64` in archive names, in the
   smoke test's `x64-windows` path, and nowhere else in `deploy.py` (only RTX, which a
   release leaves out).
2. **Tools it bundles.** `UsdccRuff.cmake` and `UsdccUv.cmake` take the `win_amd64` wheel
   on any Windows; it would run emulated. Both publish `win_arm64` wheels.
3. **The MSVC runtime.** Confirm the deploy has ARM64 `vcruntime140*.dll` and `msvcp140*.dll`.
   ARM64 Python has no `vcruntime140_1.dll`, and the copy step already skips a missing one.
4. **PySide6**, required, from phase 1.
5. **The installer.** `vpk download|pack|upload` with `--channel win-arm64`, and
   `--runtime win-arm64` on `pack`. Assets become `releases.win-arm64.json` and a
   `win-arm64` setup. The installed app reads the feed of the channel it was packed for,
   so no app change is expected. Two things to prove with a test feed
   (`USDCC_UPDATE_SOURCE`, [UPDATES.md](UPDATES.md) §6) before a real release:
   - an x64 install isn't offered the arm64 package, nor the other way round;
   - the two jobs can upload to one release: package names don't collide, and
     `--merge` is safe when both run at once. Otherwise arm64 `needs: windows`
     with `if: always()`.
   `UsdccVelopack.cmake` already unpacks the ARM64 library. Whether `dotnet` and `vpk` run
   on the Arm image has to be tried; if not, pack in a small x64 job from the uploaded deploy.
   x64 keeps the default channel, `win`, so installs made before arm64 keep reading
   `releases.win.json`.
6. **Adobe's file formats** for `win-arm64`, as in phase 2. *Written, with it.*

Done when a prerelease's arm64 installer installs on an Arm PC, starts, and updates itself
to the next prerelease.

**Trying it: a dry run.** `release.yml` run by hand with an existing release's tag and
`dry_run` builds the branch it was run on, as for that tag's version, through the smoke
test, the archives and the installer, and attaches nothing: the Linux archives and the
Windows installers are kept with the run for three days instead. `vpk download` still reads
the published feeds, to make a delta.

    gh workflow run release.yml --ref <branch> -f tag=26.11.7 -f dry_run=true

Last, for both: `docs/RELEASE.md`, `CI.md` §8, `LINUX_WSL.md` §6, `UPDATES.md` §5, the
readme, the changelog. Hosted Arm runners have no GPU, so a viewport check on real Arm
hardware is by hand.

## 5. What can ship

Ship = in the standard package. Optional = a download of its own. Local = built by a user
who has the SDK.

### usdcc's plugins

| Plugin | Verdict | Platforms | Why |
|---|---|---|---|
| Jolt Physics | **Ships** | All four | MIT, from vcpkg, built and tested on all four in CI |
| HTTP, WebDAV, S3 assets | **Ship** | All four | usdcc's code; libcurl, zlib, pugixml, all permissive. Nothing architecture-specific |
| Viewport Broadcast | **Ships** | All four | usdcc's code on Qt. No encoder SDK in the package |
| UV View | **Ships** | All four | usdcc's code on Qt and OpenUSD's `Hio` ([PRIMVAR_VIEWER.md](PRIMVAR_VIEWER.md) §18) |
| V-Ray Render | **Ships on Windows x64; Linux x64 after phase 1; not on arm64** | x64 | Our BSD code, and V-Ray runs in a helper from the user's own App SDK, so nothing of Chaos's ships. Needs PySide6. On Linux its tests pass, with a stand-in renderer; a real App SDK hasn't been tried there. arm64: only worth it if Chaos has an App SDK for that platform, which this plan couldn't confirm |
| RTX Render (native) | **Could ship without its runtime; Windows x64 only** | Windows x64 | The plugin is ours (BSD) and tells the user what to install. `release.yml` passes `--without-rtx`, because the runner has no ovrtx/ovstage SDK and no GPU. The runtime may **not** ship: NVIDIA proprietary ([RTX_RENDER.md](RTX_RENDER.md)). Pins are `windows-x64` only. First decide how CI gets the SDK, and that the build doesn't embed any of it |
| Blender Mesh Tools | **Optional, as a separate GPL package** | Windows x64, Linux x64 | Links Blender code, so the whole package is GPL-3.0-or-later and travels with its sources. Reviewed, but each review covers one exact build; it names Python 3.12.10 while we ship 3.13, so it needs redoing. arm64 has no review and no lock |
| Measure Example, C++ Example | **Ship as examples** | All four | In `examples/plugins` and `sdk/example`, not loaded by default |

### USD plugin packs

| Pack | Verdict | Platforms | Why |
|---|---|---|---|
| Adobe file formats (glTF, OBJ, PLY, STL, SPZ) | **Ships** | All four | Apache-2.0, under 3 MB, self-contained. arm64 built on the Arm runners since 2026-11-23 |
| Embree render delegate | **Ships** | All four | In the OpenUSD asset. Apache-2.0. The one CPU path tracer that every package has today |
| Cycles | **Should ship next, not ready** | x64 and macOS arm64; not Windows arm64 for now; not Linux arm64 | Apache-2.0. The pack builds for Windows x64, Linux x64 and macOS arm64 ([USD_PLUGINS.md](USD_PLUGINS.md) §7 phase 5). Windows arm64: Cycles' `half.h` doesn't compile with MSVC for ARM64 (Blender uses clang-cl there). Linux arm64: Blender has no prebuilt libraries for it |
| MoonRay | **Later, Linux only** | Linux x64 | Apache-2.0, but no Windows build. Not proven against our OpenUSD, and its libraries' licences are unread. Upstream supports Linux x64 and macOS arm64, not Linux arm64 |
| Aurora | **Don't ship** | Windows x64 at most | Apache-2.0, but built on Autodesk's own USD fork and DXR; unproven with ours. Quiet upstream |
| Gatling | **Not in the standard package; optional after a review** | x64; Linux arm64 possible | GPL-3.0: in-process it makes the whole package GPL. Works and is built in CI, unpublished. The MDL SDK 2026.0.2 has a Linux aarch64 download, none for Windows arm64. Needs a ray tracing GPU |
| Arnold | **Local only** | Windows x64, macOS arm64, Linux x64 (built in a private repository's CI, [ARNOLD_CI.md](ARNOLD_CI.md)) | The SDK is behind an Autodesk login. Its EULA allows embedding only under terms our BSD licence doesn't give, so shipping needs terms of its own. Autodesk names x86-64 for Linux, and no arm64 outside macOS |
| `usdfbx` (Adobe) | **Local only** | x64 | Links Autodesk's FBX SDK statically, so a binary would redistribute it; its licence is unread |
| `usdsbsar` (Adobe) | **Local only** | x64 | Needs Adobe's Substance SDK, which can't be republished |
| Draco | **Can ship** | All four | Apache-2.0. `--draco` in the OpenUSD build, then Draco glTF in Adobe's pack. Costs one OpenUSD rebuild |
| Alembic | **Can ship** | All four | BSD-3-Clause. `--alembic` without HDF5. Same rebuild |
| OpenVDB | **Can ship, with Cycles** | All four | MPL-2.0 up to version 11, Apache-2.0 from 12; either is fine as a separate library, with sources named for the first. `--openvdb`, and volumes in Storm |
| hdPrman | **Don't ship** | None | Needs RenderMan, commercial |
| Radeon ProRender (`hdRpr`) | **Don't ship** | None | No push to its repository since August 2025; unproven with 26.08 |
| OSPRay (`hdOSPRay`) | **Not now** | Unknown | Upstream's state not checked; Embree and Cycles cover the CPU |
| Karma, Redshift, Octane, V-Ray's delegate | **Can't** | None | Commercial, inside their own products and built for those products' USD |

The result, after all three phases:

| Package | Plugins |
|---|---|
| Windows x64 | Jolt, HTTP, WebDAV, S3, Broadcast, V-Ray Render, Embree, Adobe formats (as today) |
| Linux x64 | The same; V-Ray Render and `usdcc.ui` are new |
| Windows arm64, Linux arm64 | The same without V-Ray Render |

Best value after that, in this order: Alembic, Draco and oneTBB in one OpenUSD rebuild;
then Cycles; then RTX Render without its runtime on Windows x64.

## 6. Order and questions

Three pull requests, one per phase. Phases 1 and 2 don't depend on each other; phase 3
needs phase 1.

| # | Question | Assumption |
|---|---|---|
| 1 | May an arm64 release go out without PySide6? | No: wait for phase 1 |
| 2 | arm64 releases made by hand for 26.11.7? | No: start with the next tag |
| 3 | Is there an Arm machine to try packages on? | Unknown. Until there is, mark arm64 packages as untested in the notes |
| 4 | A GPL build (Blender Mesh Tools and Gatling together) as a second release? | Not in this plan |
