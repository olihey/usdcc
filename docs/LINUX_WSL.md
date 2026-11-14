# Linux build under WSL 2

Building and running usdcc on Linux, locally, in WSL 2 on the Windows dev PC: the first
step toward the Linux CI job in [CI.md](CI.md) §7 and the "Windows + Linux" target in
[PLAN.md](PLAN.md) §3 item 1.

**Status (2026-11-14): done.** On Ubuntu 24.04 in WSL 2, with GCC 13.3, usdcc builds, runs
on the RTX 4090 (Storm, picking, the gizmos), and passes all three test programs. They pass
both on WSLg with the GPU and headless under Xvfb with llvmpipe. Not done yet: PySide6 for
Linux, a Linux CI job, a Linux deploy, and checks on a native Linux desktop (§6).

## 1. The machine

| | |
|---|---|
| WSL | 2.6.3, kernel 6.6.87, WSLg 1.0.71 (X11 on `:0` and Wayland) |
| Distro | `Ubuntu-24.04` (24.04.4), the default; WSL 2 |
| Resources in WSL | 32 threads, 77 GB RAM, 867 GB free on its ext4 disk |
| GPU | RTX 4090 (+ AMD iGPU); `/usr/lib/wsl/lib` has `libd3d12`, `libcuda`; `nvidia-smi` works |
| Toolchain | gcc/g++ 13.3, cmake 3.28.3, ninja 1.11.1, Python 3.12.3 + `python3.12-dev`, Mesa 25.2.8 |

Packages installed for the build (on top of `build-essential` and `python3.12-dev`):

```bash
sudo apt install -y \
  cmake ninja-build pkg-config zip unzip tar autoconf autoconf-archive automake libtool \
  bison flex gperf nasm python3-venv python3-pip gdb mesa-utils xvfb \
  libgl-dev libegl-dev libegl1-mesa-dev libglu1-mesa-dev libx11-dev libx11-xcb-dev libxext-dev \
  libxrender-dev libxi-dev libxt-dev libxtst-dev libxrandr-dev libxcursor-dev libxinerama-dev \
  libsm-dev libice-dev libxkbcommon-dev libxkbcommon-x11-dev '^libxcb.*-dev' libfontconfig1-dev \
  libfreetype-dev libdbus-1-dev libwayland-dev wayland-protocols libwayland-bin
# only for looking at the running app from a script: imagemagick xdotool x11-utils
```

## 2. How it's set up

- **A separate clone on WSL's own disk** (`~/dev/usdcc`), not a build from `/mnt/c`. Over
  the 9P share, compiling is many times slower, and the two OSes would fight over `build/`
  and vcpkg's `buildtrees`. Code is edited and committed on Windows only.
  [tools/wsl.py](../tools/wsl.py) copies the working tree into the clone before each
  command (`rsync`, keeping the clone's own file modes; `core.autocrlf=input` there, so
  the CRLF files count as unchanged). It also sets the GPU's environment (§3).
- **OpenUSD** is built from the commit and patches `deps/usd.json` pins, with the same
  `build_usd.py` flags, by [tools/build_usd.py](../tools/build_usd.py) into `~/deps/usd`.
  That takes about 40 minutes. It drops the pin's Windows-only `--generator` flag and uses
  Ubuntu's Python 3.12, since only the minor version has to match. `deps/usd.json` itself is
  unchanged: any edit to it renames the Windows CI asset.
- **Qt and the rest come from vcpkg** with the `x64-linux-dynamic` triplet. A full build
  takes about an hour, mostly qtbase.
- **X11 by default.** Qt 6.11's qtbase includes the Wayland platform plugin, and Qt picks it
  under WSLg, which sets `WAYLAND_DISPLAY`. `tools/wsl.py` sets `QT_QPA_PLATFORM=xcb`
  unless `--wayland`. Under Wayland the app starts and opens a stage, but nobody has
  looked at its window yet.

## 3. OpenGL under WSLg

| Setting | Renderer | Version |
|---|---|---|
| nothing set | llvmpipe (software) | 4.5 core / 4.5 compatibility |
| only `MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` | still llvmpipe | 4.5 / 4.5 |
| only `GALLIUM_DRIVER=d3d12` | D3D12 (AMD Radeon iGPU) | 4.6 / 4.6 |
| **both** | **D3D12 (NVIDIA GeForce RTX 4090)**, accelerated, 24 GB | **4.6 / 4.6** |
| both, under `xvfb-run` | D3D12 (RTX 4090) | 4.6 |

- Mesa 25 never picks d3d12 by itself under WSLg: its X server isn't DRI3-capable, so Mesa
  falls back to software. Both variables are needed, for GLX and EGL alike.
  `tools/wsl.py` sets them (`--gpu NAME`, or `--software` for llvmpipe). For your own WSL
  shells, add them to `~/.profile`.
- d3d12 has what Storm uses: `ARB_direct_state_access`, `multi_draw_indirect`,
  `shader_draw_parameters`, `shader_storage_buffer_object`, `compute_shader`, `gpu_shader5`
  and `gpu_shader_int64`. It lacks `ARB_bindless_texture`, which Storm treats as optional.

## 4. What Linux needed

Everything compiled on the first try. What came up was in linking, at run time and in the
tests.

**Build:**

- **QWindowKit needs a Qt built with Wayland.** Its Linux code uses
  `QNativeInterface::QWaylandApplication` unguarded (upstream too). `vcpkg.json` now asks
  for qtbase's `wayland` feature on Linux only, so Windows' Qt is unchanged.
