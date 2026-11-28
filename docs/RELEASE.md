# Releases

Hosted release builds explicitly omit the optional native RTX target. To make an
RTX-enabled package locally, build with the pinned native SDKs and use the
deployment/clean-package checks in [RTX_RENDER.md](RTX_RENDER.md). Runtime DLLs
are external by default; `--bundle-rtx-runtime` is explicit and includes the SDK's
licenses. The retired Python RTX plugin is never copied into a release.

How a usdcc release is made: you create a release on GitHub, and CI builds the Windows 7z
and attaches it. The app shows its version in a new **Help > About usdcc** dialog. Windows
only for now, like the rest of CI ([CI.md](CI.md)). This is part of M11 (packaging), and
the dialog is the first step of M10's Info dialog ([PLAN.md](PLAN.md)).

**Status:** built (§5).

**To make a release:** write its section of [CHANGELOG.md](../CHANGELOG.md) and merge it
(§8). Then on GitHub, Releases > Draft a new release, a new tag that is the
version (`26.12.1`, or `26.12.1-beta.1` with "Set as a pre-release"; §1 says which
number is next) on `master`, the notes
(that section and the open known issues, §9, then "Generate release notes" for the list
of pull requests),
then Publish; or do both from a shell (§8). About 25 minutes later the release has `usdcc-<version>-windows-x64.7z` and
its symbols. If the run fails, fix the cause and run the Release workflow by hand with the
same tag (Actions > Release > Run workflow).

## 1. Decisions

- **The release's tag is the version.** You create a release with the tag `26.12.1` (or
  `26.12.1-beta.1` for a prerelease; a leading `v`, as in `v26.12.1`, works too), and
  that build is version `26.12.1`. Nothing to bump by
  hand before releasing; the hand-kept `0.1.0` in `CMakeLists.txt` goes.
- **A version is `YY.MM.N`:** the year's last two digits, the month without a leading
  zero, and a counter that starts at 1 each month and goes up by one with every release.
  `26.12.2` is December 2026's second release, whether that is the same day as the first
  or two weeks later. It stays three numbers that only go up, because an install is only
  offered a higher version: [UPDATES.md](UPDATES.md) §5, "Version numbers", says why a
  fourth number or a suffix doesn't do, how to find the next number, and what the change
  from four-digit years means for the installs of `2026.11.18` to `2026.11.20`. Those
  releases, and `0.0.1` to `0.0.3` before them, are deleted from GitHub, tags and all.
- **Every other build is a development build**, named after the last release before it:
  `26.12.1+5.3b70ef2` is 5 commits after `26.12.1`, at commit `3b70ef2`. The About dialog
  says "Development build" for these. With no release tag yet, it's `0.0.0+<commit>`.
- **CI builds the archive from the tag, not from `master`:** exactly the commit you released.
- **The archive is `tools/deploy.py`'s folder**, as it is today, so a release is what you
  already run locally.
- **Tests gate the release.** If they fail, nothing is attached, and the run says why.
- **Compressed as small as it goes:** 7z (LZMA2, solid) on Windows and `tar.xz` on
  Linux, both at their strongest settings (§4, step 8). Windows 11 (23H2 and later) opens
  a `.7z` in Explorer; Windows 10 needs 7-Zip. Releases up to `0.0.1` were zips and
  `tar.gz`.
- **Symbols ship separately.** The build is `RelWithDebInfo`, like local builds; the PDBs go
  in a second archive on the same release, so a crash dump from a user can be read.

## 2. The version in the build

**Where it comes from.** `cmake/modules/UsdccVersion.cmake` works it out:

1. `-DUSDCC_VERSION=26.12.1`, which the release workflow passes from the tag.
2. Otherwise `git describe --tags --match "v[0-9]*" --match "[0-9]*"`: the last release
   tag, the commits since it, the commit, and whether files are changed (`+dirty`).
3. Otherwise (no git, no tags) `0.0.0`.

**Two parts, updated at different times:**

