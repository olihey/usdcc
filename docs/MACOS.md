# macOS build

Building and running usdcc on macOS, on Apple Silicon. Intel Macs aren't built or tested.

**Status (2026-11-23): builds, runs and passes its 13 test programs**, with PySide6, the
Blender mesh tools and the USD plugin pack of file formats (glTF, OBJ, PLY, STL, SPZ). CI's
macOS job ([CI.md](CI.md) §9) builds and tests it. The release job hasn't run.

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
PY=$(uv python find 3.13.12)
$PY tools/build_usd.py ~/deps/usd
$PY tools/build_usd_plugin.py adobe-fileformats --usd-install ~/deps/usd --install
$PY tools/build.py --usd-install ~/deps/usd
$PY tools/build_pyside.py                     # downloads libclang, 1 GB
$PY tools/build.py                            # again, with usdcc.ui
ctest --test-dir build --output-on-failure
$PY tools/run.py --scene test_data/cube_and_sphere.usda
```

- **Run the tools with Python 3.12 or later**, as the Blender backend's build needs: macOS'
  own `python3` is older. `build_usd.py` and `build_usd_plugin.py` need 3.13.
- **The oldest macOS is 14.0**, in three places that have to agree: `tools/_common.py`
  (OpenUSD, usdcc), `cmake/triplets/arm64-osx-dynamic.cmake` (vcpkg's packages) and the
  top-level `CMakeLists.txt` (a configure by hand). `std::format` needs 13.3.
- **OpenUSD's patches for macOS alone** are in `deps/patches/macos`, not in `deps/usd.json`,
  whose hash names every platform's asset. The macOS asset's name has a hash of its own for them
  and for the flags only macOS has.
- **No Homebrew libraries in OpenUSD**: `--ignore-homebrew` keeps CMake from them, and
  `PKG_CONFIG_LIBDIR` (`tools/build_usd.py`) pkg-config. A Mac without them couldn't load what
  links them.
- **CMake 4** refuses what some of OpenUSD's dependencies ask for:
  `CMAKE_POLICY_VERSION_MINIMUM` (set by `tools/build_usd.py`) for yaml-cpp, and a patch
  for Alembic.
- **vcpkg's host tools are static** (`cmake/triplets/arm64-osx.cmake`), so Qt is built twice,
  for them release-only. With dynamic ones a clean build
  failed right after Qt, as `moc` didn't find `libpcre2`: vcpkg hard-links what it installs
  from a staging folder, and while that is read, as when Qt is packed into the binary cache,
  macOS takes the running `moc` to be there, with no libraries beside it. Reading the staged
  `moc` in a loop made the installed one fail 4 times in 10, and 3 in 10 right after.
- **Qt is patched** (, an overlay port of macOS' alone): Qt 6.11.1 frees the
  color space of an image with a color profile before using it. Its move, wait and busy mouse cursors
  are such images, and setting one crashed. Copy the port again when vcpkg's baseline moves.

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
- **Rpaths** are `@loader_path`-relative, where Linux has `$ORIGIN`. `tools/_macho.py` rewrites
  them and signs each file again, ad hoc: Apple Silicon doesn't run code whose signature no
  longer fits. `tools/ci/relocate_usd.py` uses it too. A prebuilt OpenUSD links
  `@rpath/libpython3.13.dylib`, which usdcc's programs have an rpath for.
- **The Blender backend's `*_inline.cc` files** aren't compiled by themselves, as in Blender's
  own build: Clang ignores `#pragma once` in the file it compiles.
- **Paths** compare without case, as APFS does by default.
- **Asset servers' logins** are in the Keychain ([ASSET_SOURCES.md](ASSET_SOURCES.md) §10.6).
  **Peers' key** is in a file, as on Linux.

- **The deploy** (`tools/deploy.py`) is `usdcc.app`, with everything in `Contents/Resources`
  laid out as a Linux deploy: OpenUSD and Python find their data beside their libraries, and
  `codesign` takes only signed code in `Frameworks`. Nothing may be written into it: its
  Python is compiled already, and usdcc's Python writes no `__pycache__` in a bundle.
  Libraries are linked by their own names, not a link's: the hardened runtime refuses
  `libjpeg.62.dylib` when the library calls itself `libjpeg.62.4.0.dylib`.
- **An empty menu isn't in the menu bar.** Qt hides it, so a menu filled only as it opens
  (`aboutToShow`) never does: it needs an entry from the start (Stages, Create).
- **Files** opened from Finder come as an event (`FileOpenEvents`), not an argument.

## 4. Found in the tests

- `peers_discovery_test` kept a pointer into a temporary list: wrong everywhere, but it only
  crashed here.
- OpenUSD builds OpenColorIO 2.4.2 on macOS and 2.2.1 elsewhere, which call the color picking
  space differently.
- The temporary folder is a symbolic link (`/var` to `/private/var`), so paths are compared
  canonical.
- `<sys/proc.h>` declares a `struct session`, and `<sys/param.h>` macros `MAX` and `MIN`.

## 5. Not done

- The release job hasn't run; a Developer ID and notarization. The
  deploy leaves Blender mesh tools out, as the other releases do. The Gatling (no MDL SDK download) and Arnold
  packs. MoonRay builds and renders ([MOONRAY.md](MOONRAY.md) §7.6); CI hasn't built it.
- Checked in the running app by script: a Python panel, Mesh > Triangulate and its undo, a glTF
  file, Storm and Embree in the viewport, the Outline and Wireframe selection, full screen and
  back, quitting. Seen only in frames the app saved, not on the screen, so without the HUD.
  Not tried: dragging a gizmo, two viewports side by side.
- The Wireframe selection style draws some edges broken into dashes. Whether it does elsewhere
  hasn't been looked at.
- The shader-drawn grid, gizmos and HUD haven't been looked at on Windows and Linux.
