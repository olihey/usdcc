# CI

usdcc's continuous integration on GitHub Actions. Part of M0 (docs/PLAN.md §6 item 2,
§8). **Windows and Linux.** Each workflow has a `windows` job and a `linux` job
(`ubuntu-24.04`, GCC 13): §8. macOS, USD plugins (file formats, render delegates) and
self-hosted runners come later (§7).

## 1. Decisions

- **GitHub-hosted runners.** `olihey/usdcc` is public, so hosted minutes are free and
  unlimited. A hosted runner starts clean every time, which is what M0's exit criterion
  ("a fresh clone builds") needs. Self-hosted runners on a public repo would run fork PRs'
  code on our own machines, so they wait (§7).
- **Everything lives in this repo.** No OpenUSD fork: the OpenUSD pin, its one patch and
  the build flags sit in `deps/`, next to the code that depends on them, so a USD bump
  and the usdcc fixes it needs land in one commit.
- **OpenUSD is built once and reused.** A slow, rarely run workflow builds it and
  publishes the install as a GitHub Release asset. The everyday workflow downloads that
  asset instead of building USD.
- **Pure OpenUSD only.** No file format plugins or extra render delegates yet, and no
  PySide6 until phase 4. *Since then: PySide6 (§4.1), and file format plugins as packs
  over the asset ([USD_PLUGINS.md](USD_PLUGINS.md)).*

## 2. The OpenUSD build

The same build as the local one (`C:\Users\olive\DEV\git\USD\build\usd_typhoon`), with
one difference: `--no-usdview`.

| | |
|---|---|
| Source | `https://github.com/NVIDIA-Omniverse/OpenUSD`, commit `70c45e83b08a7b45588cd6cdbd2ae67f5147befb` (branch `typhoon/main`; reports itself as 26.08) |
| Patch | `deps/patches/build_usd-glm.patch`: adds GLM to `build_usd.py`, which `--embree` needs. Taken from the local checkout's uncommitted change |
| Flags | `--no-examples --no-tutorials --no-usdview --materialx --openimageio --opencolorio --embree --build-variant=release --generator="Visual Studio 17 2022"` |
| Python | 3.13.12 x64 from `actions/setup-python`. 3.13 is the VFX Reference Platform's Python for CY2026 and CY2027; it was 3.12.10 until 2026-11-17 (§2.1) |
| Compiler | MSVC from Visual Studio 2022, like usdcc (`tools/build.py` uses the `Visual Studio 17 2022` generator), on the `windows-2022` runner image. Never `windows-latest`: `windows-2025` moved to Visual Studio 2026 in June 2026 |
| Install path | `C:\deps\usd` on CI. The asset itself works from any folder |

**Why `--no-usdview`.** usdview needs a pip-installed PySide6 on the build machine. usdcc
must never mix pip PySide6 into its process (PLAN §6 item 11), and it doesn't use usdview.

**Making it relocatable.** `build_usd.py` writes absolute paths into some CMake files, so
an install only works where it was built. In a local build they are in 12 files:
`pxrConfig.cmake` (the TBB target, `MaterialX_DIR`, `Imath_DIR`, the Python paths),
`cmake/pxrTargets.cmake` (an OpenColorIO library, the Python include folder), the boost
package files and OpenImageIO's. `tools/ci/relocate_usd.py` rewrites them before zipping:

- The install folder becomes `${_usdcc_usd_prefix}`, which a line added at the top of each
  changed file works out from `${CMAKE_CURRENT_LIST_DIR}`.
- `pxrConfig.cmake` uses its stored Python paths only if they exist on the machine;
  otherwise CMake finds a Python of that version itself. So CI, with the same Python as
  the build, needs no `-DPython3_EXECUTABLE`, and a plugin author's own Python 3.13
  install works. That lookup only asks for Python's libraries, and usdcc also needs the
  interpreter to embed, so usdcc's `FindUSD.cmake` first looks for the machine's Python
  of the same version (in the registry, then on PATH) and sets the paths as
  `pxrConfig.cmake` would.
- The Python include folder in `pxrTargets.cmake` becomes `${Python3_INCLUDE_DIRS}`, which
  `pxrConfig.cmake` has found by then.