- **The release number** (`26.12.1`), worked out at configure time. It's
  `project(VERSION)`, and it's what C++ plugins are checked against (`built_for.usdcc`,
  [PLUGIN_SDK.md](PLUGIN_SDK.md) §3). So a plugin built for 26.12.1 loads in 26.12.1
  and in development builds after it, but not in 26.12.2, without anyone having to
  remember to raise a version.
- **The build's identity** (commits since the tag, commit, `dirty`, build date), written at
  build time into a small generated source in `usdcc_core` (`cmake/scripts/write_version.cmake`).
  It's rewritten only when it changes, so a new commit recompiles one file, and the About
  dialog never shows a stale commit.

`tools/build.py` always passes `-DUSDCC_VERSION`, empty without `--usdcc-version`, so a
release build's version doesn't stay in the CMake cache for the builds after it. A new tag
reaches the release number at the next configure. CI's ordinary builds check out without
tags, so they're `0.0.0+<commit>`.

**What reads it:** `usdcc/core/version.h` (`usdcc::core::version()`, `releaseNumber()`,
`isReleaseBuild()`, `commit()`, `buildDate()`), used by:

- the About dialog (§3);
- the log's first line at startup ("usdcc 26.12.1+5.3b70ef2 (development build) starting"),
  so every log file says which build wrote it;
- Python, as `usdcc.app.version()` and `usdcc.app.is_release_build()`, for scripts and
  plugins;
- the `.exe`'s version resource (`resources/usdcc.rc`), shown in Explorer's
  Properties > Details: the release number, and "(development build)", marked prerelease,
  for a development build.

## 3. The About dialog

**Help > About usdcc**, in a Help menu just before Debug at the end of the menu bar. The same menu has
USD Runtime and Project Home (docs/USD_RUNTIME.md).

- The app icon, "usdcc", the version in large type, and "Development build" under it when
  it isn't a release.
