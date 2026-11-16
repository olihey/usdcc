# Blender mesh backend license inventory

Our original adapter, generator, probe, and plugin sources are BSD-3-Clause. The
backend links Blender's GPL-covered code. The linked artifact is not BSD-only.

The explicit source list is in `backend/sources.cmake`. `backend/extract_sources.py`
also generates selected GPL source excerpts from the pinned archive. It excludes
serialization and application entry points, and disables external custom-data IO
callbacks. It does not supply missing APIs as stubs. Generated sources retain
upstream notices and remain GPL-covered. The small `runtime.cpp` supplies the
zero-initialized diagnostic `Global` inspected by BMesh's normal code.

The build driver collects upstream license texts, source/excerpt hashes, vcpkg
SPDX records, compiler/build information, and an explicit inventory into the private
install prefix. Blender's archive checksum is recorded in `deps/blender.json`.
The phase 2 numeric conversion API uses the same GPL-linked backend and license
policy; its C ABI does not establish a separate licensing boundary.

Current dependency families are Blender BMesh/kernel/blenlib support (GPL-2.0-or-later,
with per-file notices), Blender's Eigen wrappers and guarded allocator, vendored
xxhash/wcwidth headers or code, Eigen 3.4.0 (MPL-2.0, `EIGEN_MPL2_ONLY` enabled),
fmt (MIT), and PThreads4W 3.0.0 on Windows (Apache-2.0). System threading/runtime
libraries apply on the target platform. Inspect the collected notices rather than
assuming every support file has the same license as BMesh.

The backend inventory alone does not approve a combined distribution. The Windows
artifact-specific review, accompanying-source recipe and integrity gate are recorded
in `docs/BLENDER_DISTRIBUTION_REVIEW.md` and `docs/BLENDER_SOURCE_BUILD.md`. Linux
requires its own distro-specific source receipt. Ordinary deploy tooling explicitly
excludes this plugin; only the separate combined configuration includes it.
