# Asset Picker

One dialog for choosing an asset: a local file, or a file on any server that can be browsed. It
replaces the system file dialog wherever usdcc asks for a USD file or an asset path.

**Status:** built 2026-11-17, phase 5 of [REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13, but for what §9
lists. Planned 2026-11-16; favourites (§3.1) and the scheme dropdown (§3.2) were added on the 17th.
The code: [asset_sources.h](../src/cpp/usd/include/usdcc/usd/asset_sources.h) (the sources, Qt-free)
and [asset_picker.h](../src/cpp/usd/include/usdcc/usd/asset_picker.h) (the dialog, favourites,
`pickAsset()`).

## 1. Why it needs its own interface

`ArResolver` can't list a folder. *Checked* in the local OpenUSD build's `ar/resolver.h`: it
creates identifiers, resolves, opens and writes single assets, and has nothing that enumerates. So
browsing is a separate interface that a resolver's plugin may also provide, and a resolver without
one can only be given typed paths.

## 2. Asset sources

```cpp
namespace usdcc::usd {

struct AssetRoot      { std::string label; std::string uri; };       // "studio", "webdav://studio/"
struct AssetListEntry { std::string name; bool isFolder = false; uint64_t size = 0; double modified = 0;
                        std::string origin; };                         // "test"; empty when there is one

class AssetSource {
public:
    virtual ~AssetSource();
    virtual std::string id() const = 0;
    virtual std::string label() const = 0;
    virtual std::vector<std::string> schemes() const = 0;              // what it lists: "webdav", "webdavs"
    virtual std::vector<AssetRoot> roots(const std::string& scheme, const PXR_NS::ArResolverContext&) const = 0;
    // On a worker thread. Throws std::runtime_error with a message for the dialog.
    virtual std::vector<AssetListEntry> list(const std::string& uri, const PXR_NS::ArResolverContext&,
                                             usdcc::assets::Cancel& cancel) = 0;
};

class AssetSourceRegistry {    // a singleton in usdcc_usd, which has no Qt
public:
    static AssetSourceRegistry& instance();
    void add(std::shared_ptr<AssetSource>);
    void remove(const std::string& id);
    std::shared_ptr<AssetSource> forScheme(const std::string& scheme) const;
    std::vector<std::string> browsableSchemes() const;
};

}  // namespace usdcc::usd
```

Who registers one:

| Source | Registered by |
|---|---|
| **Every protocol built on `usdcc_assets` that can list** | `AssetSourceRegistry` itself, on first use: one source for every scheme whose transport says `canList()` (WebDAV's do), listing through `usdcc::assets::listUri()`. Its roots are the names of the stage's environment; a server by address (`webdav://192.168.1.5:5000/`) is listed through the address parser its protocol registers. A listing is the union of the stack, each entry with its origin. Plain HTTP can't list, so its URLs are typed (§3) |
| **Another resolver's plugin** | `AssetSourceRegistry::instance().add()` in C++. *Not built:* `Host::addAssetSource()`, which would also note it in the Plugins panel, and Python's `usdcc.usd.register_asset_source()` |
| **Local files** | Built into the dialog, not a source |

## 3. The dialog

```
+--------------------------------------------------------------------+
| Choose Asset                                                       |
| [webdav:// v][ studio/chars/hero/                       ] [*] [Up]  |
+----------------+---------------------------------------------------+
| Favourites     | Name            Size      Modified          From  |
|   Library      | tex/                                              |
|   Hero         | hero.usd        12 KB     2026-11-12 09:14  prod  |
|   Textures     | hero_geo.usdc   48 MB     2026-11-15 17:02  test  |
| This PC        |                                                   |
| Recent         |                                                   |
| WebDAV         |                                                   |
|   studio       |                                                   |
|   lib          |                                                   |
+----------------+---------------------------------------------------+
| File name: [ hero.usd            ]   [ USD Files (*.usd *.usda…) ] |
| Path: (o) Relative  ( ) Absolute        [System dialog…] [Open] [Cancel] |
+--------------------------------------------------------------------+
```

- **Places:** Favourites (§3.1), This PC (a `QFileSystemModel`), Recent folders, then each source
  with its roots. **[\*]** in the path bar makes the folder shown a favourite, or stops it being one.
- **The path bar** is a scheme dropdown and the rest of the path (§3.2).
- **A protocol's roots** in Places are the names its environment has. **Edit Servers...** in their
  context menu opens the Asset Servers editor, where they're made
  ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §10.1).
- **The list** loads a folder when it's opened, on a worker thread, with a busy row meanwhile and
  the error in its place when it fails. Leaving a folder cancels its listing. Listings share the
  resolver's cache, so a folder a stage just composed from appears at once.
- **A typed or pasted path** that no source can list is accepted if `ArGetResolver().Resolve()`
  finds it, which is how a resolver without a source is still usable, and how an `https://` URL is
  opened ([HTTP_RESOLVER.md](HTTP_RESOLVER.md)). Recent keeps the URLs used.
- **From** shows which endpoint delivered an entry, when a name has more than one.
- **Filters** by extension, as today's dialogs have.
- **Relative or Absolute** is shown when the result goes into a layer (§4).
- **System dialog…** opens the OS file dialog, for network drives, shell extensions and habits. Its
  choice comes back through the picker.
- **With no source registered,** usdcc opens the system dialog directly, exactly as today. The
  picker only appears once there is something to browse besides the disk.

### 3.1 Favourites

Folders someone goes back to, on disk or on a server, one click away in Places. Recent lists what
was used last; favourites are what the user chose to keep, in the order they chose.

- **Folders only.** A favourite opens its folder in the list; a file is picked from there.
- **The user's own, and only theirs.** Kept in usdcc.ini (`AssetPicker/favorites`, a JSON list of
  `{label, path}`), per user. There is no shared list: not in the asset configuration, not from a
  studio, not exported.

