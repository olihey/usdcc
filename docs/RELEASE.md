# Releases

How a usdcc release is made: you create a release on GitHub, and CI builds the Windows zip
and attaches it. The app shows its version in a new **Help > About usdcc** dialog. Windows
only for now, like the rest of CI ([CI.md](CI.md)). This is part of M11 (packaging), and
the dialog is the first step of M10's Info dialog ([PLAN.md](PLAN.md)).

**Status: plan.** Nothing below is built yet.

## 1. Decisions

- **The release's tag is the version.** You create a release with the tag `v0.2.0` (or
  `v0.2.0-beta.1` for a prerelease), and that build is version `0.2.0`. Nothing to bump by
  hand before releasing; the hand-kept `0.1.0` in `CMakeLists.txt` goes.
- **Every other build is a development build**, named after the last release before it:
  `0.2.0+5.3b70ef2` is 5 commits after `v0.2.0`, at commit `3b70ef2`. The About dialog
  says "Development build" for these. With no release tag yet, it's `0.0.0+<commit>`.
- **CI builds the zip from the tag, not from `master`:** exactly the commit you released.
- **The zip is `tools/deploy.ps1`'s folder**, as it is today, so a release is what you
  already run locally.
- **Tests gate the release.** If they fail, nothing is attached, and the run says why.
- **Symbols ship separately.** The build is `RelWithDebInfo`, like local builds; the PDBs go
  in a second zip on the same release, so a crash dump from a user can be read.

## 2. The version in the build

**Where it comes from.** `cmake/UsdccVersion.cmake` works it out:

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
  build time into a small generated source in `usdcc_core`. It's rewritten only when it
  changes, so a new commit recompiles one file, and the About dialog never shows a stale
  commit.

**What reads it:** `usdcc/core/version.h` (`usdcc::core::version()`, `isRelease()`,
`commit()`, `buildDate()`), used by:

- the About dialog (§3);
- the log's first line at startup ("usdcc 0.2.0 (3b70ef2) starting"), so every log file
  says which build wrote it;
- Python, as `usdcc.app.version()`, for scripts and plugins;
- the `.exe`'s version resource (`resources/usdcc.rc`), shown in Explorer's
  Properties > Details.

## 3. The About dialog

**Help > About usdcc**, in a new Help menu at the end of the menu bar.

- The app icon, "usdcc", the version in large type, and "Development build" under it when
  it isn't a release.
- The commit, linked to it on GitHub, and the build date.
- The components it runs with: OpenUSD, Qt, Python, PySide6 (when present), and the
  compiler. Read at run time where possible (`qVersion()`, Python's `sys.version`), so the
  dialog shows what is actually loaded.
- Links to the repository and its releases.
- **Copy**, which copies all of it as plain text for a bug report.

`usdcc::ui::AboutDialog` lives in `usdcc_ui` and is given its rows. The app fills them in,
because `usdcc_ui` doesn't know about OpenUSD or Python. A test in `usdcc_ui_tests` checks
the rows and the copied text.

M10's Info dialog (third-party licences, registered USD extensions) can grow out of it
later as a second tab.

## 4. Workflow `release.yml`

**Trigger:** `release: published`, when you publish a release (or prerelease) on GitHub.
Drafts don't trigger it, so you can prepare one first. It also runs by hand
(`workflow_dispatch`) with a tag, to rebuild a release whose run failed.

1. **Check the tag.** It has to be `v` plus a version (`v0.2.0`, `v0.2.0-beta.1`).
   Otherwise the run fails before building anything.
2. **Check out the tag**, with submodules.
3. **The same steps as `build.yml`:** OpenUSD and PySide6 from their release assets, Qt
   from the vcpkg cache. PySide6 is required here, not optional: a release without
   scripting panels would be broken.
4. **Build** with `tools/build.ps1 -UsdccVersion <version>` (a new parameter that passes
   `-DUSDCC_VERSION`).
5. **Run the tests.**
6. **Deploy** with `tools/deploy.ps1` into `usdcc-<version>/`.
7. **Smoke test the deploy.** Start `bin\usdcc.exe` from the deploy folder, with a `PATH`
   that has none of the build's folders on it, on Qt's offscreen platform, with
   `--no-plugins --script check.py`. The script writes `usdcc.app.version()` and OpenUSD's
   version to a file and quits the app. The workflow checks the version is the tag's. This
   catches a missing DLL or Python file that the build's own folders were hiding.
8. **Zip:**
   - `usdcc-<version>-windows-x64.zip`: the deploy folder.
   - `usdcc-<version>-windows-x64-symbols.zip`: the PDBs of usdcc's own DLLs and exe.
9. **Attach** both to the release with `gh release upload`.

**What you see:** the release appears as soon as you publish it, and the zips appear about
15 minutes later. If the run fails, the release stays without them. Fix the cause, then run
`release.yml` by hand with the same tag.

## 5. Phases

1. **Version and About dialog.** `cmake/UsdccVersion.cmake`, `usdcc/core/version.h`, the
   startup log line, `usdcc.app.version()`, the `.exe` version resource, the Help menu and
   `AboutDialog` with its test, and `tools/build.ps1 -UsdccVersion`. All local, testable
   without a release. Done when a local build shows `0.0.0+<commit>` as a development
   build, and `tools/build.ps1 -UsdccVersion 0.2.0` shows `0.2.0` as a release.
2. **Release workflow.** `release.yml` as in §4, with the smoke test. Done when a test
   prerelease (`v0.0.1-test.1`) gets both zips, and its zip runs on a machine without the
   dev environment. The test release and its tag are deleted afterwards.
3. **Third-party licences** (§6), before the first real release.

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

- **Ship the licence texts.** `tools/deploy.ps1` gathers them into `licenses/`, one file per
  component: vcpkg's `share/<port>/copyright`, OpenUSD's and its dependencies', Python's,
  PySide6's, ruff's and uv's, QWindowKit's, plus the NOTICE files the Apache 2.0 licences
  ask for. usdcc's own `LICENSE` goes at the top of the zip.
- **Make the LGPL libraries' sources available.** The release notes link to the exact
  sources: Qt's version with the vcpkg commit whose port patches it, and PySide's tag.
- **Keep the LGPL libraries replaceable DLLs.** Never link them statically.

The About dialog's licence tab (M10's Info dialog) can later list the same `licenses/`
folder.

## 7. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should `build.yml` keep the deploy zip of every `master` build as a workflow artifact too (CI.md §7, "Deploy artifact")? | Yes, once phase 2 works: the same steps 6–8, kept for a few days, for testing a build before releasing it |
| 2 | The repository's Releases page also lists the `deps/…` prereleases (OpenUSD, PySide6) | Fine for now: they're prereleases, so they never become "Latest". Move them to a separate repository if they crowd the page |
| 3 | An installer (MSI or similar) rather than a zip? | Not yet. The deploy runs from any folder, and a zip is all that needs |
