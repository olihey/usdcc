# Blender-enabled distribution review — 2026-11-15

This review covers the Windows x64 RelWithDebInfo configuration produced by
`tools/blender_distribution.py --finalize`, with the exact binaries and accompanying
source identified in `distribution-readiness.json`. It is an engineering license
and source-delivery assessment, not a legal opinion or a certification of other builds.
Linux has its own [review and source receipt](BLENDER_LINUX_DISTRIBUTION_REVIEW.md),
covering Ubuntu Python and the Linux dependency variants. The Windows receipt must
never be reused to approve Linux or another dependency pin.

## Distribution terms

Distribute the in-process usdcc/Blender combination under **GPL-3.0-or-later**.
Blender's selected files allow GPL-2.0-or-later; selecting GPLv3 accommodates
Apache-2.0 dependencies. This does not relicense the original BSD-3-Clause usdcc
and adapter files, or remove upstream terms. Root `LICENSE.txt` in this binary
configuration contains GPLv3; original BSD terms remain in `licenses/` and source.
The separate standard package retains its existing license and excludes Blender.

The entire `source/` directory travels with the binary package, without an
additional source charge: this is accompanying machine-readable source delivery
under GPLv3 section 6, not a promise to supply it later or a link-only source offer.
Recipients can copy, modify and redistribute the covered work. Do not add a
restrictive EULA, signature requirement, activation check, or restriction on
reverse engineering to debug modifications. The portable package has no locked
installation or device authorization mechanism requiring installation keys.

Qt, PySide6/Shiboken6 and ADS remain separate replaceable dynamic libraries.
Their source, modifications, build recipes and license texts accompany the package.
See `BLENDER_SOURCE_BUILD.md` for replacement and rebuilding instructions. No
proprietary renderer integration is approved: RTX and V-Ray are excluded. The Gatling
render delegate (GPL-3.0-or-later) may be included as the USD plugin pack reviewed below. Blender's name is descriptive; no endorsement is asserted.

## Reviewed components and license choices

