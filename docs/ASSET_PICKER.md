# Asset Picker (plan)

One dialog for choosing an asset: a local file, or a file on any server that can be browsed. It
replaces the system file dialog wherever usdcc asks for a USD file or an asset path.

**Status:** planned 2026-11-16. Nothing is built yet. It is phase 5 of
[REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13.

## 1. Why it needs its own interface

`ArResolver` can't list a folder. *Checked* in the local OpenUSD build's `ar/resolver.h`: it
creates identifiers, resolves, opens and writes single assets, and has nothing that enumerates. So
browsing is a separate interface that a resolver's plugin may also provide, and a resolver without
one can only be given typed paths.

## 2. Asset sources

```cpp
namespace usdcc::usd {

struct AssetRoot  { std::string label; std::string uri; };            // "studio", "webdav://studio/"
struct AssetEntry { std::string name; bool isFolder = false; uint64_t size = 0; double modified = 0;
                    std::string origin; };                             // "test"; empty when there is one

class AssetSource {
public:
    virtual ~AssetSource();
    virtual std::string id() const = 0;
    virtual std::string label() const = 0;                             // "WebDAV"
    virtual std::vector<AssetRoot> roots(const PXR_NS::ArResolverContext&) const = 0;
    // On a worker thread. Throws with a message for the dialog.
    virtual std::vector<AssetEntry> list(const std::string& uri, const PXR_NS::ArResolverContext&,
                                         const CancelToken&) = 0;
    virtual bool canWrite(const std::string& uri, const PXR_NS::ArResolverContext&) const { return false; }
};

class AssetSourceRegistry {    // a singleton in usdcc_usd, which has no Qt
public:
    static AssetSourceRegistry& instance();
    void add(std::shared_ptr<AssetSource>);
    void remove(const std::string& id);
};

}  // namespace usdcc::usd
```

Who registers one:

| Source | Registered by |
|---|---|
| **Every protocol built on `usdcc_assets`** | The app, once: one source that lists each name of the stage's environment through the transports ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §6). A listing is the union of the stack, each entry with its origin |
| **Another resolver's plugin** | `Host::addAssetSource()` in C++ ([PLUGIN_SDK.md](PLUGIN_SDK.md)), `usdcc.usd.register_asset_source()` in Python |
| **Local files** | Built into the dialog, not a source |

## 3. The dialog

```
+--------------------------------------------------------------------+
| Choose Asset                                                       |
| [ webdav://studio/chars/hero/                               ] [Up] |
+----------------+---------------------------------------------------+
| This PC        | Name            Size      Modified          From  |
| Recent         | tex/                                              |
|                | hero.usd        12 KB     2026-11-12 09:14  prod  |
| WebDAV         | hero_geo.usdc   48 MB     2026-11-15 17:02  test  |
|   studio       |                                                   |
|   lib          |                                                   |
+----------------+---------------------------------------------------+
| File name: [ hero.usd            ]   [ USD Files (*.usd *.usda…) ] |
| Path: (o) Relative  ( ) Absolute        [System dialog…] [Open] [Cancel] |
+--------------------------------------------------------------------+
```

- **Places:** This PC (a `QFileSystemModel`), Recent folders, then each source with its roots.
- **The list** loads a folder when it's opened, on a worker thread, with a busy row meanwhile and
  the error in its place when it fails. Leaving a folder cancels its listing. Listings share the
  resolver's cache, so a folder a stage just composed from appears at once.
- **The path bar** takes a typed or pasted path or URI. One no source can list is accepted if
  `ArGetResolver().Resolve()` finds it, which is how a resolver without a source is still usable.
- **From** shows which endpoint delivered an entry, when a name has more than one.
- **Filters** by extension, as today's dialogs have.
- **Relative or Absolute** is shown when the result goes into a layer (§4).
- **System dialog…** opens the OS file dialog, for network drives, shell extensions and habits. Its
  choice comes back through the picker.
- **With no source registered,** usdcc opens the system dialog directly, exactly as today. The
  picker only appears once there is something to browse besides the disk.

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
| The asset field's browse button | [property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp) |
| Add Reference, Add Payload | [property_sheet.cpp](../src/cpp/usd/src/property_sheet.cpp) |
| Prim picker's file | [prim_picker_dialog.cpp](../src/cpp/usd/src/prim_picker_dialog.cpp) |
| A session's "Where is …?" | [session_controller.cpp](../src/cpp/scripting/src/session_controller.cpp) |

Save sites, with writing in phase 6: Save Stage As, New Layer File, Save Layer As, Flatten to File.
The picker gets a save mode: a name field, **New Folder**, an overwrite question, and only
locations where `canWrite()` is true. In an environment with a stack, a save lands on the first
writable endpoint, and the dialog says which.

Not changed: scripts, session import and export, render images, settings folders. Those are local
files.

## 6. Python

`usdcc.usd.pick_asset(title, filters, stage=None, anchor_layer=None)` returns the asset path or
`None`, and `usdcc.usd.register_asset_source(source)` takes an object with `id`, `label`, `roots()`
and `list()` ([SCRIPTING.md](SCRIPTING.md)).

## 7. Testing

- **The model** (`tests/asset_picker_test.cpp`) with a fake source: lazy loading, a slow listing
  cancelled by leaving the folder, a failing listing's message, the union of a stack with its
  origins, and filters.
- **What it returns:** the cases of §4, including a relative path across two names being refused.
- **In the app, looked at:** browsing the WsgiDAV test server ([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md)
  §6), adding a reference from it, and that nothing changes with no source registered.

## 8. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should local files use the picker too, once a source exists, or keep the system dialog with the picker only for servers? | The picker, with **System dialog…** in it: one place to choose from, and choosing local or remote isn't a decision before the dialog opens |
| 2 | Thumbnails or a preview of the chosen file? | Later. The USDA preview dialog could be reused for a selected `.usd` |
| 3 | Bookmarks of the user's own in Places? | Later; Recent covers most of it |
| 4 | Search within a server? | No. WebDAV has no usable search; a source could offer one later |
