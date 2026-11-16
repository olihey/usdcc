# Building and replacing the Blender package from accompanying source

Keep this file, the source tarball and its JSON sidecar with the binary download.
Extract the tarball to a short writable path (for example `C:/src/mesh`), using a
tar implementation that supports long paths. This is source for the exact binaries
listed by SHA-256 in `SOURCE-INVENTORY.json`. The external sidecar records the
archive's SHA-256; `distribution-readiness.json` binds the complete deliverable.

## Contents and restoring inputs

- `usdcc/`: project working-tree snapshot, build/install scripts, dependency manifests,
  patches, vcpkg port definitions and QWindowKit source (including original notices).
- `sources/blender-<commit>.tar.gz` and `blender-generated/`: original pinned Blender,
  actual generated subsets and source selection/generator.
- `sources/vcpkg/`: every checksum-pinned source archive/patch from the installed
  app and private backend SPDX inventories. Qt and ADS sources are included here.
- `sources/OpenUSD/`: actual pinned USD checkout with its local build modifications.
  `OpenUSD-dependencies/` and `OpenUSD-bundled/` include complete dependency and bundled
  source trees. Duplicate top-level USD download ZIPs are omitted; their extracted
  trees already contain the preferred source and upstream build modifications.
- `sources/supplemental/`: exact TBB and CPython sources/externals, Ruff/uv sources,
  and all Cargo.lock registry dependency archives. CPython sources include PCbuild
  build scripts and the sources of its builtin expat and other bundled components.
- `sources/pyside-setup/`: actual matching PySide/Shiboken source tree.
- `sources/git/`: self-contained vcpkg and QWindowKit Git bundles, preserving the
  commit identity needed by vcpkg's version baseline without relying on a live server.
- `inventory/`: actual app/backend and USD dependency CMake configurations, source
  hashes and compiler input logs. Absolute paths document the build machine; substitute
  your local paths. `licenses/` preserves third-party license material.

Run with Python 3.12 or later:

```powershell
python C:/src/mesh/usdcc/tools/blender_source_restore.py C:/src/mesh
```

This verifies/copies the bundled sources to vcpkg's download cache, restores the
Blender source archive and supplemental cache, and restores Git identity for the
included vcpkg/QWindowKit trees. It does not download anything. Copy
`sources/pyside-setup` to `usdcc/external/pyside-setup` before building the bindings.

## Build recipe (Windows x64)

Install Visual Studio 2022 C++ tools and Windows SDK, CMake 3.31+, Ninja or MSBuild,
Git, and Python 3.12 development files. The reviewed app/backend use MSVC
19.44.35229.0, release CRT `/MD`, RelWithDebInfo; the embedded interpreter is official
CPython **3.12.10** (the independent vcpkg Python 3.12.13 is not the runtime).
Compiler/SDK executables are general build tools and are not included as source.

1. Build/install CPython if replacing it: unpack `CPython-3.12.10.tar.gz`, populate
   its `externals/` from the supplied `CPython-*` archives with the directory names
   expected by `PCbuild/get_externals.bat`, then use `PCbuild/build.bat -p x64 -c Release`.
   These inputs are the exact official externals SPDX hashes. The binary package
   excludes Tcl/Tk and `_tkinter`; they are unnecessary for the app.
2. Build OpenUSD using `sources/OpenUSD/build_scripts/build_usd.py`, Python 3.12.10,
   the supplied dependency sources, and a writable install/build prefix. The default
   project's `deps/usd.json` and `tools/build_usd.py` record the normal build recipe;
   `inventory/OpenUSD-build/*/CMakeCache.txt` records the actual feature variants.
   Preserve Python, imaging, GL, MaterialX, Embree, OpenImageIO and OpenColorIO.
   The audited local USD cache also enabled USDView; its sources are included.
   Disable proprietary renderers, SYCL and ISPC. The USD snapshot already contains
   the modifications represented by `deps/patches/`; do not apply them twice.
   Reuse the supplied `OpenUSD-dependencies` source directories as `--src`, and the
   bundled FetchContent/ext sources rather than substituting a different revision.
3. TBB's Windows vendor download contains binaries, so its **separate complete
   v2020.3 source archive** is supplied. To modify TBB, use its included GNU Make
   build system (`Makefile`, `build/windows*.inc`, compiler=cl, arch=intel64) in a
   VS developer environment, and replace the release TBB headers/libraries in
   the USD prefix. The optional hwloc binding/preview/debug DLLs are not shipped.
4. From `usdcc/`, build vcpkg/Qt and the app using `tools/build.py --usd-install <prefix>`.
   Its backend build always enables the plugin. vcpkg uses the restored source
   cache; port definitions and patches are supplied. Optional Ruff/uv bootstrap
   downloads can be disabled with `-DUSDCC_BUNDLE_RUFF=OFF -DUSDCC_BUNDLE_UV=OFF`
   when configuring manually; rebuilding those separate tools uses their supplied
   Cargo.lock and registry archives with Cargo's normal vendor/offline workflow.
5. For the matching PySide/Shiboken bindings, run `tools/build_pyside.py` with the
   populated `external/pyside-setup` tree and the same Python/Qt. It builds only
   Core/Gui/Widgets, uses release CRT, and provides the local Qt build to shiboken.
   libclang is a general binding-generation build tool; its version/download recipe
   is in that script. Reconfigure/rebuild the app after installing the bindings.

For a direct CMake build after dependencies and bindings are installed:

```powershell
python tools/build_blender_mesh.py
cmake -S . -B build -DUSD_INSTALL=<prefix> -DPython3_EXECUTABLE=<python.exe> -DUSDCC_BUILD_BLENDER_MESH_TOOLS=ON
cmake --build build --config RelWithDebInfo --parallel 4
ctest --test-dir build -C RelWithDebInfo --output-on-failure
python tools/deploy.py build/blender-mesh-package --with-blender-mesh-tools
```

The build scripts are provided to control compilation/installation; an isolated
offline rebuild of every third-party library has not been claimed. If changing
dependencies or generating a new binary, regenerate the source receipt and review.

## Replacing LGPL libraries and installing modifications

Qt's DLLs are in `bin/`, with its plugins in their existing plugin directories.
ADS is `bin/qtadvanceddocking-qt6.dll`. PySide6/Shiboken DLLs and extensions are
under `python/Lib/site-packages/`. Build a modified source version with compatible
ABI, architecture, Python and CRT, close usdcc, and replace these files. Rebuild
the application/bindings from the included source if changing ABI. There is no
signature or activation check preventing a modified DLL from loading. Reverse
engineering to debug such modifications is permitted. Keep any renamed libraries
consistent with import names and Qt's plugin layout.

Our maintained Blender subset is generated by
`src/plugins/blender_mesh_tools/backend/extract_sources.py`; edit it or the upstream
sources, regenerate and build the backend, then build/deploy the plugin. No Blender
installation, second Python runtime or second USD runtime is required.

For a new distribution, collect sources **after** building/deploying and before
changing the tree:

```powershell
python tools/blender_mesh_package.py --usd-source <actual-USD-checkout> --package build/blender-mesh-package
python tools/blender_distribution.py build/blender-mesh-package --finalize --usd-source <actual-USD-checkout> --source-bundle build/blender-mesh-corresponding-source.tar.gz
python tools/blender_distribution.py build/blender-mesh-package --verify
```

The finalized manifest covers the whole package. Do not add smoke outputs or other
files inside it after finalization; put reports alongside it, or finalize again.
