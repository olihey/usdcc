# Plugin Registry (plan, parked)

Plugins that don't ship with usdcc: a *registry* lists them, and View > Plugins installs and
updates them from it. usdcc has one of its own, and a studio can host another. Builds on
[PLUGINS.md](PLUGINS.md) (the plugin folder and its manifest), [USD_PLUGINS.md](USD_PLUGINS.md)
(the packs CI builds) and [UPDATES.md](UPDATES.md) (how the app itself updates). It replaces
PLUGINS.md §6 decision 6, "No zip installer or store for now".

**Status: parked** (decided 2026-11-26). Nothing is built, and nothing here is to be started
until C++ plugins have a stable interface, so that one build of a plugin loads into many usdcc
versions. Today it loads into exactly one (§4), and a registry on top of that would mean an
archive of every C++ plugin for every platform and every release, and plugins that stop loading
at each update.

- **What waits on what:** a stable Plugin SDK first ([PLUGIN_SDK.md](PLUGIN_SDK.md); it isn't
  planned yet, and §4 says why it is a project of its own). Then this plan, read again.
- **What a stable SDK takes away from it:** an archive per usdcc version, **Needs update**, the
  download before an app update, and `release.yml` packing every plugin at every release (§4,
  §7, phase 4). `built_for` would name an SDK version, or a range of them, in place of the usdcc
  version. They are left in as written, as the record of why it is parked.
- **The optional packs are no longer part of this.** A pack, of plugins of any kind, gets a
  download of its own in the app ([USD_PLUGINS.md](USD_PLUGINS.md) §9), without a registry. The
  first are the renderers (MoonRay, Cycles), which is why this plan calls them the renderer
  packs; where it names them (§2, §7, phase 5), that is what was considered.
- **Still decided, for when it comes back:** the official registry holds only what usdcc builds
  (§1), and where it is hosted is left for later (§11 item 1).

"Checked" means read in this repository's code or measured in a local build; everything else is
a proposal or an assumption. §11 has the questions still open.

## 1. Decisions

- **A registry is a static file,** `index.json`, with the plugins' archives beside it or anywhere
  it names. A web server, GitHub Pages or a network share is enough; nothing usdcc-specific runs
  there (§3). Blender's extension repositories and [ASSET_SOURCES.md](ASSET_SOURCES.md) §7 work
  the same way.
- **The index carries what the app shows:** name, description, author, licence, homepage, icon,
  tags, versions and sizes. It is generated from each plugin's `usdcc-plugin.json`, never written
  by hand, so the two can't disagree (§3.3).
- **An archive is a plugin folder, zipped.** Installed, it is an ordinary plugin: the registry
  adds a way to get the folder, not a second kind of plugin.
- **Every archive is pinned by SHA-256 in the index, and the index is signed** (Ed25519, with
  libsodium, which the app already links). HTTPS alone is not what the trust rests on (§5).
- **Trust is given to a registry or to single plugins of it** (§5.2). A plugin updates only from
  the registry it came from.
- **The official registry holds only what usdcc's own CI builds:** usdcc's plugins, and its
  builds of other projects' code, such as the renderer packs. It takes no plugin built by
  someone else. Third parties and studios host registries of their own, which the user adds (§7).
- **A C++ plugin has one archive per platform and usdcc version,** because the app only loads
  one built for exactly itself (§4). Python and USD-schema plugins have one archive for all.
  *This is why the plan is parked:* with a stable SDK it becomes one archive per platform.
- **Nothing is installed without being asked, and nothing uses the network before the window is
  up.** The check for plugin updates runs with the app's own, a few seconds after the start (§6.2).
  This is [PYTHON_PACKAGES.md](PYTHON_PACKAGES.md) §4's rule.
- **An install never changes a folder in place.** A new version goes into a new folder and takes
  effect at the next start (§6.3).
- **Signed is not safe.** A plugin runs with the user's full rights, as today (PLUGINS.md §3.6).
  A signature says who published it; there is still no sandbox.

## 2. What is there now

*Checked:*

- **A plugin is a folder with `usdcc-plugin.json`:** `id`, `name`, `version`, `description`,
  `api`, `python`, `usd`, `requires`, `native` (`PluginManifest`,
  [plugin_manager.h](../src/cpp/scripting/include/usdcc/scripting/plugin_manager.h)). No author,
  licence, homepage or icon.
