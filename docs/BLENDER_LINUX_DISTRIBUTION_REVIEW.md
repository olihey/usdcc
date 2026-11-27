# Linux Blender distribution review — 2026-11-16

This review covers the **Ubuntu 24.04, x86-64, RelWithDebInfo** configuration
produced by `tools/blender_linux_distribution.py`. The exact artifact is identified
by `distribution-readiness.json`, its source receipt and the download checksum.
This is an artifact-specific license/source assessment; it does not approve arbitrary
Linux builds or a new dependency revision. The Windows receipt cannot approve Linux.

## Terms and source delivery

Convey the in-process usdcc/Blender combination under **GPL-3.0-or-later**.
Original usdcc, adapter and build-tool sources retain BSD-3-Clause; Blender files
retain their upstream GPL notices. The [common component review](BLENDER_DISTRIBUTION_REVIEW.md)
explains the GPLv3 choice, Apache dependencies and TOST trademark analysis.
OpenUSD's actual TOST `LICENSE.txt` and `NOTICE.txt` accompany this package;
MaterialX's install-root license is not a substitute.

The binary download includes the entire `source/` directory, with source archives,
DSC/orig/debian source packages, patches, generated subsets, dependency Git bundles,
build configuration and restoration instructions. This provides source alongside
the binaries without a source surcharge; it is not a promise to supply it later.
Keep `source/` and `licenses/` in the download. Do not add a restrictive EULA,
activation/signature check, or restriction on debugging modifications.

Qt, ADS, the native backend and other shared libraries remain replaceable.
The app has no installation keys or locked-device mechanism.
Ruff/uv remain independently licensed unmodified CLI tools aggregated with it.
RTX, V-Ray, TBB debug/preview/optional binding runtimes and other unreviewed native
additions are excluded. The Gatling render delegate may be included, in exactly the
build `usdplugin-gatling-945f47d69-546ef27c-linux-x64-py313`
(`deps/blender-linux-distribution.json`, `usd_plugin_packs`), as the
[Windows review](BLENDER_DISTRIBUTION_REVIEW.md)'s "Gatling pack" section describes:
`hdGatling.so` (GPL-3.0-or-later) and `libmdl_sdk.so` (BSD-3-Clause), audited by the
pack's own file list, with its sources among the supplemental resources this
configuration takes from the Windows lock. Added 2026-11-27, not yet run end to end:
the configuration needs a renewed review for the current OpenUSD pin first. Blender's name is descriptive attribution,
without a Foundation or Pixar endorsement claim.

## Actual Linux variants

| Component | Reviewed variant and evidence | Terms / delivery |
|---|---|---|
| Blender | Pinned 5.2.4 archive; 418 original compiler source/header inputs match it; nine generated subsets and generator included | GPL-2.0-or-later exercised under GPLv3; dated modification notices retained |
| Native backend | GNU C++ 13.3.0, release CRT, unsigned char, no Python/USD/TBB/internationalization; private fmt 12.2.0 | Original BSD adapters plus covered GPL combination; fmt MIT; no Windows PThreads4W runtime |
| Eigen | 3.4.0 with `EIGEN_MPL2_ONLY` | MPL-2.0 and upstream permissive notices; complete pinned source |
| OpenUSD | NVIDIA fork pin in `deps/usd.json`, actual GLM-modified source snapshot and dependency build caches | TOST-1.0 plus embedded notices; source and patch provided; modified build script carries a modification/date annotation |
| USD dependencies | **Boost 1.80.0** and **TBB 2020.3.1**, Embree 4.3.3 without SYCL/ISPC, MaterialX 1.39.5, OCIO 2.2.1, OIIO 2.5.16.0, OpenEXR 3.1.13, OpenSubdiv 3.6.1 and bundled support | Actual complete source trees, not Windows library variants or binary-only TBB; Apache/TOST/BSD/BSL/MIT and bundled notices preserved |
| Qt / ADS / QtNodes | Qt 6.11.1 including Linux platform/SQL plugins, ADS 4.5.0 and pinned QtNodes | LGPL-3.0 / LGPL-2.1-or-later / BSD-3-Clause; dynamically replaceable; vcpkg source archives, ports and patches supplied |
| Python interpreter and core stdlib | **Ubuntu python3.12 3.12.3-1ubuntu0.17** | Python/PSF and bundled component terms; exact Ubuntu DSC, upstream orig archive, Debian/Ubuntu patches and build rules supplied |
| Separately built Python extensions | **Ubuntu python3-stdlib-extensions 3.12.3-0ubuntu1** | Same source-delivery approach, preserving this distinct source version |
| D-Bus | vcpkg 1.16.2#5 | Select **GPL-2.0-or-later under GPLv3**, rather than relying on AFL compatibility; full source and notices |
| libsystemd | vcpkg 260.2, library subset | LGPL-2.1-or-later with permissive components; full source and notice collection; systemd helper executables are not shipped |
| libmount/libblkid / libxcrypt | vcpkg 2.41.3#1 / 4.5.2 | LGPL-2.1-or-later library subsets and permissive components; unrelated GPL-only utilities are not shipped |
| libcap / libb2 / fontconfig | vcpkg 2.78 / pinned port / 2.17.1#2 | Select BSD-3-Clause for libcap's BSD-or-GPL-2.0-only choice; libb2 CC0; fontconfig MIT-style terms; notices/source supplied |
| Other vcpkg libraries | ICU, FreeType, HarfBuzz, Brotli, bzip2, double-conversion, expat, JPEG/PNG, xz, libpq, lz4, md4c, OpenSSL, pcre2, SQLite, zlib, zstd | Actual installed versions and notice hashes in the review lock; permissive terms/FTL/LGPL library subsets; source and all collected notices. Both shipped USD/vcpkg image-library variants are covered. |
| System-linked Readline / GDBM | Ubuntu Readline 8.2 and GDBM 1.23 with exact distro revisions | GPL-3.0-or-later compatible with this combination; complete corresponding Ubuntu source packages and copyright files included |
| System-linked Berkeley DB | Ubuntu db5.3 5.3.28+dfsg2-7 | Sleepycat/BSD notices; full DB source and full app source supplied; no proprietary-source restriction added |
| Remaining non-core system-linked libraries | X11/XCB, Wayland, xkbcommon, GLVND, ICE/SM/Xt, OpenSSL, SQLite, libffi, bzip2/xz, ncurses, libbsd/libmd and util-linux | Exact Ubuntu source publications and notices included as well, even though these libraries remain on the host |
| Core system interfaces | Ubuntu glibc 2.39-0ubuntu8.12 and GCC runtime source version 14.2.0-4ubuntu2~24.04.1 in the tested profile | Unbundled C/C++ compiler/OS System Libraries; source exclusion under GPLv3 section 1; copyright/license materials retained. GCC runtime exception applies to its designated runtime files. |
| Graphics drivers / kernel | Host interfaces, not part of the download | Standard OS/window-system/graphics-driver interfaces; no driver or kernel binaries are distributed |
| Ruff / uv | 0.17.1 / 0.13.5 and 938 unique Cargo.lock crate versions | MIT / MIT-or-Apache plus original crate terms; 940 source archives, available license/notice files and crate license inventory supplied |
| Assets/icons/fonts | Same preview assets and icon set as the common review | BSD, CC BY 4.0, MIT, OFL/Apache font notices; attribution and original asset licenses preserved |

