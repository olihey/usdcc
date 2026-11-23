# macOS build

Building and running usdcc on macOS, on Apple Silicon. Intel Macs aren't built or tested.

**Status (2026-11-23): builds, runs and passes its 8 test programs** against OpenUSD as
`deps/usd.json` pins it, with oneTBB, OpenVDB, Alembic and Draco. Not done yet: PySide6
(`usdcc.ui`), Blender mesh tools, the USD plugin packs, CI, and a deploy or release (§5).

## 1. The machine

| | |
|---|---|
| macOS | 27.2, Apple M5 Pro |
| Toolchain | Xcode 27.2 (Apple Clang 21), CMake 4.5.1, Ninja 1.13.2 |
| Python | 3.13.12 from `uv python install` |
| OpenGL | 4.1 Core (Metal - 91.7) |

```bash
brew install cmake ninja pkg-config autoconf autoconf-archive automake libtool uv
uv python install 3.13.12
git submodule update --init --recursive
```

- **Xcode itself**, not only the Command Line Tools. Where `xcode-select` names the latter,
  the tools use `/Applications/Xcode.app` through `DEVELOPER_DIR`; run by hand, set it too.
- **Python from uv**, which has a plain `libpython3.13.dylib`. Homebrew's and python.org's
  are framework builds, which `cmake/modules/UsdccPython.cmake` doesn't take.

## 2. Building

```bash
$(uv python find 3.13.12) tools/build_usd.py ~/deps/usd
python3 tools/build.py --usd-install ~/deps/usd
ctest --test-dir build --output-on-failure
python3 tools/run.py --scene test_data/cube_and_sphere.usda
```

- **The oldest macOS is 14.0**, in three places that have to agree: `tools/_common.py`
  (OpenUSD, usdcc), `cmake/triplets/arm64-osx-dynamic.cmake` (vcpkg's packages) and the
  top-level `CMakeLists.txt` (a configure by hand). `std::format` needs 13.3.
- **OpenUSD's patches for macOS alone** are in `deps/patches/macos`, not in `deps/usd.json`,
  whose hash names every platform's asset. So the macOS asset's name doesn't change with
  them, which CI has to take care of before it publishes one.
- **CMake 4** refuses what some of OpenUSD's dependencies ask for:
  `CMAKE_POLICY_VERSION_MINIMUM` (set by `tools/build_usd.py`) for yaml-cpp, and a patch
  for Alembic.
- **vcpkg's host triplet is the dynamic one** (`tools/build.py`), so Qt is built once. A
  clean build once failed right after Qt, with `moc` not finding `libpcre2`: vcpkg
  hard-links its tools from a staging folder, and macOS had looked `moc` up there, most
  likely as vcpkg was packing Qt into its cache. Running it again works. Should it come
  back, leave `VCPKG_HOST_TRIPLET` out, for a second Qt.

## 3. What is different

- **The viewport.** macOS has no compatibility profile, so it asks for OpenGL 4.1 Core.
  Storm renders with Metal (HgiMetal), and Hydra composites color and depth into the
  viewport's OpenGL framebuffer. A Hydra engine needs a color AOV for that: without one
  Storm draws straight to the framebuffer, which Metal can't do, and the selection outline
  was missing. The grid, gizmos and HUD are drawn with shaders, on every platform.
- **Storm in `usdcc_node`** needs OpenGL 4.1 there, and 4.5 elsewhere.
- **The bundle.** Only programs are in `Contents/MacOS`; plugins, resources and tools are in
  `Contents/Resources`. `usdcc/core/install_paths.h` finds them, so nothing should go from
  `applicationDirPath()` to `..`.
- **No `DYLD_LIBRARY_PATH`.** The rpaths reach every library. Set to OpenUSD's `lib`, it also
  replaces the system's libraries of the same name, whatever the case: with OpenUSD's
  `libpng.dylib` there, drawing an emoji crashed in ImageIO.
- **The window.** The menus are in the system's menu bar, and the title bar leaves room for
  the system's buttons. QWindowKit is only used with real (Cocoa) windows: it takes a window
  id for an `NSView`, and crashed on the tests' offscreen platform.
- **Paths** compare without case, as APFS does by default.
- **Credentials and Peers' key** are in files, as on Linux: no Keychain yet.

## 4. Found in the tests

- `peers_discovery_test` kept a pointer into a temporary list: wrong everywhere, but it only
  crashed here.
- OpenUSD builds OpenColorIO 2.4.2 on macOS and 2.2.1 elsewhere, which call the color picking
  space differently.
- The temporary folder is a symbolic link (`/var` to `/private/var`), so paths are compared
  canonical.
- `<sys/proc.h>` declares a `struct session`, and `<sys/param.h>` macros `MAX` and `MIN`.

## 5. Not done

- PySide6, Blender mesh tools and the USD plugin packs; CI; `tools/deploy.py`, signing and a
  release.
- Seen only in frames the app saved, not on the screen: the viewport and the window. The HUD
  isn't in those, so nobody has looked at it. Not tried: full screen, a second viewport,
  Embree in the viewport, the Wireframe selection style, dragging a gizmo.
- The shader-drawn grid, gizmos and HUD haven't been looked at on Windows and Linux.