- **Found in five places, the first with an id winning:** `--plugin-path`, `USDCC_PLUGIN_PATH`,
  the user's `usdcc_config/plugins`, the folders of Settings > Plugins, then the shipped
  `plugins/` (`PluginManager::defaultOptions()`). Versions aren't compared.
- **A C++ plugin loads only into the exact build:** `platform`, `compiler`, `qt`, `usd` and
  `usdcc` of its `built_for` must all equal the app's (`readManifest()` in
  [plugin_manager.cpp](../src/cpp/scripting/src/plugin_manager.cpp)). So every update of usdcc
  makes every installed C++ plugin unloadable. A Python or USD-only plugin needs `api` to match,
  which is still 1.
- **Nothing is unloaded,** so turning a plugin on or off waits for the next start (PLUGINS.md
  §3.4). A plugin made while the app runs can start at once (`PluginManager::addPlugin()`), and a
  Python one can be reloaded (`reload()`).
- **No trust check:** "A plugin found there just loads" (PLUGINS.md §3.6).
- **Python packages are installed on request,** with uv, into a new folder each time, and a
  plugin's extra package indexes are shown before anything comes from them (PYTHON_PACKAGES.md
  §3, §4). That is the model for installing a plugin.
- **The app updates itself** from the GitHub releases, checking ten seconds after the window
  shows (UPDATES.md §4). Copies that can't update ask GitHub's API at most once a day, because
  it allows 60 anonymous requests an hour for an IP address, and a studio shares one (§9 there).
- **CI already builds plugin archives:** the USD plugin packs, one zip per pack and platform,
  named by a hash of what went in, with their licences and a list of their files, attached to a
  release (USD_PLUGINS.md §4). They are unzipped over the OpenUSD install, not installed as
  plugin folders.
- **libsodium is linked** for Peers, with Ed25519 identities ([PEERS.md](PEERS.md) §6).
- **No zip library** is among the dependencies (`vcpkg.json`). The deploy has a `python.exe`
  that the app already runs as a process, for uv.

**What the plugins weigh** (a local Windows build, without symbols; a release's full package is
225 MB):

| Plugin | Size | Ships today |
|---|---|---|
| Jolt Physics | 2.5 MB | Yes |
| S3, WebDAV, HTTP assets | 0.7, 0.5, 0.3 MB | Yes |
| Viewport Broadcast | 0.3 MB | Yes |
| Blender mesh tools | 2.2 MB | Its own distribution (GPL) |
| MoonRay pack | 214 MB download | A separate archive, Linux and macOS |
| Cycles pack | 126 MB installed | Not yet |
| Gatling pack | 58 MB zipped | Not published (GPL) |

So taking the shipped plugins out saves about 4 MB of 225. What the registry is for:

- **The renderer packs,** which are larger than usdcc and today are unpacked over it by hand. On
  macOS that can't be done to a signed bundle at all, so there is a second disk image, and that
  copy can't update itself (UPDATES.md §9). *Since 2026-11-26 these, and optional packs of any
  other kind, get their own download in the app instead (Status), which leaves the three
  points below as the registry's reasons.*
- **Licences.** A GPL plugin as its own download, chosen by the user, rather than a second
  distribution of the whole app. Whether usdcc may publish one is still USD_PLUGINS.md §8
  item 2; the registry makes it practical, not allowed.
- **Plugins on their own schedule,** fixed without a usdcc release.
- **Studios and third parties.** A studio's own tools, and the `local` packs it alone may build
  (Arnold, FBX), offered to its artists from its own server.

## 3. A registry

### 3.1 Where it is

Any URL of an `index.json`:

- `https://`, for a web server or GitHub Pages. Plain `http://` is refused.
- `file:` and network shares (`file://HOST/share/...`), for a studio without a web server and
  for machines without the internet.

Archive URLs in it are relative to the index, or absolute `https://`. An archive may redirect
(a GitHub release asset does); its hash is checked whatever it came from.

### 3.2 The index

