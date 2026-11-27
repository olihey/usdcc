# Renewing the GPL-3.0 configuration's review — plan (2026-11-27)

The GPL-3.0-or-later configuration (usdcc with the Blender mesh tools, and now the Gatling
pack) was reviewed on 2026-11-15 for Windows x64
([BLENDER_DISTRIBUTION_REVIEW.md](BLENDER_DISTRIBUTION_REVIEW.md)) and on 2026-11-16 for
Linux x86-64 ([BLENDER_LINUX_DISTRIBUTION_REVIEW.md](BLENDER_LINUX_DISTRIBUTION_REVIEW.md)).
Both reviews hold only for the exact inputs their locks name, and a lot has changed since.
Today `tools/blender_distribution.py --finalize` stops at "USD pin requires renewed review",
and the Linux audit at its `reviewed_inputs` comparison. This plan covers what changed, what
each change needs, which machine each step needs, and the decisions to make first.

Not legal advice: licence readings here are ours, from the texts.

## 1. Decide first

1. **Qt HTTP Server in the standard release.** This affects more than this review; see §3.
   It doesn't block the GPL configuration, but it should be decided before either
   configuration is released again.
2. **Python on Linux.** The configuration shipped Ubuntu 24.04's Python 3.12, with Ubuntu's
   exact source packages as its source. The pin is now 3.13.12, which Ubuntu 24.04 doesn't
   package. Options:
   - **(a) Build CPython 3.13.12 from python.org's source on the Ubuntu machine**
     (recommended). The source is one archive plus the Ubuntu `-dev` packages it links,
     which the existing Ubuntu source receipts already know how to cover.
   - (b) `actions/setup-python`'s build, which CI's Linux release uses. Its binaries are
     built elsewhere, from python.org's source plus build recipes in
     `actions/python-versions`. Its exact inputs would have to be traced and supplied.
   - (c) A uv-managed python-build-standalone. Its statically linked libraries (OpenSSL,
     SQLite, ncurses, libedit and others) would each need reviewing and supplying.
3. **The MDL SDK in the Gatling pack.** Keep NVIDIA's prebuilt `libmdl_sdk` (its source at
   the tag is supplied, but we can't prove the binary matches it), or build it from source
   in the pack's job. Building it settles the question for good, and costs a longer CI job.

## 2. What changed since the reviews

