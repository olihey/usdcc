# Installer and updates

usdcc on Windows comes with an installer, and an installed usdcc updates itself from the
GitHub releases: **Help > Check for Updates**, or on its own a few seconds after it starts.
Both are made with [Velopack](https://velopack.io) (MIT). The 7z stays, for anyone who
would rather unpack a folder. Part of M11 (packaging), after [RELEASE.md](RELEASE.md).

**Status:** built, and tried locally from install to update (§6). The release workflow's
part (§5) runs for the first time with the next release.

## 1. Why

A 7z downloaded with a browser carries Windows' download mark (the "mark of the web"),
and Explorer copies it onto every file it unpacks. None of usdcc's files are signed, so
Microsoft Defender sends each DLL to its cloud scan the first time it loads ("Security
scan required ... could take up to 10 seconds"). A release has about 340 of them, so the
first start took minutes. An installer is scanned once; the files it installs, and the
updates usdcc downloads itself, carry no mark.

## 2. Decisions

- **Velopack**, not an MSI or Inno Setup: it installs per user (no administrator rights),
  keeps one folder per app, and its library checks, downloads and applies updates, with
  delta packages so an update downloads only what changed.
- **Windows only.** Linux keeps its `tar.xz`; Velopack's Linux packages are AppImages,
  which the Linux deploy isn't. macOS has a disk image and no updates either.
- **The GitHub releases are the update server.** The release workflow attaches the
  installer, the packages and the feed Velopack reads (§5); nothing else to host.
- **Package id `usdcc`, the user's files in `usdcc_config`.** Velopack installs into
  `%LOCALAPPDATA%\<id>`, so into `%LOCALAPPDATA%\usdcc`, and owns that folder: it empties
  it when usdcc is uninstalled, and Setup moves aside and deletes whatever is there before
  it installs. So on Windows usdcc keeps the user's settings, logs, sessions, plugins and
  the rest beside it, in `%LOCALAPPDATA%\usdcc_config` (`usdcc::ui::userDataFolder()`,
  `ui/settings.h`), which an uninstall leaves. Earlier builds kept them in
  `%LOCALAPPDATA%\usdcc`; they aren't moved, and the installer deletes them. Linux is
  unchanged (`~/.config/usdcc` and `~/.local/share/usdcc`).
- **Not signed yet.** SmartScreen warns once about the unknown publisher of the installer
  (More info > Run anyway). Velopack can sign everything it packs (`vpk pack
  --signParams` or `--azureTrustedSignFile`) once there is a certificate (§8).
- **The installer's file is Velopack's name, `usdcc-win-Setup.exe`.** `vpk upload`
  attaches the files by the names in its own list, so they aren't renamed.

## 3. The launcher

Velopack starts the program at the top of the package, and runs it with its own arguments
for its install and update hooks. usdcc's program is `bin\usdcc.exe`, beside its DLLs. So a
small `usdcc.exe` (`src/cpp/launcher`) goes at the top of the deploy:

1. `VelopackApp::Build().Run()`, first: with a hook's argument it runs the hook and exits;
   with an update downloaded but not yet installed, it installs it and starts again.
2. Starts `bin\usdcc.exe` with the same command line, and exits. When Velopack restarted it
   after installing an update, it sets `USDCC_UPDATED=1` for the app (§4).

In the 7z, which isn't installed, Velopack finds no install and does nothing, so it is just
a launcher there, like `usdcc.cmd`.

It links Velopack's static library, which is built with the static C runtime, so the
launcher is built with it too (`MSVC_RUNTIME_LIBRARY MultiThreaded`) and has no DLLs of its
own. Its icon and version are the app's; its PDB is `usdcc_launcher.pdb`, so it doesn't take
the place of the app's `usdcc.pdb` in the symbols.

**Velopack in the build:** `cmake/modules/UsdccVelopack.cmake` downloads the pinned
`velopack_libc_<version>.zip` from Velopack's release and its `LICENSE` at that tag, each
checked against its SHA-256, into `build/velopack`. Without them (offline, or
`-DUSDCC_VELOPACK=OFF`) there is no launcher and the app can't update itself (§4).

## 4. In the app

`usdcc::app::AppUpdates` (`src/cpp/app/app_updates.*`). The app links Velopack's DLL
(`velopack_libc.dll`, beside `usdcc.exe`), which keeps its own C runtime to
itself; the app is on the runtime DLL, as all of usdcc is.

**Installed or not.** usdcc can update itself when it runs from an install: `bin\usdcc.exe`
under a `current` folder with Velopack's `Update.exe` beside that folder. This is how
Velopack's own locator finds it; usdcc checks it itself, so a copy that isn't installed
doesn't log Velopack's errors.

**Help > Check for Updates...**
- In an install: checks in the background (the status bar says so), then shows one of
  - "usdcc 26.12.1 is available": what you have, the download's size (the delta's when there
    is one), and **Download and Install** and **Release Notes**. It stays until closed.
  - "usdcc is up to date".
  - "Couldn't check for updates", with Velopack's error.
- Anywhere else: "Updates come with the installer", with **Releases Page**.

Once a check (this one or the one at the start) found an update, the menu item follows it, so
it can be installed after its notification was closed:
- **Install Update to 26.12.1**: the same as Download and Install.
- **Downloading Update to 26.12.1...** (disabled) while it downloads. If that fails, it's
  Install Update again.
- **Restart to Update to 26.12.1** once downloaded: the same as Restart Now.

**At the start.** Ten seconds after the window shows, the same check, if **Settings >
Updates > Check for updates when usdcc starts** is on (the default). It only says something
when there is an update; a failure goes to the log.

**Download and Install** downloads in the background, with the progress in the status bar.
Then "usdcc 26.12.1 is ready" with **Restart Now**. Without a restart, the launcher installs
it the next time usdcc starts (§3).

**Restart Now** closes the window as File > Exit does, so it asks about unsaved changes;
cancelled, nothing happens. Once the app has quit, Velopack's updater waits for the process
to end, installs the update with a progress window, and starts usdcc again, which shows
"Updated to usdcc 26.12.1" with **Release Notes**.

A build without the Updates feature (`USDCC_BUILD_UPDATES=OFF`,
[BUILD_OPTIONS.md](BUILD_OPTIONS.md) §11) has none of this section: no menu item, no check,
no settings.

**Settings > Updates** (only in builds with Velopack):

| Setting | Default | |
|---|---|---|
| Check for updates when usdcc starts | on | |
| Include prereleases | off | Also offer betas (`26.12.1-beta.1`) |

usdcc logs each step under `usdcc.updates`, and what Velopack's library reports in the app
goes there too. Velopack's updater (`Update.exe`, which also rebuilds the full package
from a delta) writes its own log, `%LOCALAPPDATA%\velopack\velopack_usdcc.log`.