There is **no PySide6/Shiboken6 in this Linux configuration**; no claim is made that
the Windows PySide source describes Linux binaries. The native C++ menus and mesh
tools operate without it. This review does not approve adding a binary PySide wheel
with a second Qt build. No Windows CPython 3.12.10 source is substituted for Ubuntu
Python, and no Windows TBB archive is used to establish Linux source completeness.

## Source provenance and gates

The checked Linux lock records the exact upstream pins, source/patch checksums,
compiler-input hashes, backend/toolchain identity, vcpkg notice hashes and versions,
Ubuntu source publications/files, source-tree variants and tool versions. A changed
dependency requires renewed review. Forty vcpkg source resources and **31 exact
Ubuntu source packages** cover the reviewed closure; each Ubuntu orig/debian archive
matches its official DSC SHA-256 inventory. Launchpad publication identity and HTTPS
source URLs are recorded, and original uploader signatures are retained. The audit
does not claim it independently authenticated every uploader's PGP key.

ELF provenance compares the build ID and loadable code/data sections against the
actual installed inputs, allowing the deployment script's RUNPATH relocation.
The final report additionally records whole-file hashes. It rejects unknown
runtime origins/ports, incompatible platform receipts, excluded renderer/debug
runtimes, unresolved dependencies and references into the build tree. All deployed
Python standard-library/native variants are traced to their actual source packages.
Source checks bind the binary set, full project snapshot, pinned upstream sources,
Ubuntu DSC files, modified source/recipes and required archive members.

The final manifest covers regular-file hashes/modes and symlink targets, including
the source archive and notices. Broken/escaping symlinks, changed permissions,
changed files and added/removed content invalidate verification. Source restoration
and Ubuntu patch application are checked without network access; the packaged
runtime is tested from a relocated extraction with an isolated profile and PATH.
The full Linux regression suite and native backend smoke checks pass.

This is a portable Ubuntu 24.04 x86-64 configuration, not a universal-Linux binary
or a fresh-OS certification. The source/build instructions do not claim a byte-for-byte
rebuild of every Ubuntu binary. No artifact was uploaded or published during review.

## Primary references

- [GNU GPLv3, especially sections 1 and 5–7](https://www.gnu.org/licenses/gpl-3.0.html).
- [GNU license compatibility list, including Berkeley DB](https://www.gnu.org/licenses/license-list.html#BerkeleyDB).
- [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html).
- [systemd library licensing](https://github.com/systemd/systemd/blob/main/LICENSES/README.md).
- [Ubuntu primary source archive](https://api.launchpad.net/1.0/ubuntu/+archive/primary);
  the exact source publication/file links are in `licenses/Ubuntu/source-publications.json`.
- [Ubuntu packaging source/patch documentation](https://canonical-ubuntu-packaging-guide.readthedocs-hosted.com/en/latest/).
