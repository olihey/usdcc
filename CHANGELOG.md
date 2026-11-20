# Changelog

What changed in each usdcc release, newest first. Each release's section is also the
top of its notes on [GitHub](https://github.com/olihey/usdcc/releases), above the list
of pull requests GitHub generates. A section is written when the release is made, from
the pull requests merged since the one before
([docs/RELEASE.md](docs/RELEASE.md) §8), so changes on `master` that no release has yet
aren't listed here.

## 2026.11.20 - 2026-11-20

### New

- **Schemas panel.** Lists every schema USD has registered and what each defines, shows
  which ones the stage uses, and has Select, Create and Apply for them.
- **Schema Editor** and **Promote to Schema** in the Schemas panel. Codeless schemas
  can be read, written, checked and generated as a library.
- The Inspect workspace has the Schemas panel to the right of the viewport.
- USD plugin packs: Arnold, off until Settings names an Arnold folder.
- The asset cache names its files by hashes, not by the server's path.

### Build, CI and packaging

- QWindowKit comes from upstream again: stdware merged the maximize fix.

### Plans and documentation

- Remote assets: the plan for an S3 resolver on the shared cache, with libcurl's SigV4,
  and a spike that sends signed requests through the shared HTTP client.

## 2026.11.19 - 2026-11-19

### Build, CI and packaging

- On Windows, usdcc installs into `%LOCALAPPDATA%\usdcc`, and the user's files are in
  `%LOCALAPPDATA%\usdcc_config`.

## 2026.11.18 - 2026-11-18

### New

- **An installer and updates on Windows**, with Velopack.
- USD plugin packs: the Gatling render delegate, on Windows and Linux.
- **Follow Current Stage:** an entry in every view panel's stage dropdown, and a
  setting per panel type.
- Viewport broadcast: a Broadcast submenu, a red border while a viewport is live, and a
  warning before closing it.
- Settings > Assets has Clear Cache, and the cache size the user sets wins.

### Fixed

- Maximizing the window: the fix is in QWindowKit (our fork for now), and the app's
  own filter for it is gone.
- Folder lists in Settings stand out from the page.
- The Peers panel test no longer reads a freed pointer.

### Plans and documentation

- Remote assets: more storage backends through OpenDAL, for later.

## 0.0.3 - 2026-11-18

### Fixed

- The first watcher of a broadcast whose viewport isn't changing gets the sharp picture.

## 0.0.2 - 2026-11-18

### New

- **Peers.** Running instances host rooms and join them, with channels, limits and
  guests, keys and encryption, reconnecting, shared state and room discovery. The Peers
  panel has chat and public rooms, and the protocol is written down
  ([docs/PEERS_PROTOCOL.md](docs/PEERS_PROTOCOL.md)).
- **Viewport broadcast:** Broadcast in a viewport's menu, watched live in a browser.
- An emoji button beside the chat message, with the last ones used first.

### Fixed

- The asset picker, and the contents of a `.usdz` on a server, resolve again.
- A floating side panel docks at the edge beside another screen.
- Emoji cells fit their emoji under the app's theme.
- A toast is as wide as its actions need, and wraps them past its limit.
- Qt's TLS plugin is beside `usdcc.exe`, so Qt Network finds a TLS backend.
- Test programs started by hand find Qt's platform plugin, and the asset picker's tests
  no longer leave a UI scale behind.

### Build, CI and packaging

- The peers library builds with GCC again.

### Plans and documentation

- The asset picker's favourites (folders only, the user's own), its scheme dropdown,
  and an Asset Servers editor.

## 0.0.1 - 2026-11-17

The first release: 320 commits, from the empty app shell to a build that CI tests,
packs and attaches for Windows and Linux.

- **Stages and viewports.** Any number of OpenUSD stages open at once, each shown in
  Hydra viewports with Select, Move, Rotate and Scale tools, a ground grid, a HUD,
  usdview's display options, OCIO view transforms, and undo as layer changes.
- **Panels.** Outliner, Attribute Editor (transform stack, relationships, applied
  schemas, composition arcs), Layer Editor, Variant Editor, Dependencies, USDA view,
  Log, and a timeline with per-stage time and playback.
- **Material Editor.** Shading networks as node graphs, a parameters pane, and a
  preview with its own shapes and HDRIs.
- **Rendering.** Storm and other Hydra render delegates with their own settings, and a
  V-Ray Render panel on Windows. The RTX renderer is in local builds only, not in the
  release.
- **Physics.** Rigid bodies simulated with Jolt from the timeline, joints, filtering,
  and baking a simulation.
- **Remote assets.** Stages and assets over HTTP and WebDAV through a shared caching
  library, with environments and an asset picker.
- **Scripting and plugins.** One embedded Python with a Script Editor, startup scripts
  and PySide6 panels; Python, USD and C++ plugins with a Plugins panel, their own Python
  packages, and one-click reload.
- **The app.** A custom title bar, a dark theme, UI scaling, workspaces, sessions,
  settings, toasts, and Help > About usdcc with the version.
- **Build and releases.** Windows and Linux builds in CI against a prebuilt OpenUSD and
  PySide6, releases built from a tag with their symbols, and every third-party licence
  in the archive.