## 5. Release workflow

After the 7z is attached, `release.yml`'s Windows job:

1. Installs `vpk`, the version `UsdccVelopack.cmake` pins, with the runner's .NET SDK.
2. `vpk download github`: the latest earlier release's full package (a prerelease's when
   this one is a prerelease), to make a delta from. The first release with packages has
   none, which is fine.
3. `vpk pack --packId usdcc --mainExe usdcc.exe --runtime win-x64 --noPortable` on the deploy folder.
4. `vpk upload github --tag <tag> --merge`, which attaches to the release:
   `usdcc-win-Setup.exe`, `usdcc-<version>-full.nupkg`, the delta when there is
   one, and `releases.win.json`, the feed an install reads.

The `windows-arm64` job does the same with `--runtime win-arm64` and the channel
`win-arm64` on all three commands: `usdcc-win-arm64-Setup.exe`, and a feed of its own,
`releases.win-arm64.json`. An install reads the feed of the channel it was packed for, so
an x64 install is never offered the arm64 package. x64 stays on Velopack's default
channel, `win`, which installs made before arm64 read.

An install asks GitHub for the releases (prereleases only with the setting) and reads the
`releases.win.json` of each, so a delta chain can span several releases. The `deps/...`
prereleases have no feed and are skipped.

### Version numbers

A release's version is `YY.MM.N`: the year's last two digits, the month, and a counter
that starts at 1 each month and goes up by one with every release. `26.12.1` is December
2026's first release and `26.12.2` its second, whether that is the same day or two weeks
later. The tag is the version ([RELEASE.md](RELEASE.md) §1), so this is what the release's
tag is.

Velopack orders versions as SemVer 2 does, and an install is only offered a version higher
than its own. That rules out the other ways to number a second release in a day:

- **Three numbers, never four.** `26.12.1.1` isn't a version to Velopack, and
  `release.yml`'s tag check turns it down.
- **A `-suffix` makes a version lower, not higher.** `26.12.1-hotfix.1` is older than
  `26.12.1`, and no install of `26.12.1` would be offered it. The suffix is for betas
  only: `26.12.1-beta.1`, with "Set as a pre-release", comes before `26.12.1`.
