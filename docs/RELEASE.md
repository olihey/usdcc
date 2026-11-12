# Releases

Hosted release builds explicitly omit the optional native RTX target. To make an
RTX-enabled package locally, build with the pinned native SDKs and use the
deployment/clean-package checks in [RTX_RENDER.md](RTX_RENDER.md). Runtime DLLs
are external by default; `--bundle-rtx-runtime` is explicit and includes the SDK's
licenses. The retired Python RTX plugin is never copied into a release.

How a usdcc release is made: you create a release on GitHub, and CI builds the Windows zip
and attaches it. The app shows its version in a new **Help > About usdcc** dialog. Windows
only for now, like the rest of CI ([CI.md](CI.md)). This is part of M11 (packaging), and
the dialog is the first step of M10's Info dialog ([PLAN.md](PLAN.md)).

**Status:** built (§5).

**To make a release:** on GitHub, Releases > Draft a new release, a new tag that is the
version (`0.2.0` or `v0.2.0`, or `0.2.0-beta.1` with "Set as a pre-release") on `master`, the notes,
then Publish. About 25 minutes later the release has `usdcc-<version>-windows-x64.zip` and
its symbols. If the run fails, fix the cause and run the Release workflow by hand with the
same tag (Actions > Release > Run workflow).

## 1. Decisions

- **The release's tag is the version.** You create a release with the tag `0.2.0` (or
  `0.2.0-beta.1` for a prerelease; a leading `v`, as in `v0.2.0`, works too), and that build
  is version `0.2.0`. Nothing to bump by
  hand before releasing; the hand-kept `0.1.0` in `CMakeLists.txt` goes.
- **Every other build is a development build**, named after the last release before it:
  `0.2.0+5.3b70ef2` is 5 commits after `v0.2.0`, at commit `3b70ef2`. The About dialog
  says "Development build" for these. With no release tag yet, it's `0.0.0+<commit>`.
- **CI builds the zip from the tag, not from `master`:** exactly the commit you released.
- **The zip is `tools/deploy.py`'s folder**, as it is today, so a release is what you
  already run locally.
- **Tests gate the release.** If they fail, nothing is attached, and the run says why.
- **Symbols ship separately.** The build is `RelWithDebInfo`, like local builds; the PDBs go
  in a second zip on the same release, so a crash dump from a user can be read.

## 2. The version in the build

**Where it comes from.** `cmake/modules/UsdccVersion.cmake` works it out:

1. `-DUSDCC_VERSION=0.2.0`, which the release workflow passes from the tag.
2. Otherwise `git describe --tags --match "v[0-9]*"`: the last release tag, the commits
   since it, the commit, and whether files are changed (`+dirty`).
3. Otherwise (no git, no tags) `0.0.0`.

**Two parts, updated at different times:**

- **The release number** (`0.2.0`), worked out at configure time. It's `project(VERSION)`,
  and it's what C++ plugins are checked against (`built_for.usdcc`,
  [PLUGIN_SDK.md](PLUGIN_SDK.md) §3). So a plugin built for 0.2.0 loads in 0.2.0 and in
  development builds after it, but not in 0.3.0, without anyone having to remember to
  raise a version.
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
- the log's first line at startup ("usdcc 0.2.0+5.3b70ef2 (development build) starting"),
  so every log file says which build wrote it;
- Python, as `usdcc.app.version()` and `usdcc.app.is_release_build()`, for scripts and
  plugins;
- the `.exe`'s version resource (`resources/usdcc.rc`), shown in Explorer's
  Properties > Details: the release number, and "(development build)", marked prerelease,
  for a development build.

## 3. The About dialog

**Help > About usdcc**, in a new Help menu at the end of the menu bar.

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

**Trigger:** `release: published`, when you publish a release (or prerelease) on GitHub.
Drafts don't trigger it, so you can prepare one first. It also runs by hand
(`workflow_dispatch`) with a tag, to rebuild a release whose run failed.