**What a favourite holds:** a label and a path as written, never what it resolved to:

- **On disk:** the absolute path, with forward slashes.
- **On a named server:** the logical path, `webdav://studio/chars/hero/`. Never an endpoint's URL
  or a stack id, so one favourite means TEST's servers in the test environment and PROD's in prod,
  and still works after a server moves ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §5).
- **On a server by address:** as written, `webdav://192.168.178.53:5000/single/`
  ([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §2.1). Opening it allows that host for the session, as
  opening a stage does: the user chose it.

**Adding:**

- **[\*]** in the path bar, for the folder shown.
- **Add to Favourites** in a folder's context menu in the list. A file's menu has no such entry.
- **Dragging** a folder from the list, or a folder's path from outside the app, onto Favourites. A
  file dropped there is refused.

A new favourite's label is the folder's name; a second one of the same name gets its parent's too
(`Hero (chars)`). Adding a folder that is a favourite already selects the one there.

**Managing,** in a favourite's context menu: **Rename**, **Move Up** and **Move Down** (and
dragging), **Remove**, and **Copy Path**. There is no separate dialog: the
list is short, and is edited where it's used.

**Not checked when the dialog opens.** As Open Recent does, nothing asks a disk or a server about a
favourite until it's clicked: an offline share or a server that's down would otherwise hold the
dialog up. When clicked, a missing folder or an unreachable server shows its error in the list, as
any listing does, and the favourite stays. A favourite on a server name the stage's environment
doesn't have is greyed, with the reason in its tooltip: that is known without asking anyone.

**Not in sessions or workspaces.** Favourites belong to the user, not to what's open.

### 3.2 The scheme dropdown

The path bar's first part says where the path is: a dropdown of every URI scheme OpenUSD has a
resolver for, and **file://** for the disk. The field beside it holds the rest.

```
[ file://    v ]  C:/projects/show/assets/
[ webdav://  v ]  studio/chars/hero/
[ webdavs:// v ]  dav.example.com/shots/
[ https://   v ]  example.com/scene.usdz
```

**What's in it:**

| Entry | Comes from | Choosing it |
|---|---|---|
| **file://** | Always there, first | The list shows the disk; the field takes a local path |
| A scheme with a source that lists (§2) | `ArGetRegisteredURISchemes()`, and the source's registry | The list shows its roots: for `webdav://`, the names of the stage's environment as folders (`studio/`, `lib/`) |
| Any other registered scheme | `ArGetRegisteredURISchemes()` | Nothing to browse: the field is for typing or pasting the rest, checked with `Resolve()` |

- **Registered ones only.** The list is read from OpenUSD when the dialog opens, so it holds what
  the loaded resolver plugins registered: usdcc's `webdav`, `webdavs`, `http` and `https`, and any
  a studio's own plugin adds. A protocol plugin that is turned off, or failed to load, isn't in it.
- **Order:** file://, then the schemes that can be browsed, then the others, each group by name.
  An entry's tooltip names the resolver behind it (the type in its `plugInfo.json`).