```json
{
  "schema": 1,
  "name": "usdcc plugins",
  "publisher": "usdcc",
  "homepage": "https://github.com/olihey/usdcc",
  "generated": "2026-12-03T10:15:00Z",
  "plugins": [
    {
      "id": "org.usdcc.moonray",
      "name": "MoonRay",
      "description": "DreamWorks' path tracer as a viewport renderer",
      "author": "usdcc",
      "license": "Apache-2.0",
      "homepage": "https://openmoonray.org",
      "icon": "icons/org.usdcc.moonray.png",
      "tags": ["renderer"],
      "versions": [
        {
          "version": "1.2.0",
          "api": 1,
          "parts": ["USD"],
          "requires": [],
          "python": { "requires": [], "indexes": [] },
          "changes": "Follows MoonRay 2.4",
          "builds": [
            {
              "built_for": { "platform": "linux-x64", "usdcc": "26.12.1" },
              "url": "org.usdcc.moonray-1.2.0-linux-x64-26.12.1.zip",
              "sha256": "57a6a5f39fa2cc834dc086a27b7b2e572c12fd14f8377fb8bd1c7022df3d7ccb",
              "size": 224395264
            }
          ]
        }
      ]
    },
    {
      "id": "com.example.notes",
      "name": "Notes",
      "description": "Notes on prims",
      "author": "Example Studio",
      "license": "MIT",
      "versions": [
        {
          "version": "1.2.0",
          "api": 1,
          "usdcc_min": "26.12.1",
          "parts": ["Python"],
          "python": { "requires": ["numpy>=1.26,<3"] },
          "builds": [
            { "url": "com.example.notes-1.2.0.zip", "sha256": "…", "size": 18244 }
          ]
        }
      ]
    }
  ],
  "blocked": [
    { "id": "com.example.notes", "versions": ["1.1.0"], "reason": "Deletes prims on undo" }
  ]
}
```

- **A plugin's entry is its manifest plus where to get it.** `id`, `name`, `description`, `api`,
  `python` and `requires` are copied from `usdcc-plugin.json`; `parts` is what
  `pluginTypes()` gives.
- **`author`, `license` (an SPDX name), `homepage`, `icon` and `tags` are new manifest fields,**
  optional, so the Plugins panel can show them for a plugin copied in by hand too.
- **`builds`** is one entry for a plugin without a C++ part, with no `built_for`. With one, it
  is an entry per build, and `built_for` is the manifest's own (`platform`, `compiler`, `qt`,
  `usd`, `usdcc`), compared by the same function that decides whether the library loads. The
  example shortens it.
- **`usdcc_min` and `usdcc_max`** bound a Python plugin that needs a newer app than `api` says.
- **Several versions** may be listed. The app takes the newest that has a build for it, so an
  older usdcc keeps finding its last one.
- **`blocked`** withdraws a version: it isn't offered, and an installed one is turned off at the
  next start, with the reason, until the user turns it on again.
- **Text is shown as plain text,** never as HTML. Links open in the browser, only on a click. An
  icon is a PNG of limited size, fetched when the list is shown, and cached.
- **`schema`** is raised only when an older app can't read the file; unknown keys are ignored.

### 3.3 Making one

usdcc is the tool, as Blender is for its repositories, since it has the manifest reader and
libsodium:

```bash
usdcc --plugin-registry pack <plugin folder> -o <registry folder>
usdcc --plugin-registry index <registry folder> [--sign <key file>] [--merge <index.json or URL>]
usdcc --plugin-registry keygen <key file>
```

- **`pack`** checks the manifest as the app would, zips the folder (without `__pycache__`,
  symbols and import libraries) and names the zip `<id>-<version>[-<platform>-<usdcc>].zip`.
- **`index`** reads every zip's manifest in the folder and writes `index.json`, with hashes and
  sizes, and `index.json.sig` when given a key. `--merge` keeps the entries of an earlier index
  whose archives aren't in the folder, which is how a release adds its builds (§7).
- **`keygen`** writes a key pair and prints the public key, as `ed25519:<base64>`, for the
  registry's users to check against.

A studio's registry is then: pack the plugins into a folder, index it, share the folder.

## 4. Compatibility

| Plugin | Archives | What decides |
|---|---|---|
| Python, USD schemas | One | `api`, and `usdcc_min` / `usdcc_max` if given |
| With a C++ part, or a USD library | One per platform and usdcc version | `built_for`, exactly |

**After usdcc updates itself, its C++ plugins from a registry no longer load.** Two steps keep
that from being seen:

1. **Before the restart.** Once an app update is downloaded, usdcc reads the registries again and
   downloads, for each installed C++ plugin, the build for the new version. They go into the
   store beside the current ones (§6.3 keeps several builds of a plugin), so the new usdcc finds
   them at its first start.
2. **At a start without it** (the registry was unreachable, or the build isn't published yet):
   the plugin is **Needs update**, a new state beside **Needs packages**, saying "Needs the build
   for usdcc 26.12.2", and a toast offers **Update Plugins**. None of its code has run.

A build of usdcc that isn't a release (a developer's) matches no published build, and says so,
as it does today for a plugin copied in.

**The other way, chosen on 2026-11-26 as the precondition:** a stable C++ interface, so one
build serves many versions. It was first set aside here as not needed while the official registry
is built by the release itself (§7). What makes it a project of its own: `std::` and Qt types
cross the boundary, and USD's symbols carry its version (PLUGINS.md §3.2); a plugin restricted to
a C interface couldn't derive from `ViewPanel`; and a plugin with a USD library (a resolver, a
render delegate) is tied to the OpenUSD version whatever usdcc's own interface is. So "stable"
most likely means stable across usdcc releases that share a Qt and an OpenUSD, not across all of
them, and `built_for` would still be compared for those two.

## 5. Trust

### 5.1 What is checked

| Check | Protects against |
|---|---|
| The archive's SHA-256 and size, from the index | A changed or swapped archive, wherever it is hosted |
| The index's Ed25519 signature, `index.json.sig`, against the registry's pinned key | A changed index: a broken-into web server, a proxy that opens HTTPS |
| `generated` not older than the copy the app has | An old index served again, to bring back a withdrawn version |
| The unpacked manifest's `id` and `version` equal the index's | An archive that isn't what it was listed as |
| Paths while unpacking stay inside the folder; no links out of it | An archive that writes elsewhere |

- **The key is pinned when the registry is added.** The dialog shows the registry's name,
  publisher, URL and the key's fingerprint, to compare with what the studio or the website says.
  A later index signed by another key is refused, with both fingerprints. The official registry's
  key is compiled into the app.
- **A registry may be unsigned** (a folder on a share). It is marked "Not signed" and can't be
  made trusted (§5.2): every install and update from it asks.
- **usdcc's own updates rest on HTTPS and GitHub today,** unsigned (UPDATES.md §2). The
  official registry's signing key would be a secret of the release workflow, so someone with
  control of that workflow has both.

### 5.2 Who is trusted

A registry has one of two levels, chosen when it is added and changed in Settings:

| Level | Installing | Updating |
|---|---|---|
| **Trusted** | One click | One click, or on their own with **Install updates automatically** (off by default) |
| **Ask** (the default) | A dialog the first time, per plugin | One click for a plugin already trusted |

- **The dialog** says what the plugin is, from the index: author, licence, the registry it comes
  from, its parts (`C++`, `Py`, `USD`), its Python packages and their indexes, the plugins it
  requires, and the download's size. **Install** trusts that plugin of that registry.
- **A trusted plugin asks again when it becomes something else:** an update that adds a C++
  part, names a new package index, or changes its author.
- **A plugin belongs to the registry it was installed from.** Another registry listing the same
  id, at any version, is never an update for it; the list shows it as a different entry, and
  installing it replaces the first after asking. This keeps a second registry from taking over
  an id.
- **Plugins copied into a folder by hand load as they do today.** Someone had to put them there.

### 5.3 A studio's rules

`USDCC_PLUGIN_REGISTRIES` names a file, or an `https://` URL of one, for launch scripts, as
`USDCC_PLUGIN_PATH` and `USDCC_ASSETS_CONFIG` are:

```json
{
  "registries": [
    { "name": "Studio A", "url": "https://plugins.studio-a.example/index.json",
      "key": "ed25519:…", "trust": "trusted" }
  ],
  "only": true,
  "install": ["com.studio-a.pipeline", "com.studio-a.publish"]
}
```

- **Its registries are added,** read-only in Settings, with the key and trust it gives. The
  environment is the studio's, and `USDCC_PLUGIN_PATH` already loads whatever it names.
- **`only`** removes every other registry, the official one included, and the user can't add any.
- **`install`** are plugins every seat should have: missing ones are offered at the start in one
  toast, and installed by the command line (§6.4).

## 6. In the app

### 6.1 Settings and the panel