| Component | Version / source evidence | Terms used and obligations |
|---|---|---|
| usdcc and original adapters/generators | Complete working-tree snapshot and per-file hashes | BSD-3-Clause retained; covered combination conveyed under GPLv3 |
| Blender native subset | `deps/blender.json`, immutable commit/archive SHA-256; explicit source list, nine generated subsets and compiler input log | GPL-2.0-or-later files exercised under GPLv3; original notices retained, generated files identify modifications/date |
| Eigen | 3.4.0, vcpkg source SHA-512 | MPL-2.0; `EIGEN_MPL2_ONLY` required; source supplied; no incompatible-secondary-license designation found in the used subset |
| fmt / PThreads4W | 12.2.0 / 3.0.0, private build SPDX/checksums | MIT / Apache-2.0; preserve notices; Windows only for PThreads4W |
| xxhash / wcwidth | Pinned Blender archive and backend notices | BSD-2-Clause / upstream permissive wcwidth notice |
| OpenUSD | NVIDIA fork commit in `deps/usd.json`; actual checkout snapshot | **TOST-1.0**, including embedded third-party notices in `LICENSE.txt`; `NOTICE.txt` required |
| OpenSubdiv | 3.6.1, actual dependency source | Modified Apache/TOST trademark clause; preserve its LICENSE.txt and NOTICE.txt |
| Embree / TBB | 4.3.3 source-built, SYCL and ISPC off / 2020.3 vendor runtime plus exact v2020.3 source commit in supplemental lock | Apache-2.0 and bundled permissive notices; no TBB debug, preview or optional hwloc binding binaries |
| MaterialX / QWindowKit | 1.39.5 / pinned submodule Git bundle | Apache-2.0; full notices, source and build recipes |
| Boost / GLM | 1.86.0 / 1.0.1 | BSL-1.0 / MIT; source and notices retained |
| OpenImageIO / OpenColorIO | 2.5.16.0 / 2.2.1 | BSD-3-Clause and bundled component notices; actual dependency and bundled source trees supplied |
| OpenEXR / Imath / yaml-cpp / pystring / minizip / expat / openqmc / OpenPBR | Actual source/build trees, including FetchContent and OCIO ext/build sources; CMake caches captured | BSD/MIT/Zlib/Apache and upstream per-file notices; no binary-only substitution accepted |
| JPEG / PNG / TIFF / zlib | Actual shipped variants identified by binary input hashes; vcpkg and USD variants both included in source | IJG/BSD/Zlib/Libpng/libtiff notices, including README.ijg and IJG attribution |
| Qt Core/Gui/Widgets/OpenGL/Svg and Qt plugins | 6.11.1 vcpkg source SHA-512, port definitions/patches, build snapshot | LGPL-3.0; preserve bundled third-party notices and replaceable dynamic linkage |
| PySide6 / Shiboken6 | v6.11.1, commit recorded in source receipt, actual source tree included | LGPL-3.0; replaceable DLLs/extensions, source and license collection |
| Qt Advanced Docking System / QtNodes | 4.5.0 / vcpkg pinned source | LGPL-2.1-or-later / BSD-3-Clause; LGPL libraries remain replaceable |
| CPython | **3.12.10**, not the unrelated vcpkg Python 3.12.13 source | Python/PSF license and bundled notices; exact official source archive |
| CPython's external libraries | OpenSSL 3.0.16, SQLite 3.49.1, libffi 3.4.4, bzip2 1.0.8, xz 5.2.5, zlib 1.3.1 | Official v3.12.10 externals SPDX SHA-256 receipts; Apache/MIT/public-domain/bzip2/Zlib/LGPL components; source, build scripts and notices supplied; expat is in CPython's source |
| Other shipped vcpkg runtimes | Exact installed input files and source SPDX checksums | ICU Unicode; Brotli/Harfbuzz/md4c/expat MIT; double-conversion/pcre2/zstd BSD; FreeType FTL; libpq PostgreSQL; OpenSSL Apache; bzip2; xz's library subset public-domain/LGPL; full copyright collections retained |
| Ruff / uv | 0.17.1 / 0.13.5, pinned source plus every registry dependency in both Cargo.lock files (938 unique crate versions) | Separate unmodified CLI tools, aggregated rather than linked; MIT / MIT-or-Apache plus crate terms. All crate sources and available upstream license/notice files supplied; crate license inventory retained. Dual choices may use MIT; MPL-only dependencies retain MPL terms/source. No git-sourced Cargo dependency is omitted. |
| Preview assets and icons | usdcc-generated shapes/HDRIs, USD WG Standard Shader Ball, Tabler icons | Original BSD, separately aggregated CC BY 4.0 assets with contributor/source attribution and intact LICENCE/README, MIT icons; bundled fonts retain OFL/Apache notices |
| Windows APIs and Microsoft release CRT | Enumerated imports and shipped runtime hashes | GPLv3 System Libraries exclusion from corresponding source; runtime binaries remain under Microsoft's redistribution terms, not GPL. Debug CRT is not distributable and is excluded. |

### Gatling pack (added 2026-11-27, pending a renewed run)

The configuration may ship one USD plugin pack, Gatling, in exactly the build
`usdplugin-gatling-945f47d69-546ef27c-win64-py313` (`deps/blender-distribution.json`,
`usd_plugin_packs`). Its licences were read in [USD_PLUGINS.md](USD_PLUGINS.md) §5:
Gatling is GPL-3.0-or-later; its 24 submodules, the nested googletest and the libdeflate
OpenEXR fetches are permissive (glslang's Bison output carries the Bison exception);
NVIDIA's MDL SDK 2026.0.2 and its MDL files are BSD-3-Clause, with a permissive
third-party list.

- **Deploy:** `tools/deploy.py --with-blender-mesh-tools` ships the packs in
  `GPL_CONFIGURATION_PACKS` the OpenUSD install has, file by file from the pack's list,
  and nothing of the standard set. Gatling isn't published, so the pack is the
  `deps-usd-plugins.yml` artifact of that name, unzipped over the OpenUSD install.
- **Audit:** `--finalize` takes the pack's binaries (`hdGatling.dll`, and
  `libmdl_sdk.dll` under `hdGatling/resources`) as component `Gatling` by the pack's own
  file list, records each one's licence, and rejects any other file of it. It rejects a
  pack the lock doesn't name, another build of it, and a build the pin, patches and build
  scripts in the source snapshot no longer name (`tools/ci/usd_plugin_asset.py`).