It then fails if any CMake file still names the build folder, or names the Python
folder outside those guarded lines. Tested on a copy of the local install: 12 files
rewritten, and the SDK check (§3) built and passed against the copy.

**Consumers put the install on `CMAKE_PREFIX_PATH`**, not just `-Dpxr_DIR`:
`pxrConfig.cmake` finds OpenSubdiv and TBB with plain `find_dependency()` calls that only
look there. usdcc's `FindUSD.cmake` already does this.

### The asset is also the OpenUSD SDK for plugin authors

[PLUGIN_SDK.md](PLUGIN_SDK.md) tells C++ plugin authors that OpenUSD isn't in usdcc's SDK
and that they need the one the app was built with (`-DUSD_INSTALL=<path>`). The OpenUSD
asset is exactly that, so it keeps everything needed to compile, link and run against
it:

| Kept | For |
|---|---|
| `include/` | `pxr/` headers and the dependencies' headers (TBB, boost, MaterialX, Imath, OpenImageIO, OpenColorIO, Embree, ...) |
| `lib/*.lib` | Import libraries to link against |
| `pxrConfig.cmake`, `cmake/`, `lib/cmake/*` | CMake package files for OpenUSD and its dependencies |
| `bin/`, `lib/*.dll`, `lib/usd/`, `plugin/usd/` | Running and testing a plugin |
| `lib/site-packages/`, `bin/usdGenSchema` | Python bindings and schema generation for schema plugins (`usdGenSchema` also needs `jinja2` in the Python that runs it) |