- The commit, linked to it on GitHub, and the build date.
- The components it runs with: OpenUSD, MaterialX (when OpenUSD was built with it), Qt,
  Python, PySide6 (when present), and the compiler. Read at run time where possible (`qVersion()`, Python's `sys.version`), so the
  dialog shows what is actually loaded.
- Links to the repository and its releases.
- **Copy**, which copies all of it as plain text for a bug report.

`usdcc::ui::AboutDialog` lives in `usdcc_ui` and is given its rows. The app fills them in
(`main.cpp`), because `usdcc_ui` doesn't know about Python: OpenUSD's version from
`pxr/pxr.h`, MaterialX's from `MaterialXCore/Generated.h` (the version OpenUSD was built
with; the row is left out when the header isn't there), Python's from the running interpreter, PySide6's from the module when it's
there. A test in `usdcc_ui_tests` checks the rows and the copied text. The app's stylesheet
sets font sizes, so the larger title and version are sized with a stylesheet on the label.

M10's Info dialog (third-party licences, registered USD extensions) can grow out of it
later as a second tab.

## 4. Workflow `release.yml`

The `windows` job is described here; the `linux` job does the same on `ubuntu-24.04` and
attaches `usdcc-<version>-linux-x64.tar.xz`, whose folder runs from wherever it is
unpacked (`./usdcc`), and `usdcc-<version>-linux-x64-symbols.tar.xz`, the `.debug` files of
usdcc's own binaries: unpacked in the same place, each lands next to its binary, where gdb
finds it ([CI.md](CI.md) §8). On x64 it also attaches each USD plugin pack of the optional
set as `usdcc-<version>-linux-x64-pack-<pack>.tar.xz` (MoonRay's, about 200 MB), which usdcc
downloads and installs itself ([USD_PLUGINS.md](USD_PLUGINS.md) §10); the archive to unpack
over the usdcc folder by hand is gone since 2026-11-28 (§5 there). Each job runs twice, x64 and arm64 (`windows-arm64` on
`windows-11-arm`, `linux-arm64` on `ubuntu-24.04-arm`), and `arm64` takes `x64`'s place in
the arm64 files' names ([ARM64_RELEASES.md](ARM64_RELEASES.md)).

The `linux` jobs also attach `usdcc-linux-<arch>.AppImage`, and they and the `macos` job the packages
that installs update from ([UPDATES.md](UPDATES.md) §9). This hasn't run yet.

The `macos` job attaches `usdcc-<version>-macos-arm64.dmg`, with `usdcc.app` and a link to
Applications, and the dSYMs of usdcc's own binaries. And
`usdcc-<version>-macos-arm64-pack-<pack>.tar.xz` for each optional USD plugin pack, which usdcc
installs itself ([USD_PLUGINS.md](USD_PLUGINS.md) §10); the second disk image, usdcc with the
pack, is gone since 2026-11-28 (§5 there). It has not run yet (2026-11-23).
`tools/deploy.py` makes the bundle and checks it, `tools/ci/split_dsym.py` takes the symbols
out, and the smoke test runs a copy from the disk image.

**The app is signed ad hoc, not by a Developer ID**, so macOS refuses a downloaded one at
first. Say so in the release's section of the changelog: open it once, then System Settings >
Privacy & Security > Open Anyway. With a Developer ID the secret `MACOS_SIGN_IDENTITY` names
it (`tools/deploy.py --sign`, with the hardened runtime and
`src/cpp/app/resources/macos/entitlements.plist`); importing its certificate and notarizing
the disk image have to be added. The hardened runtime has been tried with an ad-hoc
signature: the smoke test passes.

**Trigger:** `release: published`, when you publish a release (or prerelease) on GitHub.
Drafts don't trigger it, so you can prepare one first. It also runs by hand
(`workflow_dispatch`) with a tag, to rebuild a release whose run failed, or with `dry_run`
to try the branch it's run on without attaching anything ([ARM64_RELEASES.md](ARM64_RELEASES.md) §4).

1. **Check the tag.** It has to be a version (`26.12.1`, `v26.12.1`,
   `26.12.1-beta.1`): a two-digit year, a month from 1 to 12, a counter from 1, none
   with a leading zero, and nothing after them but a prerelease's suffix. So a tag with
   the year in full (`2026.12.1`), which every install would take for the newest release
   from then on ([UPDATES.md](UPDATES.md) §5), is turned down.
   The first version of this workflow only took a tag with the `v`, and turned down the
   first release, `0.0.1`.
   Otherwise the run fails before building anything.
2. **Check out the tag**, with submodules.
3. **The same steps as `build.yml`:** OpenUSD and PySide6 from their release assets, Qt
   from the vcpkg cache. PySide6 is required here, not optional: a release without
   scripting panels would be broken. So are the USD plugin packs of the standard set
   (`tools/fetch_usd.py --packs-only --require-packs`, [USD_PLUGINS.md](USD_PLUGINS.md) §5);
   the smoke test in step 7 checks the deploy has them and that their file formats read
   a sample each (`--expect-standard-usd-packs`).
4. **Build** with `tools/build.py --usdcc-version <version>` (a new parameter that passes
   `-DUSDCC_VERSION`).
5. **Run the tests.**
6. **Deploy** with `tools/deploy.py` into `usdcc-<version>/`, and check it has the
   licences of OpenUSD's dependencies, PySide6's and vcpkg's (§6).
7. **Start the deploy** (`tools/ci/smoke_test.py`). `bin\usdcc.exe` from the deploy
   folder, with a `PATH` of Windows' own folders only, on Qt's offscreen platform, with
   `--no-startup-scripts --script check.py` (the shipped plugins load as usual). The script
   records `usdcc.app.version()`, the OpenUSD, Python and PySide6 versions, and where `pxr`,
   `usdcc` and `PySide6` were imported from, and quits the app. It fails if the app
   doesn't quit within 3 minutes, if the version isn't the tag's, or if a module came from
   outside the deploy. That catches a missing DLL or Python file that the build's own
   folders were hiding; it prints the app's log when it fails. The deploy only has Qt's
   `windows` platform plugin, so the offscreen one comes from vcpkg's folder.
8. **Pack** with `7z a -t7z -mx=9 -md=256m -mfb=273 -ms=on -mqs=on -mmt=on`: LZMA2 with a
   256 MB dictionary in one solid block, the files sorted by type so the DLLs compress
   against each other, and 7-Zip's x86 filter on the executables. Unpacking needs about
   256 MB of memory. Linux uses `xz -9e -T0` (a 64 MB dictionary; all cores, in 192 MB
   blocks, about 1% larger than one thread would make it). On a 620 MB Windows deploy
   (2,337 files): 130 MB as 7z in 84 s, against 253 MB as the old zip (`-mx=7`) in 17 s;
   153 MB as `tar.xz` against 261 MB as `tar.gz`.
   - `usdcc-<version>-windows-x64.7z`: the deploy folder, `usdcc-<version>/`.
   - `usdcc-<version>-windows-x64-symbols.7z`: the PDBs of usdcc's own programs and
     libraries (the exe, its DLLs, the `usdcc` Python modules, the example C++ plugin),
     laid out as they are in the deploy.
9. **Attach** both to the release with `gh release upload`.
10. **The installer and the update packages**, made from the same deploy with Velopack's
    `vpk` and attached with it: `usdcc-win-Setup.exe`, the packages and the feed
    installs update from ([UPDATES.md](UPDATES.md) §5).

It uses `build.yml`'s vcpkg cache, read-only.

**What you see:** the release appears as soon as you publish it, and the archives appear about
15 minutes later. If the run fails, the release stays without them. Fix the cause, then run
`release.yml` by hand with the same tag.

## 5. Phases

1. **Version and About dialog.** `cmake/modules/UsdccVersion.cmake`, `usdcc/core/version.h`,
   the startup log line, `usdcc.app.version()`, the `.exe` version resource, the Help menu
   and `AboutDialog` with its test, and `tools/build.py --usdcc-version`. All local, testable
   without a release. Done when a local build shows `0.0.0+<commit>` as a development
   build, and `tools/build.py --usdcc-version 0.2.0` shows `0.2.0` as a release.
   *Done. A local build is `0.0.0+5cb3857.dirty`; `write_version.cmake` gives
   `0.2.0-beta.1` for a release and `0.1.0+1.5cb3857.dirty` one commit after a (temporary)
   `v0.1.0` tag; a version such as `v0.3` is refused at configure time.*
2. **Release workflow.** `release.yml` as in §4, with the smoke test. Done when a test
   prerelease (`v0.0.1-test.1`) gets both zips, and its zip runs on a machine without the
   dev environment. The test release and its tag are deleted afterwards.
   *Done, with a real release instead of a test one: `0.0.1`. Its first run turned the tag
   down (only `v0.0.1` was accepted then), and its second failed a test that called a
   plugin built for usdcc 0.0.1 wrong; the tag was moved to the fix, which had nothing
   attached yet. The third took 14 minutes: tests green, the deploy started as `0.0.1`
   with `pxr`, `usdcc` and PySide6 from its own folder, and it attached
   `usdcc-0.0.1-windows-x64.zip` (166 MB) and the symbols (36 MB, 9 PDBs). That zip,
   downloaded and unzipped on a development machine, passed the same smoke test, and its
   `usdcc.exe` reports 0.0.1, not a prerelease.*
3. **Third-party licences** (§6), before the first real release. *Done: a local deploy has
   29 vcpkg licences, 20 of OpenUSD's dependencies, PySide6's, Python's, QWindowKit's,
   ruff's and uv's, and `SOURCES.txt`.*

## 6. Licences

**The standard usdcc distribution is BSD-3-Clause** (`LICENSE`), which every library
in that dependency set allows. None of those libraries requires usdcc to be under
the GPL. The planned optional Blender configuration is covered separately below:

- **Permissive:** OpenUSD (TOST, based on Apache 2.0), TBB, Embree, MaterialX, OpenSSL,
  QWindowKit (Apache 2.0); QtNodes, OpenColorIO, pybind11, zstd, libjpeg-turbo (BSD); Jolt
  Physics (MIT), in `plugins/physics_jolt` with its licence in that folder's `licenses/`
  (docs/PHYSICS.md); and MIT, zlib, ICU, PostgreSQL, Python (PSF) and public-domain licences
  for the rest.
- **LGPL:** Qt and PySide6 (LGPL v3), Qt Advanced Docking System (LGPL v2.1). Allowed
  because they stay separate DLLs the user can replace, as they already are.
- **Dual-licensed, used under the permissive option:** freetype (FTL), zstd (BSD), Qt (LGPL).
  dbus (AFL or GPL) is installed by vcpkg but not shipped.
- **No GPL-only Qt module.** Some Qt modules are offered only under a commercial licence or
  GPL-3.0, with no LGPL option: Qt HTTP Server, Charts, Data Visualization, Graphs, Virtual
  Keyboard, Quick 3D, Quick Timeline, Lottie, CoAP, MQTT, OPC UA, Wayland Compositor, Protobuf,
  gRPC and others. The standard release can't ship one: it is BSD-3-Clause, and the RTX Render
  plugin loads NVIDIA's proprietary runtime into the same process. `tools/deploy.py` fails a
  standard deploy that has the library (or PySide6 binding) of one, on every platform
  (`GPL_ONLY_QT_MODULES`, tested by `tests/deploy_gpl_qt_test.py`); only the GPL-3.0
  configuration below may.
  **26.11.1 to 26.11.11 shipped one:** `Qt6HttpServer` (with `Qt6Concurrent`, which only it
  needed), linked by `usdcc_peers` for the page a room serves to guests. From the next release
  the room serves that page with Qt Network and Qt WebSockets alone ([PEERS.md](PEERS.md) §6.1),
  and the library is no longer built or deployed.

What the release has to do, whatever usdcc's own licence:

- **Ship the licence texts.** `tools/deploy.py` gathers them into `licenses/`, and usdcc's
  own `LICENSE` goes at the top as `LICENSE.txt`:
  - `vcpkg/<port>.txt`: each vcpkg package's `share/<port>/copyright` (not vcpkg's own
    build helpers).
  - `OpenUSD/`: OpenUSD's actual `LICENSE.txt` (TOST) and `NOTICE.txt`. The install
    root's `LICENSE`/`THIRD-PARTY.md` may belong to MaterialX and do not substitute
    for OpenUSD's notices. New USD builds preserve these in `licenses/OpenUSD/`.
  - `OpenUSD-dependencies/<library>/`: the licence and NOTICE files of the libraries
    `build_usd.py` builds, which its install leaves out. `tools/ci/usd_licenses.py`
    collects them from its source and build folders (top-level licence files of each
    source, and the libraries OpenColorIO and OpenEXR build in themselves).
    `deps-usd.yml` runs it before deleting those folders, into the asset's `licenses/`;
    `deploy.py` runs it for a local install that still has them.
  - `OpenUSD-plugins/<pack>/<library>/`: for each USD plugin pack in the deploy, the
    licences of the plugins and of the libraries built into them, which
    `tools/build_usd_plugin.py` collects into the pack ([USD_PLUGINS.md](USD_PLUGINS.md)).
    `SOURCES.txt` names each pack's version, repository and commit.
  - `PySide6/`: pyside-setup's `LICENSES`, which `tools/build_pyside.py` copies into its
    install.
  - `Python/`, `QWindowKit/`, `ruff/`, `uv/`, `debugpy/`, `types-usd/`.
- **Make the LGPL libraries' sources available.** `licenses/SOURCES.txt` names, for every
  vcpkg package, its version, the vcpkg port it was built with and its upstream source
  (from the SPDX file vcpkg writes), and PySide6's tag.
- **Keep the LGPL libraries replaceable DLLs.** Never link them statically.

The About dialog's licence tab (M10's Info dialog) can later list the same `licenses/`
folder.

