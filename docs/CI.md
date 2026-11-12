# CI

usdcc's continuous integration on GitHub Actions. Part of M0 (docs/PLAN.md §6 item 2,
§8). **Windows only for now.** Linux, macOS, USD plugins (file formats, render delegates)
and self-hosted runners come later (§7).

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
  PySide6 until phase 4.

## 2. The OpenUSD build

The same build as the local one (`C:\Users\olive\DEV\git\USD\build\usd_typhoon`), with
one difference: `--no-usdview`.

| | |
|---|---|
| Source | `https://github.com/NVIDIA-Omniverse/OpenUSD`, commit `70c45e83b08a7b45588cd6cdbd2ae67f5147befb` (branch `typhoon/main`; reports itself as 26.08) |
| Patch | `deps/patches/build_usd-glm.patch`: adds GLM to `build_usd.py`, which `--embree` needs. Taken from the local checkout's uncommitted change |
| Flags | `--no-examples --no-tutorials --no-usdview --materialx --openimageio --opencolorio --embree --build-variant=release --generator="Visual Studio 17 2022"` |
| Python | 3.12.10 x64 from `actions/setup-python` (the same patch version as the local build) |
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
  otherwise CMake finds Python 3.12 itself. So CI, with the same Python as the build,
  needs no `-DPython3_EXECUTABLE`, and a plugin author's own Python 3.12 install works.
  Under vcpkg's toolchain, as in usdcc's own build, "finds it itself" would mean vcpkg's
  python3, which can't be embedded (vcpkg's `find_package` wrapper insists on it), so
  usdcc's `FindUSD.cmake` first looks for the machine's Python of the same version (in the
  registry, then on PATH) and sets the paths as `pxrConfig.cmake` would.
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
| `lib/python/`, `bin/usdGenSchema` | Python bindings and schema generation for schema plugins (`usdGenSchema` also needs `jinja2` in the Python that runs it) |

Only `src/` (the dependencies' downloaded archives and unpacked sources, and
`build_usd.py`'s log) and `build/` (their intermediate CMake build trees) are dropped.
Neither is used when compiling against the install.

Plugins must match how OpenUSD was built: **Release only**, so a plugin uses the release
CRT (`/MD`) and `_ITERATOR_DEBUG_LEVEL=0` in every configuration, as usdcc itself does (top-level
`CMakeLists.txt`), or the STL objects it passes to OpenUSD have the wrong layout. Plus the
same MSVC toolset generation and Python 3.12.

**Debug symbols.** `--build-variant=release` produces no `.pdb` files for OpenUSD itself;
the only ones in the install are TBB's (12 small files), which stay in the zip. Stepping
into OpenUSD would need a separate symbols build (`--build-variant=relwithdebuginfo`,
same CRT and ABI) as its own asset. Not planned yet. The sources are public at the
pinned commit.

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
  "python": "3.12.10",
  "platform": "win64"
}
```

The asset's name comes from it: `usd-<short commit>-<hash>-win64-py312.zip`, where
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

1. Check out usdcc with submodules (`external/vcpkg`, `external/qwindowkit`).
2. Set up the same Python as `deps-usd.yml`.
3. Work out the asset name from `deps/usd.json` (the same script as in §3), download it
   with `gh release download`, and unzip it to `C:\deps\usd`. If it's missing, fail with
   a message saying to run `deps-usd.yml`.
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

- **Name.** `tools/ci/pyside_asset.py`: `pyside-<tag>-qt<version>-<hash>-win64-py312`,
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

- **Linux and macOS.** Hosted `ubuntu-24.04`, `ubuntu-24.04-arm` and `macos-latest`, each
  with its own OpenUSD asset. The scripts in `tools/` are Python, so they run there, and
  `tools/build.py` and the naming, relocating and licence scripts in `tools/ci` already
  take the platform into account (`tools/_common.py`). Still Windows only: `deps/usd.json`
  (its `platform` and generator flag), `tools/deploy.py`'s folder layout with the smoke
  tests that check it, and `tools/build_pyside.py`. None of it has run there yet.
- **USD plugins.** A second layer on top of the OpenUSD asset (file format plugins,
  render delegates), built against it and unzipped over it, so `tools/deploy.py` picks
  them up from `plugin/usd`. Autodesk's FBX SDK can't be republished in a public release.
- **Deploy artifact.** Upload `tools/deploy.py`'s output once M11 lands. Releases are
  planned in [RELEASE.md](RELEASE.md): a GitHub release gets its Windows zip from
  `release.yml`.
- **Self-hosted runners** (the Windows PC, the Mac) for faster incremental builds, only
  on `push` and `workflow_dispatch`, never on pull requests from forks.