**Settings > Plugins > Registries:** a list of name, URL, level and state ("checked 10:15",
"offline, as of yesterday", "Not signed"), with add, remove and refresh. The official one is
there from the start and can be turned off. Beside it: **Check for plugin updates when usdcc
starts** (on).

**View > Plugins** gets two lists:

- **Installed,** as today, with each plugin's registry and, when there is one, **Update to
  1.3.0**, its changes on a click. A plugin from a registry has **Uninstall**.
- **Browse:** every plugin of every registry that has a build for this app, with icon, name,
  author, description, size, tags and **Install**; a search field; one registry at a time or
  all. A plugin without a build for this platform or version is listed greyed, with why. One
  whose id belongs to a feature left out of this build ([BUILD_OPTIONS.md](BUILD_OPTIONS.md)) isn't
  listed.

### 6.2 When registries are read

- **Ten seconds after the window shows,** with the app's update check, when the setting is on;
  on **Refresh**; and when Browse is opened with a copy older than an hour.
- **A conditional request** (ETag) for one small static file per registry. Not GitHub's API, so
  its hourly limit doesn't apply.
- **The last valid copy** is kept in the user's data folder and used when the registry can't be
  reached or the new file doesn't verify. The list says how old it is and why.
- **A toast** only when there is something to do: "Updates for 2 plugins" with **Show**, or the
  studio's missing plugins (§5.3). New plugins in a registry are in Browse, not in a toast.

### 6.3 Installing

Installed plugins live in a store of their own, not in the user's `plugins` folder, which stays
for what the user puts there:

```text
%LOCALAPPDATA%\usdcc_config\plugin-store\
  installed.json                      per id: registry, version, each build's folder and built_for
  com.example.notes\1.2.0-57a6a5f39fa2\     a plugin folder; the name ends in its archive's hash
  org.usdcc.jolt\1.4.0-9f8e7d6c5b4a\        the build for usdcc 26.12.1
  org.usdcc.jolt\1.4.0-1a2b3c4d5e6f\        the build for 26.12.2, fetched before the update
  .downloads\
```

1. Download to `.downloads`, in the background, with Qt Network and the system's proxy, as
   `release_check` does. Check size and SHA-256.
2. Unpack into `<folder>.partial`, in a process of the deploy's `python.exe`, as uv runs, so the
   app stays responsive. Check the manifest. Rename.
3. Write `installed.json` last, and atomically. A failed or cancelled install leaves things as
   they were.
4. **A new Python-only plugin starts at once,** through `addPlugin()`. A plugin with a USD or
   C++ part, and every update, takes effect at the next start, and the panel says so.
5. **Python packages** the plugin needs are in the same confirmation, and installed right after
   by the existing installer.
6. **Plugins it requires** are found in the same registry first, then in the others, and listed
   in the confirmation.

- **Old folders go at a start, when nothing uses them:** each running usdcc holds an open
  `.in-use` file in the folders it loaded, the rule of PYTHON_PACKAGES.md §3, since Windows lets
  a loaded DLL be deleted.
- **Uninstall** takes the plugin out of `installed.json`; its folder goes the same way. Its
  settings stay.
- **Where the store is in the search order:** after the user's and the added folders, before the
  shipped one. A store copy that isn't newer than the shipped plugin of the same id is passed
  over, so a newer usdcc isn't held back by an old download. The store gives `PluginManager` one
  folder per id, the build that fits this app.
- **Files usdcc downloads itself carry no download mark,** so Defender doesn't scan each DLL at
  its first load (UPDATES.md §1).

### 6.4 For farm machines

```bash
usdcc --install-plugins [<id> ...]
usdcc --update-plugins
```

With no ids, the studio's `install` list. No window; 0 when everything went in, 2 when some was
left out, 1 when it failed, as `--install-plugin-packages`, which they run afterwards. Only
from registries that are trusted: there is nobody to ask.

## 7. The official registry

- **Only what usdcc builds** (§1). Every archive in it comes out of this repository's workflows,
  from a pinned source, so its key signs nothing that wasn't built here. There is no submission
  or review process to run.
- **Where it is hosted isn't decided** (§11 item 1). The design asks only for a static file at an
  `https://` URL that the app knows, and archives at URLs that file names; the URL is one
  constant in the app, beside the key.
