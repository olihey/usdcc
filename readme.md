# usdcc

Cross-platform C++20 digital content creation (DCC) tool that manages multiple scenes
via OpenUSD stages and displays them through Hydra viewports.

## Status

Early planning stage — no source tree yet. See [docs/PLAN.md](docs/PLAN.md) for the
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

Build instructions will be added once the CMake bootstrap (Milestone M0 in
[docs/PLAN.md](docs/PLAN.md)) lands.

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