### Optional Blender mesh tools distribution

The [Blender mesh tools plan](BLENDER_MESH_TOOLS.md) adds an optional native plugin.
Its six editing commands, jobs and conversion are implemented and built with every
app (`USDCC_BUILD_BLENDER_MESH_TOOLS`, on by default; [BUILD_OPTIONS.md](BUILD_OPTIONS.md)).
Standard deploy explicitly excludes its source and any stale built
plugin from the examples folders on Windows and Linux.
`--with-blender-mesh-tools` creates a separate combined candidate, excludes RTX/V-Ray
integrations, and records pending distribution review. `--blender-source-bundle`
adds the source archive/sidecar from `tools/blender_mesh_package.py`. See the plan's
section 15 for isolated-runtime smoke checks, CI reports, and the publication gate.
Our independently written code stays BSD-3-Clause, while Blender source retains its
upstream licenses. Treat the linked app/plugin configuration as a combined work
for GPL compliance; a separate plugin download does not establish a license boundary.

Keep Blender code out of the standard artifact. Before publishing the optional
configuration, audit its complete dependency set, including OpenUSD/TOST, and select
the applicable GPL terms. Exclude proprietary SDK integrations unless compatibility
has been confirmed. Ship the required licenses/notices and provide corresponding
source for the covered combination, including the exact usdcc/Blender revisions,
patches, and build/install scripts, with clear source access beside the binaries.
The existing LGPL obligations still apply. The Windows review is recorded in
[BLENDER_DISTRIBUTION_REVIEW.md](BLENDER_DISTRIBUTION_REVIEW.md), with source and
library-replacement instructions in [BLENDER_SOURCE_BUILD.md](BLENDER_SOURCE_BUILD.md).
After deploy and smoke checks, collect sources for the exact package, run
`tools/blender_distribution.py --finalize` with that source archive, and `--verify`.
Finalization applies GPL terms, installs full notices/source and binds every delivered
file to the report. Ship the entire package, including `source/` and `licenses/`.
Linux has its own [review](BLENDER_LINUX_DISTRIBUTION_REVIEW.md) and
[source/build instructions](BLENDER_LINUX_SOURCE_BUILD.md). Use
`tools/blender_linux_distribution.py` to collect evidence/source, finalize and verify
the Ubuntu 24.04 x86-64 artifact. Its source includes the exact Ubuntu Python patches
and 31 Ubuntu source packages. The Windows receipt cannot approve Linux.

