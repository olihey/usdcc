# Linux accompanying source: restore, build and replace libraries

This source accompanies the exact Ubuntu 24.04 x86-64 binaries listed in
`SOURCE-INVENTORY.json`. Keep the binary download's complete `source/` and
`licenses/` directories. The external source sidecar records the archive SHA-256;
`distribution-readiness.json` binds all delivered files, permissions and symlinks.

Extract on a Linux filesystem supporting case-sensitive names, modes and symlinks:

```sh
mkdir mesh-source
tar -xzf blender-mesh-linux-corresponding-source.tar.gz -C mesh-source
python3 mesh-source/usdcc/tools/blender_source_restore.py mesh-source
```

The restoration script verifies the supplied source hashes, reconstructs vcpkg's
download cache, restores the Blender/tool caches and dependency Git identities,
and copies exact Ubuntu source packages to the project cache. It does not fetch
anything or change apt repositories. `sources/git/` contains self-contained
vcpkg/QWindowKit bundles; their commit identities satisfy the version baseline.

## Source layout

- `usdcc/`: full project working-tree snapshot, original licenses, dependency pins,
  patches, build/install scripts, port definitions and QWindowKit source.
- `sources/blender-<commit>.tar.gz` and `blender-generated/`: immutable upstream
  Blender archive, actual generated GPL subsets, generator and source selection.
- `sources/vcpkg/`: all checksum-pinned source archives/patches from both installed
  app/backend inventories, including Qt, ADS, Eigen and Linux-only libraries.
- `sources/OpenUSD/`: actual pinned fork with the GLM build-script modification;
  a dated comment marks modified files. The supplied `deps/patches` describes it.
- `sources/OpenUSD-dependencies/`: complete **Linux** source trees, including
  Boost 1.80.0, TBB 2020.3.1 and Embree. `OpenUSD-bundled/` contains fetched/bundled
  Imath, yaml-cpp, pystring and minizip sources. Duplicate source download ZIPs and
  build-object files are omitted when the complete extracted sources are present.
- `sources/ubuntu/`: all 31 official source publications, including `.dsc`, original
  upstream archive(s), and Debian/Ubuntu packaging/patch archives. Python's two source
  versions are distinct. The original DSC signatures and SHA-256 inventories remain.
- `sources/supplemental/`: Ruff/uv sources plus all their Cargo.lock registry sources.
- `inventory/`: actual app/backend/USD dependency CMake configurations, backend
  source/excerpt hashes and SPDX records. Compiler input hashes are also in the
  root inventory. Absolute paths describe the build machine; replace them locally.

The GNU C/C++ runtime, OS/window-system interfaces and graphics drivers listed as
host dependencies are not bundled. Their notices and the tested system-package
versions are recorded separately; kernel/compiler/driver build tools are not supplied.

## Ubuntu Python and library sources

The interpreter/core stdlib source is `python3.12=3.12.3-1ubuntu0.17`; separately
built extensions use `python3-stdlib-extensions=3.12.3-0ubuntu1`. The DSC files,
upstream archives, patch series and `debian/rules` for both are provided. To restore
a patched source tree after the restoration script has verified all file hashes:

```sh
cd mesh-source/usdcc/external/blender-work/ubuntu-sources/python3.12-3.12.3-1ubuntu0.17
dpkg-source --no-check -x python3.12_3.12.3-1ubuntu0.17.dsc /tmp/python-source
```

`--no-check` avoids requiring an uploader's PGP key during offline restoration;
the restoration script has already checked source-file SHA-256 hashes. Signatures
remain available for independent validation with the appropriate Ubuntu keys.
Repeat for the extension source package or any supplied system-library source.

On a prepared Ubuntu 24.04 build environment, install the general compiler/build
dependencies described in each DSC and build with Debian's usual
`dpkg-buildpackage -b -uc -us`. Ubuntu-specific patches and flags are in `debian/`.
The app uses the release Python ABI and a shared `libpython3.12.so.1.0`. Replace the
interpreter, library, stdlib and extension modules consistently when changing ABI.
The source receipt captures distro patches; it does not promise bit-identical Ubuntu
rebuilds with a different compiler or build-dependency environment.

