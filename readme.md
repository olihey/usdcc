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
- QWindowKit (custom frameless title bar; git submodule at `external/qwindowkit`)
- OpenUSD / Hydra
- Python scripting and plugin support
  - Shiboken6 for Qt/UI bindings
  - `pxr_boost::python` for OpenUSD bindings
  - pybind11 for other C++ bindings
- vcpkg (as a git submodule at `external/vcpkg`)
- CMake

## Building

The submodules (vcpkg, QWindowKit) need to be cloned, and vcpkg bootstrapped, once:

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

OpenUSD is not fetched by vcpkg — vcpkg's own `usd` port hardcodes Python bindings
off, which the spec requires (see [docs/PLAN.md](docs/PLAN.md) §6 item 11). Build
OpenUSD separately with Python support (e.g. Pixar's `build_usd.py`) and pass
`-DUSD_INSTALL=<path-to-usd-install>` (the directory containing `pxrConfig.cmake`)
to enable USD-dependent targets. Until then the app shell builds without it.

### Python bindings (`usdcc.ui`)

`SidePanel` is exposed to Python via Shiboken6, so scripts can add their own side
panels (`usdcc.app.add_side_panel()`) and view panel types
(`usdcc.app.register_view_panel()`, [docs/SCRIPTING.md](docs/SCRIPTING.md) §8).
This requires PySide6 and Shiboken6 built **from source against usdcc's own vcpkg
Qt**, for OpenUSD's Python. A pip-installed PySide6 ships a different,
independently-compiled Qt, and loading both in one process causes intermittent
DLL-load crashes (see [docs/PLAN.md](docs/PLAN.md) §6 item 11). After a first
`tools/build.ps1`, build it once with:

```
tools/build-pyside.ps1
```

It downloads libclang (~700 MB) and pyside-setup and takes 20+ minutes. The result
lands in `external/pyside-install`, where the next CMake configure finds it
(`USDCC_PYSIDE_DIR`) and builds `usdcc.ui`. Without it, everything else builds and
scripts just can't make panels.

### Python bindings (`usdcc.usd`)

`StageManager` is exposed to Python via pybind11 as `usdcc.usd.StageManager`, once
`USD_INSTALL` is set. Its Python-facing methods (`open_stage`, `stage_cache_ids`,
`current_stage_cache_id`, `set_current_stage`, `close_stage`) exchange USD stages
as plain integer ids from `UsdUtilsStageCache::Get()`, not real `Usd.Stage` objects
— passing those directly across the pybind11⇄`pxr_boost::python` boundary corrupts
unrelated USD Python state (see [docs/PLAN.md](docs/PLAN.md) §5 item 11). Retrieve
the actual stage with USD's own bindings:

```python
from pxr import Usd, UsdUtils
import usdcc.usd

manager = usdcc.usd.StageManager()
cache_id = manager.open_stage("scene.usda")
stage = UsdUtils.StageCache.Get().Find(Usd.StageCache.Id.FromLongInt(cache_id))
```

### Settings

Edit > Settings… (`Ctrl+,`) holds the app's preferences, such as the accent color, the UI
scale and the tool shortcuts (Q select, W move, E rotate, R scale). They are saved in
`usdcc.ini`, and Python reads and changes them with `usdcc.settings`. See
[docs/SETTINGS.md](docs/SETTINGS.md).

### Convenience scripts

[tools/build.ps1](tools/build.ps1) and [tools/run.ps1](tools/run.ps1) wrap the
steps above (including the one-time vcpkg bootstrap):

```
tools/build.ps1              # configure + build
tools/run.ps1                # configure + build + launch
tools/run.ps1 -Scene test_data\cube_and_sphere.usda   # ...and open a stage on launch
```

Both accept `-Configuration`, `-Generator`, `-Arch`, `-UsdInstall`, `-Scene`, and
`-Clean`; `build.ps1` additionally takes `-Run` (which is what `run.ps1` wraps).
`-UsdInstall` only needs to be passed once — it's cached by CMake and reused by
later `-Run`/`run.ps1` invocations automatically. See each script's help
(`Get-Help tools/build.ps1 -Full`) for details.

### Deploying

[tools/deploy.ps1](tools/deploy.ps1) copies a build and everything it needs to run
(OpenUSD's DLLs and plugins, Qt, the Python OpenUSD was built with, and the usdcc, pxr
and PySide6 packages) into one folder that runs on a machine without the dev
environment. Zip it to distribute; start it with `usdcc.cmd` or `bin\usdcc.exe`.

```
tools/deploy.ps1                       # into .\deploy
tools/deploy.ps1 D:\dist\usdcc         # into another folder
tools/deploy.ps1 -Configuration Release
tools/deploy.ps1 -UsdInstall C:\usd\install   # OpenUSD from there instead of the build's
```

Build first. An earlier deploy in the folder is replaced; any other non-empty folder is
refused.

## Repository Layout

```
docs/
  PLAN.md
  changelog.md
CMakeLists.txt
external/vcpkg/
external/qwindowkit/
src/
  cpp/
    core/
    ui/
    usd/
    tools/
  python/
    (mirrors src/cpp: usdcc.core, usdcc.ui, usdcc.usd, usdcc.tools)
  plugins/
test_data/
readme.md
```