## 7. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should `build.yml` keep the deploy zip of every `master` build as a workflow artifact too (CI.md §7, "Deploy artifact")? | Yes, once phase 2 works: the same steps 6–8, kept for a few days, for testing a build before releasing it |
| 2 | The repository's Releases page also lists the `deps/…` prereleases (OpenUSD, PySide6) | Fine for now: they're prereleases, so they never become "Latest". Move them to a separate repository if they crowd the page |
| 3 | An installer (MSI or similar) rather than a zip? | Done, beside the 7z: Velopack's, which also updates an install ([UPDATES.md](UPDATES.md)). A browser's download mark on the 7z made Defender scan each of its unsigned DLLs at the first start |

## 8. Pull requests, the changelog and release notes

A release's notes say what changed since the one before. Three things make them, and
each feeds the next.

**Every change reaches `master` through a pull request, merged once its Build passes.**
Not for review: nobody has to approve it. The pull request is where the change is
described, what gets built and tested before it is on `master`, and what GitHub builds a
release's notes from. A commit pushed straight to `master` is in no release's generated
notes.

1. The work is committed on a branch of its own, and the branch is pushed.
2. `gh pr create --base master`, with:
   - **a title** that says what changed, as the commit's subject does. It becomes a line of
     the release's notes, so it's written for someone who uses usdcc, not for whoever
     reads the diff;
   - **a description**: what changed and why, and how it was checked;
   - **one label**, which picks the heading the title goes under:

     | Label | Heading | For |
     |---|---|---|
     | `enhancement` | New | A feature, or more of one |
     | `bug` | Fixed | Something that was wrong, in the app or a test |
     | `build` | Build, CI and packaging | The build, the workflows, dependencies, the deploy, the installer |
     | `documentation` | Plans and documentation | A plan, a spike's results, the docs |
     | `skip-changelog` | (left out) | What no reader of the notes needs, such as a typo |

     `.github/release.yml` maps the labels to the headings. A pull request with none of
     them is listed under "Other changes".