## Build usdcc and its native backend

Use Ubuntu 24.04 x86-64, GNU C++ 13.3.0, CMake/Ninja or Make, Git, Python 3.12,
development headers, X11/Wayland/OpenGL build dependencies and `patchelf`.
General build tools can be installed separately. Consult `docs/LINUX_WSL.md`
for the baseline development packages.

1. Copy `sources/OpenUSD` into a writable `<work>/OpenUSD`, the dependency trees
   into `<work>/src`, and preserve the supplied bundled-source directory layout
   when configuring the corresponding dependencies. The actual feature choices
   and FetchContent versions are in `inventory/OpenUSD-build/**/CMakeCache.txt`.
   Use the existing patched `build_scripts/build_usd.py`; do not apply the GLM
   patch twice. `deps/usd.json` and `tools/build_usd.py` describe the normal recipe;
   Linux omits the Visual Studio generator flag. Build release libraries with Python,
   GL imaging, MaterialX, Embree, OpenImageIO and OpenColorIO enabled and proprietary
   renderers/SYCL/ISPC disabled. Use the supplied Boost/TBB Linux source variants.
2. From `usdcc/`, bootstrap the restored vcpkg source and use its restored cache.
   Build the backend, then configure/build the app against the resulting USD prefix:

```sh
sh external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
python3 tools/build_blender_mesh.py --jobs 4
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=external/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-dynamic -DUSD_INSTALL=<usd-prefix> \
  -DUSDCC_BUILD_BLENDER_MESH_TOOLS=ON
cmake --build build --parallel 4
xvfb-run -a ctest --test-dir build --output-on-failure
python3 tools/deploy.py build/blender-mesh-package --with-blender-mesh-tools
```

The plugin is always built/enabled. The backend has no Blender application, `bpy`,
second Python or second USD runtime. Its maintained subsets are generated by
`src/plugins/blender_mesh_tools/backend/extract_sources.py`; source and generator
modifications retain upstream GPL notices. No PySide/Shiboken is shipped here.

The original build caches document architecture, compiler, flags and dependency
features. Do not copy absolute-path cache files directly over a new build. All
source materials/build recipes are supplied; a complete offline compilation of
every third-party library has not been claimed. Unmodified general build-tool
bootstraps may still need their normal executable packages.

## Replace shared libraries and install modifications

Qt, ADS and other public shared libraries are in `lib/`, with Qt's plugin tree in
`Qt6/plugins/`. The Blender backend and fmt are private to
`plugins/blender_mesh_tools/bin/linux-x86_64/`. Rebuild a compatible library from
the supplied source, close usdcc, and replace the real `.so` and its symlink chain.
Keep SONAME/ABI, architecture, C++ runtime and Qt plugin versions consistent. Rebuild
the app as well if changing ABI. There is no signing or activation barrier to loading
your modifications, and reverse engineering to debug them is permitted.

Deployment sets relative `$ORIGIN` RUNPATHs; preserve symlinks and executable modes.
Do not use `LD_LIBRARY_PATH` to load a second Qt or USD. User data and profiles
belong outside the pristine distribution folder when checking its integrity.

## Produce a newly reviewed distribution

```sh
python3 tools/blender_linux_distribution.py build/blender-mesh-package --collect
# Review any changed dependencies and update the checked Linux review lock deliberately.
python3 tools/blender_linux_distribution.py build/blender-mesh-package \
  --sources build/blender-mesh-linux-corresponding-source.tar.gz
python3 tools/blender_linux_distribution.py build/blender-mesh-package \
  --finalize build/blender-mesh-linux-corresponding-source.tar.gz
python3 tools/blender_linux_distribution.py build/blender-mesh-package --verify
python3 tools/blender_linux_distribution.py build/blender-mesh-package \
  --archive build/usdcc-blender-linux-x86_64.tar.gz
```

Collect source after the binary build and runtime checks, before changing the tree.
Finalization rejects source/binary drift, unknown runtimes, missing notices/source,
escaping links and a mismatched review lock. It writes an artifact-specific readiness
report; `--collect` alone does not approve a release. Ship the entire resulting
download and publish its checksum. This review does not upload or publish it.