- **A `+suffix` isn't compared at all**, and the tag check turns it down too.
- **No leading zeros**, which SemVer doesn't allow: January is `27.1.1`, not `27.01.1`.

**The year in two digits, never four.** `2026.12.1` is higher than every `YY.MM.N`
version there will be, so each install would be offered it, and nothing after it. The tag
check turns it down.

**The releases with the year in full are deleted.** `2026.11.18` to `2026.11.20` were
made before the year was shortened, and to Velopack each of them is higher than `26.11.1`
and everything after it. With one of them on GitHub, an install of `26.11.1` would read
its feed, take `2026.11.20` for the newest release and offer it, and `release.yml`'s
`vpk pack` would turn down a version lower than the package `vpk download` fetched. So
the three were deleted from GitHub, with their tags, and `0.0.1` to `0.0.3` with them;
[CHANGELOG.md](../CHANGELOG.md) still says what was in each.

An install of one of them finds no update, now or later. Running `usdcc-win-Setup.exe`
from a `YY.MM.N` release installs over it, after asking; the user's files, in
`%LOCALAPPDATA%\usdcc_config`, stay.

**The next number.** The latest release's tag:

```powershell
git fetch --tags
git tag --list "[0-9][0-9].[0-9]*.[0-9]*" --sort=-v:refname | Select-Object -First 1
```

Raise its last number by one; in a new month, start at 1. With no tag yet, the first is
`26.11.1`.

## 6. Trying it locally

With `vpk` (it needs .NET 8 or later; `dotnet tool install -g vpk --version <pinned>`):

```powershell
python tools/deploy.py C:\tmp\usdcc-deploy
vpk pack --packId usdcc --packVersion 0.0.90 --packDir C:\tmp\usdcc-deploy --mainExe usdcc.exe --runtime win-x64 --noPortable --shortcuts None -o C:\tmp\feed
C:\tmp\feed\usdcc-win-Setup.exe --silent --installto C:\tmp\usdcc-install
vpk pack --packId usdcc --packVersion 0.0.91 --packDir C:\tmp\usdcc-deploy --mainExe usdcc.exe --runtime win-x64 --noPortable --shortcuts None -o C:\tmp\feed
```

`--shortcuts None` keeps the test install off the desktop and Start menu; `--installto`
keeps it out of `%LOCALAPPDATA%`. The second `vpk pack` finds 0.0.90 in the folder and
makes a delta from it. Setup 1.2.161 crashes when given arguments for the app (`-- ...`),
so give none.

Then start `C:\tmp\usdcc-install\usdcc.exe` with `USDCC_UPDATE_SOURCE=C:\tmp\feed`, which
makes usdcc read that folder (or a URL of one) instead of GitHub; Help > Check for Updates
offers 0.0.91. Uninstall with `C:\tmp\usdcc-install\Update.exe --silent uninstall`.

**Tried this way** (with `USDCC_TEST_PROFILE` and the offscreen platform, a script pressing
the buttons): Setup installed 0.0.90 in 4 s, through the launcher's install hook; Velopack's
stub, `Update.exe`, the launcher and `bin\usdcc.exe` started in turn with the arguments
passed on. The check found 0.0.91 with its 0.8 MB delta (the full package is 225 MB), the
download rebuilt the full package from it in 18 s, Restart Now closed the app (saving the
Last session), Velopack installed 0.0.91 and started usdcc, which logged "Updated to", and
its startup check found nothing newer. The uninstall left no folder and no entry in the
Apps list.

## 7. Licences

`licenses/Velopack/LICENSE` (MIT), and a line in `licenses/SOURCES.txt` with Velopack's
version and source. Velopack's library and `Update.exe` are Rust; the crates built into
them are under MIT, Apache-2.0, BSD, ISC, Zlib, Unicode and MPL-2.0 licences (what its
`deny.toml` allows), pinned by its `Cargo.lock` at the tag that `SOURCES.txt` names.

## 8. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Sign the installer and the binaries? | Yes, when there's a certificate: Azure Trusted Signing (about $10 a month) or SignPath (free for open source), through `vpk pack`'s signing options. Signed files also skip Defender's cloud scan of the 7z |
| 2 | Ship the Rust crates' own licence texts too, not just Velopack's? | Yes, generated from Velopack's `Cargo.lock` at the pinned tag (`cargo about`), into `licenses/Velopack/` |
| 3 | Keep the 7z? | Yes, for now: some people won't run an installer. Its first start stays slow until it's signed, unless unblocked (Properties > Unblock) before unpacking |
| 4 | Updates on Linux? | Later, if the Linux deploy becomes an AppImage |