- **`release.yml` builds it.** Each platform's job packs that platform's builds of the registry's
  C++ plugins against the usdcc it just built, and attaches them to the release. A last job, after
  all of them, runs `index --merge` on the published index, signs it, and publishes it. A
  platform whose job failed simply has no build of that release.
- **Portable plugins** are packed once, when their version changes.
- **The index is checked in CI** before it is published: each archive downloads, matches its
  hash, and installs into the deploy the smoke test starts.

**What moves out, and what stays:**

| Plugin | | Why |
|---|---|---|
| HTTP, WebDAV, S3 assets | Stay shipped | 1.5 MB; a stage needs them before any registry is read, and on farm machines |
| Viewport Broadcast, Jolt Physics | Stay shipped for now | 2.8 MB; moving them proves phase 4 and nothing else |
| V-Ray Render | Registry (phase 2) | Python; only useful with a V-Ray licence |
| RTX Render | Registry (phase 4) | Off by default today; needs NVIDIA's SDK |
| Blender mesh tools | Registry (phase 4), after the licence question | GPL: its own download instead of a second distribution |
| Optional packs: MoonRay, Cycles, Gatling, and any kind that follows | **Not the registry:** their own download in the app (Status) | The size; and on macOS, a signed bundle nothing can be unpacked into |
| Arnold, FBX | A studio's registry | `local` packs, never published by usdcc |

**A USD pack as a plugin** is a folder with a manifest and the pack's files under `usd/`, which
`registerUsdFolders()` registers before the first stage. Two things are unproven. The packs'
own download in the app avoids both where it unpacks over usdcc's folder, and meets the second
only on macOS and in the AppImage (USD_PLUGINS.md §9):

- **Windows:** OpenUSD loads a plugin's library with plain `LoadLibraryW`, which doesn't look in
  that library's folder for the DLLs it needs ([deploy.py](../tools/deploy.py), at
  `SHIPPED_NATIVE_PLUGINS`); the asset protocols keep theirs in `bin` for that reason. A pack's
  own DLLs (`libmdl_sdk.dll`, `fileformatUtils.dll`) would need loading by full path first, as
  `arnold_runtime.cpp` does for `ai.dll`: Windows then takes the one already loaded, by its
  name. `AddDllDirectory` alone, which `python_runtime.cpp` uses for Python's imports, isn't
  enough: a plain `LoadLibraryW` doesn't look in the folders it adds.
- **Linux and macOS:** a pack's files have RUNPATHs relative to an OpenUSD install
  (USD_PLUGINS.md §5), which a plugin folder isn't laid out as. *On Linux, tried 2026-11-26:*
  a folder laid out as an install, with `lib` a link to the app's, is enough for MoonRay's
  pack (USD_PLUGINS.md §9). macOS isn't tried.

## 8. Considered, and not chosen

| | Why not |
|---|---|
| A git repository as the list, releases as the archives (Obsidian) | Needs git or GitHub's API on every seat; no network shares |
| A service with search, ratings and accounts (the VS Code Marketplace) | Something to host and moderate; a studio can't copy it. A service can serve the same `index.json` later |
| One XML or JSON file per app version (JetBrains) | Fits when the server filters by the app's build; here the client picks from `builds` |
| Hashes only, HTTPS as the protection (Blender, ASSET_SOURCES.md §7) | The index decides what code runs; whoever can write to the server would decide it |
| A signature per author, on each archive | A second set of keys to manage and show, for a case that doesn't arise: a registry has one publisher, and the official one only usdcc's builds (§1) |
| Other people's plugins in the official registry | Each version would need reviewing, and usdcc's key would sign code it didn't build. They host their own registry |
| The Update Framework (TUF) | Its roles and rotating metadata are for registries with many publishers and a threat model this doesn't have yet |
| Installing updates at startup | The network at launch, and a plugin changing under a session; PYTHON_PACKAGES.md §4 |

## 9. Phases

None is started (Status). Phase 4 is the one a stable SDK shrinks, and phase 5 has moved out of
this plan.