- **vcpkg's python3 was embedded instead of OpenUSD's.** pybind11's port pulls in vcpkg's
  python3, whose `libpython3.12.so.1.0` sits in `vcpkg_installed/<triplet>/lib`, ahead of
  the system one on every binary's RUNPATH. The app then ran Python 3.12.13 against Ubuntu's
  3.12.3 standard library and couldn't import `math`. An overlay port
  ([cmake/ports/pybind11](../cmake/ports/pybind11)) keeps that dependency on Windows only.
  usdcc only uses `pybind11::headers`.
- `MemorySettingsBackend` got an exported out-of-line constructor and destructor, so its
  vtable stays inside `usdcc_core`, which hides what it doesn't export.
- `usdcc_usd_bindings`, a static library linked into a module, is built with `-fPIC`.
- `UsdccPython.cmake` finds `libpython3.XY.so` and OpenUSD's `pxr` folder
  (`lib/python3.12/site-packages` on Linux) itself.
- `UsdccUv.cmake` and `UsdccRuff.cmake` download the pinned Linux wheels too. The app
  looks for `tools/uv/uv` and `tools/ruff/ruff`, and a Python's scripts in `bin`, without
  `.exe` off Windows.
- Plugins get `$ORIGIN` on their rpath: a plugin's own libraries are found next to it, as
  Windows does for DLLs.
- A build without the PCH (`-DUSDCC_USE_PCH=OFF`) found two missing includes, which affect
  every platform: `class QLabel*` in a member declaration in `array_editor_dialog.h`
  declared a new `usdcc::usd::properties::QLabel`, and `scene_counts_test.cpp` lacked
  `<pxr/usd/usd/attribute.h>`.

**Run time:**

- **The viewport got a core profile and Storm drew nothing.** For any request of 3.0 or
  later, Qt asks for a forward-compatible context unless `QSurfaceFormat::DeprecatedFunctions`
  is set. Mesa answers "compatibility + forward-compatible" with a core context, and NVIDIA's
  Windows driver ignores the flag. `main.cpp` sets the option now, on every platform.
- **`MainWindow` crashed on destruction.** Each side panel's `destroyed()` erased it from
  `m_sidePanels`, through a connection Qt only drops in `~QObject`, after `~QWidget` has
  deleted the panels and `m_sidePanels` is gone. The destructor now disconnects them first.
  Windows got away with it.
- **Plugin package folders "in use" are locked with `flock()` on Linux.** Windows can't
  delete a file another process holds open, and `removeUnused()` relied on that.
- **`PythonRuntime::executable()`** names the interpreter (`bin/python3.12` on Linux) for
  uv's `--python`, instead of `home/python.exe`.

**Tests:**

- **Comparing an `SdfLayerHandle` with an `SdfLayerRefPtr` loops for ever** under GCC's C++20
  rules. `TfWeakPtrFacade`'s `friend operator==(TfRefPtr<T>, Derived)` returns `p2 == p1`,
  which GCC resolves to the same friend again, a reversed candidate that is an exact match.
  MSVC picks the member operator. GCC doesn't warn. A check
  build with a `static_assert` in a shadowed copy of `weakPtrFacade.h` found the only two
  uses, both in tests. They now compare handle with handle. App code has none.
- Windows paths (`C:/...`, backslashes) as absolute paths, the trash under `/tmp` in Qt's
  test mode (now [tests/trash_temp_dir.h](../tests/trash_temp_dir.h)), a `std::system`
  command quoted for cmd.exe, `.dll` names, and the test plugin helper's `lib` prefix.
- **The Material Editor's `settle()`** spun for 80 ms. On Linux's offscreen platform one
  pass can take that long, so the `QTimer::singleShot(0)` it posted never ran. It now always
  does at least three passes.
- **Qt's offscreen backing store crashes** when a floating ADS dock's windows are
  destroyed: `QOffscreenBackingStore::clearHash()` dereferences `find()` without checking for
  `end()`. That's a Qt 6.11 bug. The floating step of `material_editor_draws_after_tabbing_and_floating`
  is skipped on the offscreen platform outside Windows, and Windows still checks it.

## 5. Day to day

```
python tools/wsl.py build
python tools/wsl.py run --scene test_data/cube_and_sphere.usda
python tools/wsl.py test                       # on WSLg, with the GPU
python tools/wsl.py --headless --software test # as a hosted Linux runner would
python tools/wsl.py exec -- git status         # anything else, in the clone
```

Now and then, run `git -C ~/dev/usdcc fetch win && git -C ~/dev/usdcc reset --hard win/master`
in WSL, to drop anything left over from syncing. Commit and push from Windows only.

## 6. Not done yet

- **PySide6 on Linux:** port `tools/build_pyside.py` (libclang for Linux, the `.so`
  layout). Until then `usdcc.ui` and Python-made panels are off on Linux.
- **Linux CI:** an `ubuntu-24.04` job in `build.yml`, with a Linux OpenUSD asset from
  `deps-usd.yml` built by `tools/build_usd.py`, and `deps/usd.json` given per-platform
  parts. The headless llvmpipe run above is what that job would do.
- **Linux deploy:** `tools/deploy.py` for an `$ORIGIN`-rpath tarball or AppImage, and the
  `resources/linux` desktop entry and icons.
- **A native Linux desktop:** QWindowKit's title bar (moving, edge resizing, snapping) and ADS
  floating windows have only been seen under WSLg's compositor; native Wayland not at all.
- **Crash analysis:** core dumps and `gdb` in place of `analyze_crash.py`/`cdb`.
- **macOS.**