- **file:// is the dropdown's own.** OpenUSD's default resolver has no `file` scheme: a local
  asset is a plain path. So the picker never writes `file://` into a layer; choosing a local file
  gives a path, as today (§4). A pasted `file:///C:/a/b.usd` is taken as the path `C:/a/b.usd`.
- **Typing or pasting a whole URI** in the field (`webdav://studio/...`, `https://...`) moves its
  scheme into the dropdown. One no resolver is registered for is refused, naming the scheme, and
  the dropdown stays as it was.
- **Switching scheme** clears the field and shows that scheme's roots, or an empty list to type
  into. What was shown before is one click back (**Back**, or Recent).
- **The scheme of what's shown follows navigation:** clicking a WebDAV root in Places or a
  favourite on a server sets the dropdown to `webdav://`, This PC sets it to `file://`.
- **The rest of the path** is completed from the folder's listing, where there is one, as the
  system dialog completes a local path.

```cpp
// Empty when cancelled. An asset path as it should be written: relative to `anchor` when asked for.
QString usdcc::usd::pickAsset(QWidget* parent, const AssetPickerOptions& options);
```

`AssetPickerOptions` holds the title, filters, start location, the stage (for its resolver context)
and the layer the result is anchored against, if any.

## 4. What it returns

An asset path, not a file path:

- **A local file:** as `assetPathForFile()` gives today, relative to the layer's folder or
  absolute.
- **A remote file, relative:** when the layer is under the same name
  (`webdav://studio/shots/s010.usd` choosing `webdav://studio/chars/hero/hero.usd` gives
  `../chars/hero/hero.usd`). Worked out on the URIs' paths alone.
- **A remote file, absolute:** the logical URL, `webdav://studio/chars/hero/hero.usd`. Never an
  endpoint's URL, and never a stack id.
- **Across names or protocols,** only absolute is possible.

The asset field's **Rel/Abs** button ([property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp))
uses `QDir` today and gets the same rules for URIs.

## 5. Where it's used

Read sites, in phase 5:

