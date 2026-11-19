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

Mesh tools are built and enabled in every build. Python 3.12+ builds the pinned
private Blender backend before configuring usdcc. `python tools/build.py` handles
both builds; for manual builds (Windows/Visual Studio shown; use Ninja on Linux):

```
python tools/build_blender_mesh.py --fetch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo --parallel 4
```

The **Mesh** menu offers triangulate, merge, dissolve, normals, decimate and bevel.
See [the mesh tools plan](docs/BLENDER_MESH_TOOLS.md) for supported inputs, build
results and the separate combined-distribution license/source requirements.

`--parallel` builds several projects at once; each compiles its files in parallel too.
The main libraries and tests use precompiled headers for OpenUSD and Qt
(`cmake/pch/`). `-DUSDCC_USE_PCH=OFF` turns them off, which is worth a build now and then
to check that every file still includes what it uses.

The first configure builds Qt6 and pybind11 from source via vcpkg, which can
take significant time without a populated binary cache.

OpenUSD is not fetched by vcpkg — vcpkg's own `usd` port hardcodes Python bindings
off, which the spec requires (see [docs/PLAN.md](docs/PLAN.md) §6 item 11). Build
OpenUSD separately with Python support (e.g. Pixar's `build_usd.py`) and pass
`-DUSD_INSTALL=<path-to-usd-install>` (the directory containing `pxrConfig.cmake`)
to enable USD-dependent targets. Until then the app shell builds without it.

On Windows there is no need to build it: `python tools/fetch_usd.py C:\deps\usd`
downloads the OpenUSD CI builds usdcc against, with the USD plugins a release
ships (glTF, OBJ, PLY, STL and SPZ file formats) unzipped over it
([docs/USD_PLUGINS.md](docs/USD_PLUGINS.md)). Then
`python tools/build.py --usd-install C:\deps\usd`.

### Python bindings (`usdcc.ui`)

`SidePanel` is exposed to Python via Shiboken6, so scripts can add their own side
panels (`usdcc.app.add_side_panel()`) and view panel types
(`usdcc.app.register_view_panel()`, [docs/SCRIPTING.md](docs/SCRIPTING.md) §8).
This requires PySide6 and Shiboken6 built **from source against usdcc's own vcpkg
Qt**, for OpenUSD's Python. A pip-installed PySide6 ships a different,
independently-compiled Qt, and loading both in one process causes intermittent
DLL-load crashes (see [docs/PLAN.md](docs/PLAN.md) §6 item 11). After a first
`tools/build.py`, build it once with:

```
python tools/build_pyside.py
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

### Plugins

A plugin is a folder with a `usdcc-plugin.json`, holding Python, USD files or a C++ library, that
the app finds in `%LOCALAPPDATA%/usdcc_config/plugins`. It can add panels, tools, menu entries and USD
schemas. See [docs/PLUGINS.md](docs/PLUGINS.md), and [docs/PLUGIN_SDK.md](docs/PLUGIN_SDK.md) for C++.
Try the examples with `usdcc --plugin-path src/plugins --plugin-path build/plugins`.

### Convenience scripts

[tools/build.py](tools/build.py) and [tools/run.py](tools/run.py) wrap the
steps above (including the one-time vcpkg bootstrap):

```
python tools/build.py              # configure + build
python tools/run.py                # configure + build + launch
python tools/run.py --scene test_data\cube_and_sphere.usda   # ...and open a stage on launch
python tools/run.py --without-tests  # ...skipping the usdcc_tests executable
```

Both accept `--configuration`, `--generator`, `--arch`, `--usd-install`, `--scene`,
`--without-tests`, and `--clean`; `build.py` additionally takes `--run` (which is what `run.py` wraps).
`--usd-install` only needs to be passed once — it's cached by CMake and reused by
later `--run`/`run.py` invocations automatically. See each script's help
(`python tools/build.py --help`) for details.

The scripts in `tools/` are Python (3.9 or later, standard library only), so they
aren't tied to Windows. `build.py` and `run.py` pick Ninja and vcpkg's dynamic
triplet on Linux and macOS (macOS hasn't been built yet); `deploy.py` and
`build_pyside.py` only know Windows' layout so far, and say so elsewhere.

### Linux (and WSL 2)

usdcc builds, runs and passes its tests on Ubuntu 24.04, so far in WSL 2 on Windows
([docs/LINUX_WSL.md](docs/LINUX_WSL.md) has the packages to install and what was found).
OpenUSD comes from [tools/build_usd.py](tools/build_usd.py), which builds the commit
`deps/usd.json` pins. Run it with a Python 3.13, the version the pin names; Ubuntu 24.04's
own is 3.12, and Linux hasn't been built again since the pin moved to 3.13:

```
python3.13 tools/build_usd.py ~/deps/usd        # about 40 minutes
python3 tools/build.py --usd-install ~/deps/usd
ctest --test-dir build --output-on-failure
```

From Windows, [tools/wsl.py](tools/wsl.py) does the same in a clone on WSL's own disk:
it copies this working tree there first, so edits are built without committing them.

```
python tools/wsl.py setup     # once: the clone, its submodules and vcpkg
python tools/wsl.py usd       # once: OpenUSD into ~/deps/usd
python tools/wsl.py build
python tools/wsl.py run --scene test_data/cube_and_sphere.usda
python tools/wsl.py test      # --headless: under Xvfb; --software: without the GPU
```

Under WSLg, OpenGL needs `GALLIUM_DRIVER=d3d12` and `MESA_D3D12_DEFAULT_ADAPTER_NAME=<GPU>`
to use the GPU (`tools/wsl.py` sets both); without them Mesa renders in software. PySide6
isn't built for Linux yet, so `usdcc.ui` and Python-made panels are off there.

### When usdcc crashes

Windows saves a dump of every crash in `%LOCALAPPDATA%\CrashDumps`.
[tools/analyze_crash.py](tools/analyze_crash.py) reads the newest one with WinDbg's `cdb`
(`winget install Microsoft.WinDbg`, once) and prints the crashing and main threads'
stacks and the end of that run's log:

```
python tools/analyze_crash.py              # the newest usdcc.exe dump
python tools/analyze_crash.py --list       # all of them, test programs' too
```

Release and RelWithDebInfo both make PDBs (Release's code is unchanged by it). Each
build of everything copies its new PDBs and binaries into
`%USERPROFILE%\.usdcc-symbols\builds` (the last 5 of each file;
[tools/symbol_store.py](tools/symbol_store.py), off with `-DUSDCC_ARCHIVE_SYMBOLS=OFF`),
so a dump still gets names after a rebuild.

For a full dump, which also holds the objects the stacks point to, run once in an
administrator PowerShell (about the app's memory use per dump, the last 5 kept):

```
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\usdcc.exe" /v DumpType /t REG_DWORD /d 2 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\usdcc.exe" /v DumpCount /t REG_DWORD /d 5 /f
```

### Deploying

[tools/deploy.py](tools/deploy.py) copies a build and everything it needs to run
(OpenUSD's DLLs and plugins, Qt, the Python OpenUSD was built with, and the usdcc, pxr
and PySide6 packages) into one folder that runs on a machine without the dev
environment. Zip it to distribute; start it with `usdcc.exe` (`usdcc.cmd` in a build
without Velopack) or `bin\usdcc.exe`. Releases also come with an installer, made from the
same folder, that updates itself ([docs/UPDATES.md](docs/UPDATES.md)).

```
python tools/deploy.py                       # into .\deploy
python tools/deploy.py D:\dist\usdcc         # into another folder
python tools/deploy.py --configuration Release
python tools/deploy.py --usd-install C:\usd\install   # OpenUSD from there instead of the build's
python tools/deploy.py D:\dist\usdcc --clean # delete the folder first, whatever it holds
```

Build first. An earlier deploy in the folder is replaced; any other non-empty folder is
refused, unless `--clean` says to delete it first.

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