3. `gh pr merge <number> --squash --auto`, right after creating it. Opening the pull
   request starts `build.yml` ([CI.md](CI.md) §4), and GitHub merges it when both jobs
   have passed, about 25 minutes later: one commit on `master` per pull request, with the
   pull request's number in its subject. The repository deletes the branch once it's
   merged.
4. **When the Build fails, nothing happens:** the pull request stays open and `master` is
   as it was. A push to the branch that fixes it starts the Build again, and the merge
   follows by itself. `gh pr merge <number> --disable-auto` takes the merge back.

Two settings of the repository do the waiting, so nobody has to watch the run:

- **`master` is protected, with `windows` and `linux` as required status checks.** These
  are `build.yml`'s two jobs, by name. The branch doesn't have to be level with `master`
  ("Require branches to be up to date" is off): several sessions merge at once, and each
  would otherwise build again after every other merge. Administrators aren't held to it,
  so the owner can still merge by hand (`gh pr merge --squash --admin`) when the Build
  itself is what's broken.
- **Auto-merge is allowed** (Settings > General > Pull Requests), which is what `--auto`
  needs.

```
gh api -X PATCH repos/olihey/usdcc -F allow_auto_merge=true
gh api -X PUT repos/olihey/usdcc/branches/master/protection --input protection.json
```

