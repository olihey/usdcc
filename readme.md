# usdcc

Cross-platform C++20 digital content creation (DCC) tool that manages multiple scenes
via OpenUSD stages and displays them through Hydra viewports.

## Status

Milestone M0 (repository & build bootstrap) is in progress: the app builds and
launches an empty window on Windows. See [docs/PLAN.md](docs/PLAN.md) for the
full requirements, architecture guidelines, open questions, and milestone plan.

## Technical Stack

- C++20
- Qt6 (UI), Qt Advanced Docking System
- OpenUSD / Hydra
- Python scripting and plugin support
  - Shiboken6 for Qt/UI bindings
  - `pxr_boost::python` for OpenUSD bindings
  - pybind11 for other C++ bindings
- vcpkg (as a git submodule at `external/vcpkg`)
- CMake

## Building

The vcpkg submodule needs to be cloned and bootstrapped once:

```
git submodule update --init --recursive
external/vcpkg/bootstrap-vcpkg.bat   # bootstrap-vcpkg.sh on Linux/macOS
```

Then configure and build (Windows/Visual Studio shown; substitute a generator
like Ninja on Linux/macOS):

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

The first configure builds Qt6 and pybind11 from source via vcpkg, which can
take significant time without a populated binary cache.

OpenUSD is not fetched by vcpkg — it's expected to be a separately built/installed
OpenUSD tree. Pass `-DUSD_INSTALL=<path-to-usd-install>` (the directory containing
`pxrConfig.cmake`) to enable USD-dependent targets once they exist (milestone M2).
Until then the app shell builds without it.

### Convenience scripts

[tools/build.ps1](tools/build.ps1) and [tools/run.ps1](tools/run.ps1) wrap the
steps above (including the one-time vcpkg bootstrap):

```
tools/build.ps1              # configure + build
tools/run.ps1                # configure + build + launch
```

Both accept `-Configuration`, `-Generator`, `-Arch`, `-UsdInstall`, and `-Clean`;
`build.ps1` additionally takes `-Run` (which is what `run.ps1` wraps). See each
script's help (`Get-Help tools/build.ps1 -Full`) for details.

## Repository Layout

```
docs/
  PLAN.md
  changelog.md
CMakeLists.txt
external/vcpkg/
src/
  cpp/
    core/
    ui/
    usd/
    tools/
  python/
    (mirrors src/cpp: usdcc.core, usdcc.ui, usdcc.usd, usdcc.tools)
  plugins/
readme.md
```