Only `src/` (the dependencies' downloaded archives and unpacked sources, and
`build_usd.py`'s log) and `build/` (their intermediate CMake build trees) are dropped.
Neither is used when compiling against the install.

Plugins must match how OpenUSD was built: **Release only**, so a plugin uses the release
CRT (`/MD`) and `_ITERATOR_DEBUG_LEVEL=0` in every configuration, as usdcc itself does (top-level
`CMakeLists.txt`), or the STL objects it passes to OpenUSD have the wrong layout. Plus the
same MSVC toolset generation and Python 3.13.

**Debug symbols.** `--build-variant=release` produces no `.pdb` files for OpenUSD itself;
the only ones in the install are TBB's (12 small files), which stay in the zip. Stepping
into OpenUSD would need a separate symbols build (`--build-variant=relwithdebuginfo`,
same CRT and ABI) as its own asset. Not planned yet. The sources are public at the
pinned commit.

### 2.1 Python 3.13

`deps/usd.json` pinned Python 3.12.10 until 2026-11-17. It is 3.13 now, the VFX Reference
Platform's Python for CY2026 and CY2027. The pin is the one place the version is written:
the assets' names, the embedded interpreter and the per-version package environments
follow from it.

- **vcpkg no longer installs a Python.** Its `python3` port, which the pybind11 port
  depended on, has no version after 3.12, and its CMake wrapper forces that version on
  every `find_package(Python3)`, so an OpenUSD built with 3.13 wasn't found at all. usdcc
  only uses pybind11's headers, so the overlay port in `cmake/ports/pybind11` has no
  `python3` any more (it already had none on Linux), and pybind11's own Python search is
  off (`PYBIND11_NOPYTHON`).
- **Every other Python lookup is pointed at OpenUSD's.** Shiboken6's and PySide6's CMake
  files call `find_package(Python)` themselves. vcpkg's wrapper used to answer that with
  its 3.12; without it CMake takes the newest Python on the machine (3.14 here), headers
  included. The top-level `CMakeLists.txt` now sets `Python_EXECUTABLE` to OpenUSD's
  interpreter before they run.
- **Tried on Windows before the pin changed,** with a locally built OpenUSD and PySide6
  for 3.13.12 and a vcpkg tree without the Python: OpenUSD at the pinned commit builds
  with the pin's flags, `tools/build_pyside.py` builds PySide6 6.11.1, the Adobe file
  format pack builds, and usdcc builds and passes all nine test programs. The same
  sources still build and pass against the 3.12 OpenUSD, so a local build with an older
  OpenUSD keeps working. The V-Ray App SDK installed here has a module for 3.13, which
  imports.
- **Not tried:** Linux. `tools/build_usd.py` now needs a Python 3.13 to run on, and
  Ubuntu 24.04's own is 3.12 ([LINUX_WSL.md](LINUX_WSL.md)). The Blender Linux
  distribution's tooling and review name Ubuntu's Python 3.12
  (`tools/blender_linux_distribution.py`) and have to be redone for another Python.
  Also not tried: a Debug build, a deploy with its smoke test, and the Python
  `actions/setup-python` installs, as the trial used a uv-managed one.
- **Every asset has a new name** (`...-py313`), so `deps-usd.yml` has to publish the new
  OpenUSD (about two hours a platform) before `build.yml` can pass again, and
  `deps-pyside.yml` and `deps-usd-plugins.yml` follow it.

### `deps/usd.json`

One file pins the build:

```json
{
  "repository": "https://github.com/NVIDIA-Omniverse/OpenUSD",
  "commit": "70c45e83b08a7b45588cd6cdbd2ae67f5147befb",
  "patches": ["patches/build_usd-glm.patch"],
  "flags": ["--no-examples", "--no-tutorials", "--no-usdview", "--materialx",
            "--openimageio", "--opencolorio", "--embree", "--build-variant=release",
            "--generator=Visual Studio 17 2022"],
  "python": "3.13.12",
  "platform": "win64"
}
```

The asset's name comes from it: `usd-<short commit>-<hash>-win64-py313.zip`, where
`<hash>` is the first 8 hex characters of a SHA-256 over `usd.json` and the patch files
(line endings normalised). `tools/ci/usd_asset.py` prints it, with the tag. Any
change to the pin, the flags or a patch gives a new name, so a stale asset is never used.
It's published under the tag `deps/usd-<short commit>-<hash>` as a **prerelease**, so it
stays out of the way of real usdcc releases.

## 3. Workflow `deps-usd.yml`

[.github/workflows/deps-usd.yml](../.github/workflows/deps-usd.yml). Runs on
`workflow_dispatch`, and on pushes to `master` that change `deps/`, its scripts or
itself. It skips the build if the asset for the current hash is already published.

1. Name the asset (`tools/ci/usd_asset.py`) and check whether it's published.
2. Set up Python; fetch OpenUSD at the pinned commit (a shallow fetch of that one
   commit, without line-ending conversion) and apply the patches with `git apply`.
3. `python build_scripts/build_usd.py <flags> --src C:\b\src --build C:\b\build C:\deps\usd`.
   The downloads and build trees go outside the install. On failure, `build_usd.py`'s
   logs are uploaded as an artifact.
4. Make it relocatable (`tools/ci/relocate_usd.py`, §2).
5. Zip it and check it's under GitHub's 2 GiB per-asset limit. The local install is
   ~830 MB unzipped.
6. **Check it as an SDK:** delete `C:\deps\usd`, unzip the asset into another folder, and
   build and run `tools/ci/usd-sdk-check` against it. That's a minimal OpenUSD file format
   plugin plus a program that loads it through `PlugRegistry` and opens a file with it,
   so a path the relocation missed fails here instead of on a plugin author's machine.
7. Publish with `gh release create --prerelease`, with the pin, flags, Python, runner
   image and run link in the release notes.

Measured on the first run (`windows-2022`, 4 cores): `build_usd.py` took 2 h 1 min and
the whole job 2 h 3 min, inside the 6-hour job limit. The runner started with 79 GB free on
`C:` (and 220 GB on `D:`), which was enough. The zip is 216 MB, far under the 2 GiB limit.

## 4. Workflow `build.yml`

Runs on pushes to `master` and on pull requests.

1. Check out usdcc with submodules (`external/vcpkg`, `external/qwindowkit`), then
   fetch the vcpkg submodule's full history. `actions/checkout` clones submodules at
   depth 1, which is enough for ports at the baseline, but a manifest's `overrides`
   make vcpkg check out the port's tree from an older commit (the Blender mesh
   backend's `vcpkg.json` pins `eigen3` 3.4.0). With a shallow clone that fails with
   `read-tree <tree> failed` / `failed to unpack tree object`.
2. Set up the same Python as `deps-usd.yml`.
3. Work out the asset name from `deps/usd.json` (the same script as in §3), download it
   with `gh release download`, and unzip it to `C:\deps\usd`. If it's missing, fail with
   a message saying to run `deps-usd.yml`.
   Then `tools/fetch_usd.py --packs-only` unzips the USD plugin packs of the standard set
   over it ([USD_PLUGINS.md](USD_PLUGINS.md) §4). One that isn't published is a warning,
   and the build goes on without it: `deps-usd-plugins.yml` may still be building it.
   Windows only, as the packs are.
4. The same for PySide6 (§4.1), unzipped into `external/`, where the CMake configure
   looks for it. If it's missing, the build carries on without `usdcc.ui` and the Python
   panel tests, with a warning: after a Qt update, `deps-pyside.yml` needs a few minutes
   to catch up, and the build shouldn't fail in the meantime.
5. **vcpkg binary cache.** `VCPKG_BINARY_SOURCES=clear;files,<dir>,readwrite`, with that
   directory saved by `actions/cache`. `tools/ci/vcpkg_cache.py` sets it up and names the
   keys, for this workflow and `deps-pyside.yml`. vcpkg's old `x-gha` cache backend is gone, so it's
   the `files` backend plus `actions/cache`. The first run builds Qt (an hour or more), and
   later runs restore it in minutes.
   - The cache holds one zip per package build, named after the package's ABI hash,
     which covers its port, its dependencies and the compiler.
   - Restored: the newest cache for the same dependencies (`vcpkg.json`, the overlay
     ports in `cmake/ports` with `vcpkg-configuration.json`, and the vcpkg submodule
     commit), or else the newest of any. So after a dependency change or a runner image
     update only what changed is built.
   - Saved whenever vcpkg built something, even if usdcc's build or tests failed after it,
     under a key of the run's own. An earlier version used one key per set of
     dependencies, and a run that stopped halfway would have left that key behind,
     with later runs hitting it, rebuilding the rest and never saving it.
   - Pruned after a successful build: each installed package's `vcpkg_abi_info.txt`
     hashes to its zip's name, and the zips no package used are dropped. Otherwise an
     old compiler's or a replaced port's packages would be carried from cache to cache.
     Every install tree and triplet counts: `build/vcpkg_installed/*` (on Linux Qt's
     host tools are built for `x64-linux`, beside `x64-linux-dynamic`) and the Blender
     mesh backend's `external/blender-work/build/vcpkg_installed/*`. Counting only the
     main triplet dropped Linux's host qtbase after every build, so each run rebuilt it
     (about 35 minutes), and Windows rebuilt the backend's eigen3, fmt and pthreads.
   - Only from pushes, not pull requests: a pull request's caches would crowd `master`'s
     out of the repository's 10 GB, and pull requests can restore `master`'s.
6. Configure and build with `tools/build.py --usd-install C:\deps\usd`, in
   `RelWithDebInfo` like local builds.
7. Run the tests: `ctest --verbose --timeout 900`, so the log shows every test case and
   the check counts, and a crash waiting behind an error dialog ends after 15 minutes.

### 4.1 Workflow `deps-pyside.yml`

PySide6 and shiboken6 can't come from pip: their Qt is a different build from vcpkg's, and
two Qts in one process fail at random (`src/cpp/ui/python/CMakeLists.txt`). So they're
built from source against vcpkg's Qt, as `tools/build_pyside.py` does locally, and
published like OpenUSD.

- **Name.** `tools/ci/pyside_asset.py`: `pyside-<tag>-qt<version>-<hash>-win64-py313`,
  tag `deps/pyside-<tag>-qt<version>-<hash>`. The hash covers the PySide tag, qtbase's
  vcpkg version and port version (an overlay port in `cmake/ports` wins), the Python
  version from `deps/usd.json`, `build_pyside.py` and the naming script itself. Other
  `vcpkg.json` changes, such as adding QtNodes, don't need a new build.
- **Runs** by hand, or on a push that changes one of those inputs. It skips the build when
  the asset exists.
- **Steps.** Restore `build.yml`'s vcpkg cache, read-only; `vcpkg install` into
  `build/vcpkg_installed`, where `build_pyside.py` looks for Qt; run `build_pyside.py`
  for the Python from `setup-python`; zip `external/pyside-install` with
  `external/libclang/lib/clang` (clang's headers, which the generator parses usdcc's
  header with); check the zip from another folder with vcpkg's Qt deleted (PySide6
  imports, `shiboken6 --version` runs); publish as a prerelease.