| Where | Today |
|---|---|
| File > Open Stage, Open Stage with Options | [main.cpp](../src/cpp/app/main.cpp), two `getOpenFileName` |
| File > Show USDA, Show Prims | [main.cpp](../src/cpp/app/main.cpp) |
| Layer Editor > Add Existing Layer | [layer_editor_view_panel.cpp](../src/cpp/usd/src/layer_editor_view_panel.cpp) |
| The asset field's browse button, everywhere it's used: | [property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp), one place for all of these |
| &nbsp;&nbsp;an asset attribute in the Attribute Editor | [property_sheet.cpp](../src/cpp/usd/src/property_sheet.cpp) |
| &nbsp;&nbsp;an asset input of a shading node in the Material Editor (a `UsdUVTexture`'s `inputs:file`) | [material_editor_view_panel.cpp](../src/cpp/usd/src/material_editor_view_panel.cpp), through the same `PropertySheet` in its shading mode |
| &nbsp;&nbsp;a new asset input's value in the Material Editor's Add Input | [add_shade_input_dialog.cpp](../src/cpp/usd/src/add_shade_input_dialog.cpp) |
| &nbsp;&nbsp;a reference's or payload's file in the composition arcs | [property_sheet.cpp](../src/cpp/usd/src/property_sheet.cpp) |
| Add Reference, Add Payload | [property_sheet.cpp](../src/cpp/usd/src/property_sheet.cpp) |
| Prim picker's file | [prim_picker_dialog.cpp](../src/cpp/usd/src/prim_picker_dialog.cpp) |
| A session's "Where is …?" | [session_controller.cpp](../src/cpp/scripting/src/session_controller.cpp) |

Save sites, with writing in phase 6: Save Stage As, New Layer File, Save Layer As, Flatten to File.
The picker gets a save mode: a name field, **New Folder**, an overwrite question, and only
locations where `canWrite()` is true. In an environment with a stack, a save lands on the first
writable endpoint, and the dialog says which. *Built 2026-11-22* (§9.1).

Not changed: scripts, session import and export, render images, settings folders. Those are local
files.

## 6. Python

`usdcc.usd.pick_asset(title, filters, stage=None, anchor_layer=None)` returns the asset path or
`None`, and `usdcc.usd.register_asset_source(source)` takes an object with `id`, `label`, `roots()`
and `list()` ([SCRIPTING.md](SCRIPTING.md)).

Favourites: `usdcc.usd.asset_favorites()` lists the user's as `(label, path)`, and
`add_asset_favorite(folder, label=None)` and `remove_asset_favorite(folder)` change them.

## 7. Testing

- **The model** (`tests/asset_picker_test.cpp`) with a fake source: lazy loading, a slow listing
  cancelled by leaving the folder, a failing listing's message, the union of a stack with its
  origins, and filters.
- **What it returns:** the cases of §4, including a relative path across two names being refused.
- **The scheme dropdown:** file:// first, the fake protocol's scheme and the real ones as
  registered, browsable ones before the others; a pasted URI moving its scheme there, one with no
  resolver refused, `file:///C:/a.usd` taken as a path; navigating setting the scheme.
- **Favourites** (`tests/asset_favorites_test.cpp`): adding from the path bar, a folder's context
  menu and a drop; a file refused in each; a second of the same name labelled with its parent; adding
  one that's there already; renaming, reordering and removing, kept in usdcc.ini across a restart of
  the model; a server's favourite stored as its logical path even when added from a resolved one;
  one whose server name the environment lacks greyed with no request made; one on a dead server
  opening to an error without holding the dialog up.
- **In the app, looked at:** browsing an `rclone serve webdav` server and the tests' own
  ([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §6), adding a reference from it, a favourite on it
  across environments and a restart, and that nothing changes with no source registered.

## 8. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should local files use the picker too, once a source exists, or keep the system dialog with the picker only for servers? | The picker, with **System dialog…** in it: one place to choose from, and choosing local or remote isn't a decision before the dialog opens |
| 2 | Thumbnails or a preview of the chosen file? | Later. The USDA preview dialog could be reused for a selected `.usd` |
| 3 | Bookmarks of the user's own in Places? | Planned: favourites, §3.1 |
| 4 | Search within a server? | No. WebDAV has no usable search; a source could offer one later |
| 5 | Favourites of files, or only folders? | **Decided: folders only** (2026-11-17) |
| 6 | Shared favourites: a studio's list, per environment, or exported to colleagues? | **Decided: none** (2026-11-17). Favourites are the user's own |
| 7 | Favourites in the system file dialog's sidebar too? | No. Windows' own dialog takes no sidebar from the app, and it can't open a server's paths anyway |
| 8 | Should the scheme dropdown hide schemes that can't be browsed (`http`, `https`)? | No. They can't be browsed because plain HTTP has no request for listing a folder (a server's HTML index page isn't part of the protocol), whereas WebDAV has `PROPFIND`. Browsing never comes from OpenUSD's `ArResolver`, which can't list for any scheme, but from a source's `list()` (§2). A URL can still be typed for them, and seeing the scheme says the protocol is installed |
| 9 | Label schemes by their protocol's name ("WebDAV") rather than `webdav://`? | Show the scheme as written, since that is what the path will hold; the protocol's name goes in the tooltip |

## 9. As built

Built 2026-11-17 with what §2 to §5 describe, and checked in usdcc_ui_tests
([asset_picker_test.cpp](../tests/asset_picker_test.cpp), on a source registered by the test):
browsing a scheme's roots and its folders, filters, where entries came from, a failing listing's
message, a slow listing left behind not landing, a scheme with no resolver refused, the star, a
favourite on a name the environment lacks greyed with no request made, favourites added, named,
renamed, moved and removed, recent folders, a `file:///` start, a missing file not chosen, This PC,
and relative paths on disk and on a server.

**Where it's used,** in place of the system dialog: File > Open Stage, Open Stage with Options,
Preview > Show USDA and Show Prims, the Layer Editor's Add Existing Layer, the asset field's "…"
(the Attribute Editor, the Material Editor's node inputs and Add Input, a composition arc's file),
and Add Reference and Add Payload. A sublayer or reference on a server is written relative to its
layer when both are on one server (`relativeAssetPath()`), else by its logical path.

**How it behaves where the plan left it open:**

- **Listings are on a detached thread,** so an offline share or a slow server holds up neither the
  dialog nor its closing. Leaving a folder cancels its listing, and a late answer is dropped.
- **A file picked from a listing is taken as it is;** one typed or pasted is first resolved through
  the stage's environment (on the UI thread, once, for that file).
- **Going to a server's folder allows its host for the session,** as opening a stage does.
- **A busy row while a page comes** (added 2026-11-20): a busy bar and the seconds it has taken,
  at the top of an empty list or under what's shown for Show More, as a slow server's page can take
  half a minute.
- **A folder too big for one page shows its first** (added 2026-11-20): the source's `listPage()`
  gives a page and where the next starts, the status says there are more, and a **Show More** row
  under the entries asks for the next page, added to what's shown. An S3 folder of thousands on a
  slow server shows its first thousand after one request, not after all of them.
- **A refused listing offers Log In** (added 2026-11-20): when a server refused for want of a
  login that Edit > Asset Server Login can save, a **Log In as <credential>...** button under the
  message opens that dialog with the credential filled in (an access key and its secret for S3),
  and the folder is listed again once one is saved. The source says so by throwing
  `AssetLoginNeeded` with the credential id; none is offered for AWS's own credentials
  ([S3_RESOLVER.md](S3_RESOLVER.md) §4.2), which come from AWS's files.
- **The scheme dropdown** holds `file://`, the schemes a source lists, then the rest OpenUSD
  registered; each one's tooltip says whether it can be browsed and through which resolver.
- **Places' server headings** are the schemes with roots (`webdav://` and its names); a scheme with
  none, such as `webdavs://`, is only in the dropdown.
- **The star** only updates itself and Places; the folder isn't listed again.

**Not built:**

- **Dragging** a folder onto Favourites, and reordering them by dragging: Move Up and Move Down do it.
- **Completion** of the path from the folder's listing.
- **Python** (§6), and `Host::addAssetSource()` for C++ plugins.
- **The session's "Where is …?"** keeps the system dialog: a stage on a server is never reported
  missing, so it only ever asks for a file on disk.

**Not checked:** the dialog clicked through by hand, and drawn: the tests drive it offscreen.

### 9.1 The save mode as built

Built 2026-11-22, with phase 6 of [REMOTE_ASSETS.md](REMOTE_ASSETS.md) (§13.6).

- **`pickSaveAsset()`** ([asset_picker.h](../src/cpp/usd/include/usdcc/usd/asset_picker.h)), or
  `AssetPickerOptions::save`: a **Save** button, **New Folder...**, and a question before a file is
  replaced. It returns a path on disk or a server's logical path, never made relative. With
  nothing to browse it is the system's save dialog, as `pickAsset()` is the system's open dialog.
- **Used by** Save Stage As, New Layer File, Save Layer As and Flatten to File. File > Create Stage
  > As File, scripts, session export and render images keep the system dialog: they are files on
  disk.
- **Where it goes.** In a server's folder a note under the list says **Saves to <server>**: the
  first server of the stack that isn't read-only. Where none can take it (every server read-only,
  offline, or a protocol that can't write) the note says **Can't save here** with the resolver's
  reason, and Save refuses with the same words. Nothing is greyed out: the plan had only writable
  locations shown, but a read-only folder is still where a user browses to a writable one.
- **Replacing.** On a server a file is asked about only when it is on the server the save goes
  to. One only PROD has, saved in the test environment, is a new file on TEST, so there is no
  question.
- **A name without an extension** gets the filter's first (`shot` becomes `shot.usda`).
- **New Folder** makes the folder on disk at once. On a server it goes into a folder that isn't
  there yet and says so. The folder is made when a file is saved in it: WebDAV's `put` makes the
  folders a file is in, and S3 has none.
- **A suggested name** for a file not there yet (an in-memory layer's) starts the dialog in its
  folder with the name filled in.
- **Saving to a server** (`layer_actions`): the stage's resolver context is bound around the write,
  so a server's path means the stage's servers. A failure says why. *Can't write
  webdav://studio/shots/shot.usda:* is followed by the resolver's words, such as a file changed on
  the server since it was opened. Relative paths aren't rewritten for a file on a server: there's
  no folder on disk to rewrite them for, so they are written as they read from the layer's own
  folder. A layer on a server has no folder on disk either; before this its server path was read as
  a local one.
- **Tests** ([asset_picker_save_test.cpp](../tests/asset_picker_save_test.cpp), in
  `usdcc_ui_tests`, which registers the WebDAV plugin at start and runs the test servers in
  process):
  - on disk: the Save button, the suggested name, the default extension, the question before
    replacing, and New Folder;
  - on TEST over PROD: the note, no question for a file only PROD has, a question for one TEST
    has, New Folder on a server, and a refusal where every server is read-only;
  - `layer_actions::saveStageAs` writing to TEST with PROD untouched, a new file, a conflict named
    in the error, and a read-only stack refusing.
- **Not checked:** the dialog clicked through by hand in the app.
