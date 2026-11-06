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
| Compiler | MSVC from Visual Studio 2022, like usdcc (`tools/build.ps1` uses the `Visual Studio 17 2022` generator), on the `windows-2022` runner image. Never `windows-latest`: `windows-2025` moved to Visual Studio 2026 in June 2026 |
| Install path | `C:\deps\usd` on CI. The asset itself works from any folder |

**Why `--no-usdview`.** usdview needs a pip-installed PySide6 on the build machine. usdcc
must never mix pip PySide6 into its process (PLAN §6 item 11), and it doesn't use usdview.

**Making it relocatable.** `build_usd.py` writes absolute paths into some CMake files, so
an install only works where it was built. In a local build they are in 12 files:
`pxrConfig.cmake` (the TBB target, `MaterialX_DIR`, `Imath_DIR`, the Python paths),
`cmake/pxrTargets.cmake` (an OpenColorIO library, the Python include folder), the boost
package files and OpenImageIO's. `tools/ci/relocate-usd.ps1` rewrites them before zipping:

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
(line endings normalised). `tools/ci/usd-asset.ps1` prints it, with the tag. Any
change to the pin, the flags or a patch gives a new name, so a stale asset is never used.
It's published under the tag `deps/usd-<short commit>-<hash>` as a **prerelease**, so it
stays out of the way of real usdcc releases.

## 3. Workflow `deps-usd.yml`

[.github/workflows/deps-usd.yml](../.github/workflows/deps-usd.yml). Runs on
`workflow_dispatch`, and on pushes to `master` that change `deps/`, its scripts or
itself. It skips the build if the asset for the current hash is already published.

1. Name the asset (`tools/ci/usd-asset.ps1`) and check whether it's published.
2. Set up Python; fetch OpenUSD at the pinned commit (a shallow fetch of that one
   commit, without line-ending conversion) and apply the patches with `git apply`.
3. `python build_scripts/build_usd.py <flags> --src C:\b\src --build C:\b\build C:\deps\usd`.
   The downloads and build trees go outside the install. On failure, `build_usd.py`'s
   logs are uploaded as an artifact.
4. Make it relocatable (`tools/ci/relocate-usd.ps1`, §2).
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
4. **vcpkg binary cache.** `VCPKG_BINARY_SOURCES=clear;files,<dir>,readwrite`, with that
   directory saved by `actions/cache`. vcpkg's old `x-gha` cache backend is gone, so it's
   the `files` backend plus `actions/cache`. The first run builds Qt (an hour or more), and
   later runs restore it in minutes.
   - The key is the triplet, the runner image (`ImageOS` and `ImageVersion`), `vcpkg.json`,
     the overlay ports (`vcpkg-configuration.json` and `cmake/ports`) and the vcpkg
     submodule commit. The image is in it because its compiler is part of
     every package's ABI hash: with an unchanged key after an image update, vcpkg would
     rebuild everything on every run and never save it.
   - A new key restores the newest older cache (by prefix), so only what changed is
     rebuilt, and saves the result under the new key.
   - Saved even when the build fails, so Qt isn't built again after a usdcc compile
     error. Only from pushes, not pull requests: a pull request's caches would crowd
     `master`'s out of the repository's 10 GB, and pull requests can restore `master`'s.
5. Configure and build with `tools/build.ps1 -UsdInstall C:\deps\usd`, in
   `RelWithDebInfo` like local builds.
6. Run the tests (phase 3).

## 5. Phases

1. **OpenUSD asset.** `deps/usd.json`, the patch, `tools/ci/usd-asset.ps1`,
   `tools/ci/relocate-usd.ps1`, `tools/ci/usd-sdk-check` and `deps-usd.yml`;
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
4. **PySide6.** A second deps asset built by `tools/build-pyside.ps1` against vcpkg's
   Qt, keyed on the vcpkg baseline, the pyside-setup tag and the Python version, so it
   always matches the Qt usdcc builds against. With it, `usdcc.ui` and the Python panel
   tests build and run in CI.

## 6. Open questions

| # | Question | Assumption until decided |
|---|---|---|
| 1 | Pin NVIDIA's `typhoon/main` or a Pixar release tag? `typhoon/main`'s latest commit is "add on-hold banner", which suggests that branch may be paused | Pin the commit the local build uses, so CI matches it; revisit when bumping USD |
| 2 | PLAN §5 item 2 says OpenUSD 26.11 is in use, but the local build reports 26.08 | Treat 26.08 (the commit above) as the truth and correct PLAN §5 when the pin is settled |
| 3 | Is a symbols build of OpenUSD (§2, "Debug symbols") worth its own asset? | Not until someone needs to step into OpenUSD |

## 7. Later

- **Linux and macOS.** Hosted `ubuntu-24.04`, `ubuntu-24.04-arm` and `macos-latest`, each
  with its own OpenUSD asset.
- **USD plugins.** A second layer on top of the OpenUSD asset (file format plugins,
  render delegates), built against it and unzipped over it, so `tools/deploy.ps1` picks
  them up from `plugin/usd`. Autodesk's FBX SDK can't be republished in a public release.
- **Deploy artifact.** Upload `tools/deploy.ps1`'s output once M11 lands.
- **Self-hosted runners** (the Windows PC, the Mac) for faster incremental builds, only
  on `push` and `workflow_dispatch`, never on pull requests from forks.