- **Contents.** `pyside-install/` holds PySide6 (Core, Gui and Widgets, with the Qt DLLs
  it was built with), shiboken6 and the shiboken6 generator with its `libclang.dll`. The
  generator's CMake files name the build machine's libclang and Qt folders, but only as
  information; it runs from its own folder.

## 5. Phases

1. **OpenUSD asset.** `deps/usd.json`, the patch, `tools/ci/usd_asset.py`,
   `tools/ci/relocate_usd.py`, `tools/ci/usd-sdk-check` and `deps-usd.yml`;
   PLUGIN_SDK.md points plugin authors at the asset. Done when the first asset is
   published, which `deps-usd.yml` only does after the SDK check passed against it in
   another folder. *Done: the first run published
   [`deps/usd-70c45e83b-99f25090`](https://github.com/olihey/usdcc/releases/tag/deps/usd-70c45e83b-99f25090).*
2. **usdcc builds in CI.** `build.yml` up to step 5, with the vcpkg cache, without
   PySide6 (the CMake configure already turns off `usdcc.ui` and Python-made panels
   without it). Done when `master` is green and a second run restores Qt from the cache.
   *Done. The first run took 1 h 23 min: vcpkg 74 min (Qt alone 41 min), usdcc 8 min,
   and a 629 MB cache. The second restored all 37 packages in 11 s and took 11 min. A
   local configure against the published asset is what turned up `FindUSD.cmake`'s Python
   lookup above.*
3. **Tests in CI.** `ctest` for `usdcc_tests`, `usdcc_ui_tests` and
   `usdcc_python_tests`; the UI tests already run with `QT_QPA_PLATFORM=offscreen`.
   Hosted runners have no GPU, so tests that need Hydra/OpenGL (the viewport, picking,
   gizmos) may fail there. Find them, give them a `gpu` CTest label, and run
   `ctest -LE gpu` in CI. Done when the test step is green and every excluded test is
   labelled.
   *Done, without a `gpu` label: none of the tests uses the GPU. The viewport only
   creates its GL context and Hydra engine when its window is exposed, which on Qt's
   offscreen platform never happens, so the viewport tests cover its camera, layout and
   events but never render, on CI or locally. Rendering would need a test that's run on
   a real platform. The first runs found one timing race: `usda_view_test` checked
   Find's 200 ms debounce after a fixed 400 ms and failed when the runner was busy, so
   it now waits for the result. Since then, two runs in a row were green (134, 112 and
   56 test cases), with the test step taking under a minute. Without PySide6, the six
   test files for Python-made panels and the render plugins aren't built (phase 4).*
4. **PySide6.** A second deps asset built by `tools/build_pyside.py` against vcpkg's
   Qt, keyed on the vcpkg baseline, the pyside-setup tag and the Python version, so it
   always matches the Qt usdcc builds against. With it, `usdcc.ui` and the Python panel
   tests build and run in CI.
   *Done (§4.1). The first run published
   [`deps/pyside-6.11.1-qt6.11.1-c0e9afd2`](https://github.com/olihey/usdcc/releases/tag/deps/pyside-6.11.1-qt6.11.1-c0e9afd2),
   75 MB, in 7 minutes: Qt came from the cache and PySide's build of Core, Gui and Widgets
   is small. With it, `build.yml` builds `usdcc._ui` and the Python tests went from 56 to
   71 test cases, all green, including the then-current Python render plugins.*

Native RTX now has SDK-independent stage/navigation/controller tests using a fake
engine and presentation, included in normal CI with `--without-rtx`. V-Ray keeps
its helper tests. `rtx-gpu.yml` is a manually dispatched workflow on a prepared
self-hosted Windows RTX runner: it builds pinned native SDKs, runs the labeled
GPU tests in all three modes and CUDA/Vulkan interop, and validates a packaged
application with isolated PATH/profile. See [RTX_RENDER.md](RTX_RENDER.md).
GPU tests are explicitly opt-in and are not counted as passing in hosted CI.

## 6. Open questions

| # | Question | Assumption until decided |
|---|---|---|
| 1 | Pin NVIDIA's `typhoon/main` or a Pixar release tag? `typhoon/main`'s latest commit is "add on-hold banner", which suggests that branch may be paused | Pin the commit the local build uses, so CI matches it; revisit when bumping USD |
| 2 | PLAN §5 item 2 says OpenUSD 26.11 is in use, but the local build reports 26.08 | Treat 26.08 (the commit above) as the truth and correct PLAN §5 when the pin is settled |
| 3 | Is a symbols build of OpenUSD (§2, "Debug symbols") worth its own asset? | Not until someone needs to step into OpenUSD |

## 7. Later

- **macOS and Linux arm64.** Hosted `macos-latest` and `ubuntu-24.04-arm`, each with its
  own OpenUSD asset, as the Linux x64 one (§8). Still Windows only: `tools/build_pyside.py`,
  so Linux has no PySide6 (no `usdcc.ui`, no Python-made panels, no V-Ray Render).

- **USD plugins.** A second layer on top of the OpenUSD asset (file format plugins,
  render delegates), built against it and unzipped over it, so `tools/deploy.py` picks
  them up from `plugin/usd`. Autodesk's FBX SDK can't be republished in a public release.
  Planned in [USD_PLUGINS.md](USD_PLUGINS.md).
- **Deploy artifact.** Upload `tools/deploy.py`'s output once M11 lands. Releases are
  planned in [RELEASE.md](RELEASE.md): a GitHub release gets its Windows 7z from
  `release.yml`.
- **Self-hosted runners** (the Windows PC, the Mac) for faster incremental builds, only
  on `push` and `workflow_dispatch`, never on pull requests from forks.

## 8. Linux

The `linux` jobs mirror the `windows` ones on `ubuntu-24.04` ([LINUX_WSL.md](LINUX_WSL.md)
has how Linux was brought up, in WSL 2):

- **`deps-usd.yml`** builds OpenUSD with `tools/build_usd.py` (the steps the `windows` job
  runs inline, as a script) against `actions/setup-python`'s CPython of the pinned version.
  `tools/ci/relocate_usd.py` also rewrites the ELF files' RUNPATHs, which name the build
  folder, relative to `$ORIGIN` (patchelf). The zip (`--symlinks`) is checked as an SDK from
  another folder with a clean environment, and goes into the same release as the Windows
  one: `tools/ci/usd_asset.py` names each platform's zip after the platform it runs on, so
  `deps/usd.json`, and the Windows asset's name, are unchanged. Either job may make the
  release; the other adds its zip, and the `windows` job writes the notes.
- **`build.yml`** builds against it with the same CPython at the same path (pxrConfig.cmake
  names it), with the standard USD plugin packs for Linux unzipped over it
  ([USD_PLUGINS.md](USD_PLUGINS.md) §4), and runs the tests under `xvfb-run` with llvmpipe. vcpkg's cache is the
  `windows` job's scheme with the Linux triplet in its keys.
- **`release.yml`** deploys with `tools/deploy.py`'s Linux layout (a prefix of its own,
  RUNPATHs relative to `$ORIGIN`, the CPython bundled, the standard USD plugin packs,
  which it requires), moves usdcc's debug info out with
  `tools/ci/split_debug.py`, starts the deploy with `tools/ci/smoke_test.py`, and attaches
  `usdcc-<version>-linux-x64.tar.xz` and `...-symbols.tar.xz`.

The hosted runner has 4 cores: OpenUSD takes hours, Qt's first build in vcpkg one or two.
Each job first deletes the image's unused SDKs for disk space.