| Area | Reviewed | Now | Licence | What it needs |
|---|---|---|---|---|
| OpenUSD pin (`deps/usd.json`) | Python 3.12.10; no oneTBB/OpenVDB/Alembic/Draco; one patch | Python 3.13.12; `--onetbb --openvdb --alembic --draco`; patches for Boost with VS 2026 and Embree on Windows arm64 | — | New `usd_pin_sha256`, `openusd_pin`, `openusd_diff_sha256` |
| TBB | TBB 2020.3 | oneTBB 2021.12.0 | Apache-2.0 | Source tree (`oneTBB-2021.12.0`); its `third-party-programs.txt` (the audit already looks for `oneTBB-*`) |
| OpenVDB | — | 10.1.0 | **MPL-2.0**, no "Incompatible With Secondary Licenses" notice in any file (checked) | Allowed in a GPL-3.0 combination (MPL-2.0 §3.3), as Eigen's MPL subset is. Its source has to be supplied |
| c-blosc (OpenVDB's) | — | 1.20.1 | BSD-3-Clause, with bundled LZ4, Snappy, Zstd and zlib (BSD/zlib) | Source tree; check which bundled codecs are compiled in |
| Alembic | — | 1.8.5 | BSD-3-Clause | Source tree; confirm it's built without HDF5 |
| Draco | — | 1.5.6 | Apache-2.0 | Source tree |
| zlib (OpenUSD's) | — | 1.3.2 | Zlib | Source tree |
| CPython (Windows) | 3.12.10, with its externals' SPDX receipts | 3.13.12 | PSF-2.0; externals (OpenSSL 3, SQLite, libffi, bzip2, xz, zlib, mpdecimal and others) from `Misc/externals.spdx.json` at `v3.13.12` | New supplemental resources by SHA-256, replacing the 3.12.10 ones |
| CPython (Linux) | Ubuntu's 3.12 | 3.13.12 | — | Decision 1.2 |
| PySide6 | commit `73fb12a` | `pyside-6.11.1-qt6.11.1-ccb6b551`, built for 3.13; `tools/build_pyside.py` changed | LGPL-3.0 | Confirm the commit; new `pyside_commit` (Windows only; Linux ships none) |
| vcpkg: `qtwebsockets` | — | 6.11.1 | LGPL-3.0 (or GPL) | `PORT_LICENSES`, notice, sources |
| vcpkg: `qthttpserver` | — | 6.11.1 (overlay port) | **GPL-3.0 or commercial only** | Compatible here; see §3 for the standard release |
| vcpkg: `libsodium` | — | — | ISC | `PORT_LICENSES`, notice, sources |
| vcpkg: `mdns` | — | header only | Unlicense | No runtime; record the source |
| vcpkg: `curl` | — | 8.21, `ssl` feature only (Schannel on Windows, OpenSSL on Linux) | curl (MIT-like) | `PORT_LICENSES`; check its dependency closure (zlib, OpenSSL on Linux) |
| vcpkg: `pugixml` | — | — | MIT | `PORT_LICENSES` |
| vcpkg: `joltphysics` | — | with `rtti` | MIT | `PORT_LICENSES`; it's in `plugins/physics_jolt` |
| vcpkg configuration | overlay ports | overlay ports and triplets; `pybind11` port changed | — | New `vcpkg_resource_sha512`, `vcpkg_ports` |
| qwindowkit submodule | reviewed commit | bumped | Apache-2.0 | New `submodule_commits` |
| Blender backend | reviewed CMake | `backend/CMakeLists.txt` and the plugin's CMake changed (macOS/Clang support, PIC) | — | New `backend_input_sha256`; recheck the compiler inputs (418 files) |
| App components in the process | as reviewed | adds `usdcc_peers` (Qt Network, WebSockets, HttpServer, libsodium) and the shipped native plugins `physics_jolt`, `http`, `webdav`, `s3`, `viewport_broadcast` and `uv_view` | usdcc's BSD-3-Clause, plus the ports above | The audit's usdcc name allowlist; the inventory will list them |
| Gatling pack | excluded | included (PR [olihey/usdcc#124](https://github.com/olihey/usdcc/pull/124)) | GPL-3.0-or-later; MDL SDK BSD-3-Clause | Already reviewed ([USD_PLUGINS.md](USD_PLUGINS.md) §5); its build name depends on the OpenUSD asset, so a rebuilt asset means a rebuilt pack and a new lock entry |
| MSVC | as reviewed | the Boost patch suggests VS 2026 toolsets | Microsoft redistributable terms | Recheck which runtime DLLs ship and their redistribution terms |
| Ruff, uv | reviewed versions | check | MIT / MIT-or-Apache, plus crates | `tool_versions`, and the crate list if they changed |

## 3. Qt HTTP Server: a finding about the standard release

Qt's documentation says Qt HTTP Server is available "under commercial licenses" and under
"the GNU General Public License, version 3", with no LGPL option. Since Peers phase 1a
(`054a42b`, 2026-11-17), `usdcc_peers` links it, and `tools/deploy.py` requires
`bin/Qt6HttpServer.dll` in every Windows deploy. So **releases 26.11.1 to 26.11.11 ship a
GPL-3.0 library inside the BSD-3-Clause standard download**, in the same process as the RTX
Render plugin's proprietary runtime when a user installs it. [RELEASE.md](RELEASE.md) §6
lists every standard dependency as permissive or LGPL, which is no longer true.

- **In the GPL configuration** this is fine: GPL-3.0-only code combines with
  GPL-3.0-or-later code, and the whole is conveyed under GPL-3.0. Record it in both reviews.
- **For the standard release**, the options are:
  - (a) Serve the guests' page without Qt HTTP Server, on Qt Network's `QTcpServer` and the
    WebSocket upgrade that Qt WebSockets (LGPL-3.0) already has. This keeps the release
    BSD-3-Clause. **Recommended.**
  - (b) Move the guests' page into a plugin that the standard release doesn't ship.
  - (c) Accept that the standard release is a GPL-3.0 combination. That would mean dropping
    the RTX plugin and other proprietary integrations from it, and shipping its full source.
- Also add a gate to `tools/deploy.py` or the release check that fails on any GPL-only Qt
  module (HttpServer, Charts, DataVisualization and others) in the standard configuration.

This is its own task, separate from this renewal.

## 4. The work, step by step

### Phase A — reading and tooling (any machine, including this Mac)

1. Read the licences in §2 that haven't been read yet: OpenVDB's bundled code and
   c-blosc's codecs; Alembic without HDF5; Draco; oneTBB's third-party list; CPython
   3.13.12's externals; and the seven new vcpkg ports with their transitive ports.
2. Add the new ports to `PORT_LICENSES` in both audits: `qtwebsockets`
   `LGPL-3.0-only`, `qthttpserver` `GPL-3.0-only`, `libsodium` `ISC`, `curl` `curl`,
   `pugixml` `MIT`, `joltphysics` `MIT`.
3. Extend the audits' OpenUSD allowlists with the new runtimes: OpenVDB, Alembic, Draco
   and blosc if it's shared, on Windows (`check_binary_closure`) and Linux
   (`validate_rows`). Do the same for the usdcc runtimes (`usdcc_peers`, the native
   plugins).
4. Extend the dependency source-tree lists in both audits: `oneTBB-2021.12.0`,
   `openvdb-10.1.0`, `c-blosc-1.20.1`, `alembic-1.8.5`, `draco-1.5.6`, `zlib-1.3.2`. The
   archive check should require their `CMakeLists.txt` as it does for the others.
5. Replace the CPython 3.12.10 supplemental resources in `deps/blender-distribution.json`
   with 3.13.12's and its externals', by SHA-256 from `Misc/externals.spdx.json`.
6. Make decisions 1.2 and 1.3, and write their consequences into the Linux tooling (for
   example, a CPython source build step and its receipt).
7. Update both reviews' component tables and add the Qt HTTP Server note.

Each of steps 2–5 gets a unit test in `tests/blender_mesh_package_test.py`, in the style of
the existing ones.

### Phase B — Windows x64 (the Windows machine, with an RTX/Vulkan GPU for Gatling)

1. Build OpenUSD at the pin (or fetch the published asset), PySide6, vcpkg and usdcc
   with `--with-blender-mesh-tools`, and the Blender backend.
2. Download the Gatling CI artifact that matches the asset
   (`usdplugin-gatling-945f47d69-546ef27c-win64-py313`, or a newer build if the asset
   changed), and unzip it over the OpenUSD install.
3. `tools/deploy.py --with-blender-mesh-tools`, then `tools/blender_mesh_package.py` for
   the source bundle (it downloads the supplemental resources, the Gatling sources
   included).
4. Regenerate `reviewed_inputs` in `deps/blender-distribution.json` from what the
   machine has: the pins, `vcpkg_resource_sha512`, `pyside_commit`,
   `openusd_diff_sha256`, `submodule_commits`, `backend_input_sha256` and
   `tool_versions`. Compare each against §2 before accepting it. A change that §2 didn't
   predict needs reading first.
5. `tools/blender_distribution.py <deploy> --finalize`, then `--verify`.
   `tests/blender_mesh_packaged_smoke.py` against the package.
6. Start the packaged usdcc and render a frame with Gatling and with Storm. Use the Blender
   mesh tools once.

### Phase C — Linux x86-64 (Ubuntu 24.04 machine)

1. Python per decision 1.2.
2. The same build, Gatling artifact (`...-linux-x64-py313`), deploy and source bundle as
   on Windows. `tools/blender_ubuntu_sources.py` for the Ubuntu source receipts of the new
   runtime closure.
3. Regenerate `deps/blender-linux-distribution.json`'s `reviewed_inputs` from
   `evidence()`, comparing against §2 again.
4. `tools/blender_linux_distribution.py --finalize`, `--verify`, the restore check, and
   the packaged smoke test. Render with Gatling where there's a Vulkan ray tracing GPU.

### Phase D — closing

1. New review dates and receipts in both reviews; the Gatling sections lose "pending a
   renewed run".
2. [USD_PLUGINS.md](USD_PLUGINS.md) §5 and [RELEASE.md](RELEASE.md) §6: the configuration
   is current again.
3. Publishing the configuration remains a separate decision.

## 5. What can go wrong

- **The OpenUSD asset changes again** during the renewal (a new pin or patch). Every gate
  and the Gatling build name follow it, so finish the renewal against one asset, and pin
  that asset's name in the plan before starting Phase B.
- **CI artifacts expire after two weeks.** Download the Gatling packs at the start of
  Phase B and keep them with the machine's build inputs.
- **Transitive vcpkg ports.** The new ports may pull others into the runtime (curl's TLS
  and zlib, Qt's network backends). The audits fail on any port missing from
  `PORT_LICENSES`, which is the intended safety net, but plan a reading round for them.
- **The Blender compiler-input check** compares 418 files byte for byte against the
  Blender archive. The backend's CMake changes shouldn't change that list, but if the
  generated subsets change, they need reviewing.
