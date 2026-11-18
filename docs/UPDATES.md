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
  which the Linux deploy isn't.
- **The GitHub releases are the update server.** The release workflow attaches the
  installer, the packages and the feed Velopack reads (§5); nothing else to host.
- **Package id `usdcc-app`.** Velopack installs into `%LOCALAPPDATA%\<id>` and deletes that
  folder when usdcc is uninstalled. `%LOCALAPPDATA%\usdcc` holds the user's settings, logs
  and sessions, so the install can't be there. The name in Windows' list of apps and on the
  shortcuts is still "usdcc".
- **Not signed yet.** SmartScreen warns once about the unknown publisher of the installer
  (More info > Run anyway). Velopack can sign everything it packs (`vpk pack
  --signParams` or `--azureTrustedSignFile`) once there is a certificate (§8).
- **The installer's file is Velopack's name, `usdcc-app-win-Setup.exe`.** `vpk upload`
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
  - "usdcc 0.2.0 is available": what you have, the download's size (the delta's when there
    is one), and **Download and Install** and **Release Notes**. It stays until closed.
  - "usdcc is up to date".
  - "Couldn't check for updates", with Velopack's error.
- Anywhere else: "Updates come with the installer", with **Releases Page**.

**At the start.** Ten seconds after the window shows, the same check, if **Settings >
Updates > Check for updates when usdcc starts** is on (the default). It only says something
when there is an update; a failure goes to the log.

**Download and Install** downloads in the background, with the progress in the status bar.
Then "usdcc 0.2.0 is ready" with **Restart Now**. Without a restart, the launcher installs
it the next time usdcc starts (§3).

**Restart Now** closes the window as File > Exit does, so it asks about unsaved changes;
cancelled, nothing happens. Once the app has quit, Velopack's updater waits for the process
to end, installs the update with a progress window, and starts usdcc again, which shows
"Updated to usdcc 0.2.0" with **Release Notes**.

**Settings > Updates** (only in builds with Velopack):

| Setting | Default | |
|---|---|---|
| Check for updates when usdcc starts | on | |
| Include prereleases | off | Also offer betas (`0.2.0-beta.1`) |

usdcc logs each step under `usdcc.updates`, and what Velopack's library reports in the app
goes there too. Velopack's updater (`Update.exe`, which also rebuilds the full package
from a delta) writes its own log, `%LOCALAPPDATA%\velopack\velopack_usdcc-app.log`.

## 5. Release workflow

After the 7z is attached, `release.yml`'s Windows job:

1. Installs `vpk`, the version `UsdccVelopack.cmake` pins, with the runner's .NET SDK.
2. `vpk download github`: the latest earlier release's full package (a prerelease's when
   this one is a prerelease), to make a delta from. The first release with packages has
   none, which is fine.
3. `vpk pack --packId usdcc-app --mainExe usdcc.exe --runtime win-x64 --noPortable` on the deploy folder.
4. `vpk upload github --tag <tag> --merge`, which attaches to the release:
   `usdcc-app-win-Setup.exe`, `usdcc-app-<version>-full.nupkg`, the delta when there is
   one, and `releases.win.json`, the feed an install reads.

An install asks GitHub for the releases (prereleases only with the setting) and reads the
`releases.win.json` of each, so a delta chain can span several releases. The `deps/...`
prereleases have no feed and are skipped.

## 6. Trying it locally

With `vpk` (it needs .NET 8 or later; `dotnet tool install -g vpk --version <pinned>`):

```powershell
python tools/deploy.py C:\tmp\usdcc-deploy
vpk pack --packId usdcc-app --packVersion 0.0.90 --packDir C:\tmp\usdcc-deploy --mainExe usdcc.exe --runtime win-x64 --noPortable --shortcuts None -o C:\tmp\feed
C:\tmp\feed\usdcc-app-win-Setup.exe --silent --installto C:\tmp\usdcc-install
vpk pack --packId usdcc-app --packVersion 0.0.91 --packDir C:\tmp\usdcc-deploy --mainExe usdcc.exe --runtime win-x64 --noPortable --shortcuts None -o C:\tmp\feed
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