with `protection.json`:

```json
{
  "required_status_checks": { "strict": false, "contexts": ["windows", "linux"] },
  "enforce_admins": false,
  "required_pull_request_reviews": null,
  "restrictions": null
}
```

**[CHANGELOG.md](../CHANGELOG.md) gets a section per release, written when the release is
made.** Not a line per pull request under an "Unreleased" heading: several sessions merge
into `master` at once, and all of them editing the same lines would conflict each time.
The section is written from `git log <last tag>..master` and the descriptions of those
pull requests, in the words of someone using the app, under the same headings as the
labels. A plain `## <version> - <date>` heading starts it, newest first. It goes in by a
pull request like any other change (label `documentation`), before the tag, so the
release's own commit has it.

**The release's notes are that section, then GitHub's generated list.** From a shell:

```
python tools/release_notes.py 26.11.1 > notes.md
gh release create 26.11.1 --target master --title 26.11.1 --notes-file notes.md --generate-notes
```

`tools/release_notes.py` prints the version's section without its heading, turns links to
files in the repository into links to them at the tag, and fails when the changelog has no
such section, so a release isn't published with empty notes. Under the section it lists
the open known issues (§9); it needs `gh` for that, signed in, and fails without it
unless given `--no-known-issues`. `--generate-notes` adds, under
it, the pull requests merged since the last release, grouped by label, and a link to the
full comparison. Publishing the release starts `release.yml` (§4), as it does from the
website.

The releases up to `2026.11.20` (since deleted, [UPDATES.md](UPDATES.md) §5) were made
before this: their commits went straight to
`master`, so their generated notes are only the comparison link, and their sections in
the changelog were written afterwards from the commits.

## 9. Known issues

A known issue is a problem someone using usdcc should know about until it's fixed. Each
is **one GitHub issue with the `known-issue` label**, and that issue is the record: what
you see, who it affects, what's known of the cause, a workaround if there is one, and how
far the fix is. Nothing else lists them by hand, so no list goes stale.

- **The title** is a line of every release's notes, so it says what a user sees, in
  their words, with the area first: "Arnold: an orthographic view is stretched unless
  the viewport is square".
- **`upstream` as well** when the cause is in another project (a USD plugin, OpenUSD,
  Qt). The issue links theirs, or says it still has to be reported.
- **`bug` as well**, as for any bug.
- **The docs link to it** where they describe the problem (a plan's "not right yet",
  a phase's "not done"), so a reader of the plan finds whether it's fixed, and the issue
  links back to the doc for the details it doesn't repeat.
- **Fixed means closed.** The fix's pull request says "Fixes #<number>", which closes the
  issue when it's merged, and the fix is in the next release's notes under "Fixed" like
  any other.

**Every release's notes end with the ones still open**: `tools/release_notes.py` (§8)
puts a "Known issues" list under the changelog's section, a line per open `known-issue`
issue with its link, oldest first, and leaves the heading out when there are none. A
release's page therefore shows what was known when it was made. The open ones now:
[issues labelled known-issue](https://github.com/olihey/usdcc/issues?q=is%3Aissue+is%3Aopen+label%3Aknown-issue).