1. **Check the tag.** It has to be a version (`0.2.0`, `v0.2.0`, `0.2.0-beta.1`). The
   first version of this workflow only took `v0.2.0`, and turned down the first release,
   `0.0.1`.
   Otherwise the run fails before building anything.
2. **Check out the tag**, with submodules.
3. **The same steps as `build.yml`:** OpenUSD and PySide6 from their release assets, Qt
   from the vcpkg cache. PySide6 is required here, not optional: a release without
   scripting panels would be broken.
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
8. **Zip:**
   - `usdcc-<version>-windows-x64.zip`: the deploy folder, `usdcc-<version>/`.
   - `usdcc-<version>-windows-x64-symbols.zip`: the PDBs of usdcc's own programs and
     libraries (the exe, its DLLs, the `usdcc` Python modules, the example C++ plugin),
     laid out as they are in the deploy.
9. **Attach** both to the release with `gh release upload`.

It uses `build.yml`'s vcpkg cache, read-only.

**What you see:** the release appears as soon as you publish it, and the zips appear about
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

**usdcc is BSD-3-Clause** (`LICENSE`), which every library the zip ships allows. None of
them requires usdcc to be under the GPL:

- **Permissive:** OpenUSD (TOST, based on Apache 2.0), TBB, Embree, MaterialX, OpenSSL,
  QWindowKit (Apache 2.0); QtNodes, OpenColorIO, pybind11, zstd, libjpeg-turbo (BSD); and
  MIT, zlib, ICU, PostgreSQL, Python (PSF) and public-domain licences for the rest.
- **LGPL:** Qt and PySide6 (LGPL v3), Qt Advanced Docking System (LGPL v2.1). Allowed
  because they stay separate DLLs the user can replace, as they already are.
- **Dual-licensed, used under the permissive option:** freetype (FTL), zstd (BSD), Qt (LGPL).
  dbus (AFL or GPL) is installed by vcpkg but not shipped.

What the release has to do, whatever usdcc's own licence:

- **Ship the licence texts.** `tools/deploy.py` gathers them into `licenses/`, and usdcc's
  own `LICENSE` goes at the top as `LICENSE.txt`:
  - `vcpkg/<port>.txt`: each vcpkg package's `share/<port>/copyright` (not vcpkg's own
    build helpers).
  - `OpenUSD/`: OpenUSD's `LICENSE` and `THIRD-PARTY.md`, from its install.
  - `OpenUSD-dependencies/<library>/`: the licence and NOTICE files of the libraries
    `build_usd.py` builds, which its install leaves out. `tools/ci/usd_licenses.py`
    collects them from its source and build folders (top-level licence files of each
    source, and the libraries OpenColorIO and OpenEXR build in themselves).
    `deps-usd.yml` runs it before deleting those folders, into the asset's `licenses/`;
    `deploy.py` runs it for a local install that still has them.
  - `PySide6/`: pyside-setup's `LICENSES`, which `tools/build_pyside.py` copies into its
    install.
  - `Python/`, `QWindowKit/`, `ruff/`, `uv/`.
- **Make the LGPL libraries' sources available.** `licenses/SOURCES.txt` names, for every
  vcpkg package, its version, the vcpkg port it was built with and its upstream source
  (from the SPDX file vcpkg writes), and PySide6's tag.
- **Keep the LGPL libraries replaceable DLLs.** Never link them statically.

The About dialog's licence tab (M10's Info dialog) can later list the same `licenses/`
folder.

## 7. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should `build.yml` keep the deploy zip of every `master` build as a workflow artifact too (CI.md §7, "Deploy artifact")? | Yes, once phase 2 works: the same steps 6–8, kept for a few days, for testing a build before releasing it |
| 2 | The repository's Releases page also lists the `deps/…` prereleases (OpenUSD, PySide6) | Fine for now: they're prereleases, so they never become "Latest". Move them to a separate repository if they crowd the page |
| 3 | An installer (MSI or similar) rather than a zip? | Not yet. The deploy runs from any folder, and a zip is all that needs |