- **Source:** the lock's supplemental resources carry Gatling at `945f47d`, each submodule
  at the commit Gatling pins, googletest, libdeflate v1.18 and the MDL SDK's source at tag
  `2026.0.2`, by SHA-256. The patches and build scripts are in the usdcc snapshot. The
  audit refuses a pack whose sources aren't all in the lock.
- **Not yet settled:** the pack's `libmdl_sdk` is NVIDIA's prebuilt binary from the
  `2026.0.2` release; its source is the tag's, but that the binary was built from exactly
  it can't be checked here. Building it from source would settle that.

This addition hasn't been run end to end: the configuration as a whole needs a renewed
review for the current OpenUSD pin first (`deps/usd.json` moved to Python 3.13, oneTBB,
OpenVDB, Alembic and Draco on 2026-11-23), and `--finalize` refuses it until then.

OpenUSD is **not** plain Apache-2.0. Comparison of the actual pinned TOST section 6
with Apache-2.0 shows a narrower trademark grant, including affiliates. Our
compatibility assessment relies on GPLv3 section 7(e), which permits declining
trademark rights, while retaining the otherwise Apache-style terms. The modified
trademark clause is not a restriction on running, modifying or redistributing code.
This is our stated analysis; an Apache-only compatibility statement would not
establish compatibility for a different custom license.

The runtime audit matches shipped bytes to their installed inputs, enumerates normal
and delay imports, rejects unknown origins/ports and unreviewed USD binaries, and
accepts unresolved Windows API-set contracts or actual System32 libraries only.
The backend enables no Python, USD, TBB or internationalization. Compiler input
inspection found GPL-2.0-or-later Blender sources/headers, BSD support and Eigen's
MPL subset, not a GPL-2.0-only requirement. Embree uses the existing TBB runtime;
there are no SYCL, ISPC, CUDA or renderer SDK additions in the reviewed package.
All 418 original Blender source/header inputs found in the compiler logs are also
checked byte-for-byte against the immutable upstream archive; changed originals
or GPL-2.0-only inputs require renewed review. The supplemental lock records the
reviewed source/patch checksums, backend subset identity, submodule/PySide commits,
USD modifications and tool versions. A dependency change cannot inherit this review.

## Source and release checks

`SOURCE-INVENTORY.json` is inside the archive; its external sidecar additionally
contains the archive hash. Both identify the full project snapshot, binary hashes,
Blender and USD pins, supplemental archives, vcpkg resources, dependency source
trees, Git bundles for build-system baseline identity, and PySide commit. Include
the exact generated Blender sources, upstream archive, generator, source selection,
all patches, CMake/toolchain configuration and USD dependency build caches.
The modified USD build script carries an added modification/date comment in the
source snapshot, with the GLM patch preserved; the annotation changes no executable code.
General unmodified compilers, SDKs and standard build tools are installed separately.

Finalization requires that source files still match, backend input hashes and feature
choices match, all pinned source resources are present, the binary set matches the
source receipt, and required notices exist. It hashes **all** resulting deliverable
files. `--verify` rejects additions, removals or changes. These are integrity and
policy checks; a hash is not itself evidence of a license grant. This document is
the review rationale, and changes to dependencies require renewed review.

No release was uploaded or published as part of this review. Source restoration and
package smoke checks are recorded alongside the local artifact; this review does
not claim a fresh operating-system installation or a byte-for-byte rebuild of all
third-party libraries. Do not strip `source/` or `licenses/` when making a download.

## Primary references

- [Blender license](https://www.blender.org/about/license/).
- [GPLv3 sections 1, 5–7 and 14](https://www.gnu.org/licenses/gpl-3.0.html).
- [GNU linking and source FAQ](https://www.gnu.org/licenses/gpl-faq.html).
- [Apache's GPL compatibility explanation](https://www.apache.org/licenses/GPL-compatibility).
- [Pinned OpenUSD license](https://github.com/NVIDIA-Omniverse/OpenUSD/blob/70c45e83b08a7b45588cd6cdbd2ae67f5147befb/LICENSE.txt).
- [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.html) and [LGPLv2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).
- [CPython 3.12.10 externals source receipts](https://github.com/python/cpython/blob/v3.12.10/Misc/externals.spdx.json).
- [Microsoft runtime redistribution](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files).