| Phase | Builds | Done when |
|---|---|---|
| **1. Archives** | The new manifest fields, in the reader, the Plugins panel and the VS Code schema; `--plugin-registry pack`; the store; **Install from File...** in View > Plugins | A packed Python plugin installs from a zip, runs, updates from a second zip at the next start, and uninstalls |
| **2. Registries** | `index`, reading and caching an index, Settings > Plugins > Registries, Browse, install, update and uninstall of portable plugins, the hash, the trust dialog and the two levels, the startup check and its toast | V-Ray Render installs from a registry on a share and from one on a web server. It stays in the deploy until the official registry has a home (§11 item 1) |
| **3. Signing and studios** | `keygen`, `--sign`, the pinned key, `generated`, `blocked`; `USDCC_PLUGIN_REGISTRIES` with `only` and `install`; `--install-plugins`, `--update-plugins` | A changed index and a changed archive are each refused, saying which; a seat with `only` shows the studio's registry alone |
| **4. C++ plugins** | Builds per `built_for`, several builds in the store, **Needs update**, the download before an app update; `release.yml` packing, merging, signing and publishing | An install updated from one release to the next starts with its registry C++ plugin running |
| ~~5. USD packs~~ (moved out) | A pack as a plugin folder on each platform, after §7's two points are proven with the Adobe pack; then MoonRay, in place of its archive and its disk image | A macOS usdcc from the plain disk image installs MoonRay, renders with it, and still updates itself |

## 10. Testing

- **Unit tests** with registries the tests make in a temporary folder (`file:` URLs, no network):
  picking a build, a wrong hash, a wrong signature, an older `generated`, a blocked version, a
  manifest that isn't the listed one, an archive with a path out of its folder, two registries
  with one id, an install cancelled between download and rename, the `.in-use` rule.
- **In the app,** under a test profile, with scripts pressing the real buttons: the phases'
  "done when" lines, and a registry that goes away between two starts.
- **CI:** the published index, as in §7.

**Not checked, to check before the phase that needs it:**

- §7's two points about USD packs.
- **macOS:** whether the signed app loads a downloaded library that is signed ad hoc (Apple
  silicon runs no unsigned code, [MACOS.md](MACOS.md)), and whether a file usdcc downloads is
  quarantined. The app's entitlements turn library validation off
  (`com.apple.security.cs.disable-library-validation`), so it is expected to; not tried.
- **Corporate proxies** that open HTTPS, and a registry behind a login.
- **Blender's index fields** were not read (the manual's page for them wasn't found); its hosting
  model was, on 2026-11-26.

## 11. Open questions

| # | Question | Assumption until decided |
|---|---|---|
| 1 | Where are the official index and its archives hosted? | **Left for later** (2026-11-26). Nothing before phase 4 needs it: phases 1 to 3 run against registries in a folder. Candidates: GitHub Pages with the archives as release assets (nothing new to run; plain download URLs, not the API), or a server of usdcc's own |
| 2 | Does the official registry take other people's plugins? | **No** (decided 2026-11-26): only what usdcc builds, its builds of third-party code included (§1, §7) |
| 3 | Zip, or `tar.xz`? | Zip. A zip loses symbolic links and executable bits unless they're handled, which phase 5's Linux and macOS packs need: `tar.xz` for those, or a zip reader that keeps them |
| 4 | How long does the index keep builds for old usdcc versions? | Those of the last twelve months. Longer, and the builds move to a file per usdcc version that the index names |
| 5 | Changing a registry's key? | A new index names the next key, signed by the current one. Until that exists: remove the registry and add it again |
| 6 | Registries behind a login? | Later, with the logins of ASSET_SOURCES.md §5 |
| 7 | May usdcc publish GPL plugins in its registry? | USD_PLUGINS.md §8 item 2 and [RELEASE.md](RELEASE.md) §6 decide; not until then |
| 8 | Do Viewport Broadcast and Jolt Physics move out too? | Not before phase 4 has run for a few releases |
| 9 | When does this start? | **Parked** (decided 2026-11-26) until a stable Plugin SDK exists. That SDK has no plan yet |
| 10 | Could the portable half (phases 1 to 3: Python and schema plugins, which need no build per version) go ahead on its own? | No: the decision parks all of it. It is the part that doesn't depend on the SDK, if that is ever wanted earlier |

## 12. Not planned

- **A sandbox,** or permissions a plugin asks for and is held to.
- **Ratings, download counts, accounts, paid plugins.**
- **A plugin a stage brings with it.** Code comes from folders and registries the user or the
  studio chose (PLUGINS.md §3.6).
- **Installing uv on demand** (31 MB of the deploy): a different mechanism, as it isn't a plugin.
