# Remote Assets (plan)

Loading USD assets from servers instead of the local disk: a shared library that does the caching,
the server names and the TEST-over-PROD overlays, and one small resolver plugin per protocol on top
of it. The first two protocols are plain HTTP ([HTTP_RESOLVER.md](HTTP_RESOLVER.md)) and WebDAV
([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md)); the asset picker that browses servers is in
[ASSET_PICKER.md](ASSET_PICKER.md).

**Status:** planned 2026-11-16. **Phases 0 and 1 are done:** the spike (§13.1), and the shared
library `usdcc_assets` with its tests, on a fake protocol and no network (§13.2). No real protocol,
nothing in the app and nothing in a deploy yet. "Checked" below means run in the spike or the tests,
or looked at in this repository or the local OpenUSD build (26.08); everything else is still an
assumption.

## 1. Decisions

- **A shared library, `usdcc_assets`, holds everything that isn't a protocol.** The cache, the server
  names, environments, credentials, the HTTP client and the resolver base class. A protocol adds a
  *transport*: four calls (§6). A later S3 plugin reuses all of it.
- **One resolver per protocol, each with its own scheme.** `http://` and `https://` for plain HTTP,
  `webdav://` for WebDAV, `s3://` for a later S3 plugin. A neutral scheme (`asset://`) was
  considered and rejected (§16).
- **The name in the URL is logical.** `webdav://studio/chars/hero.usd`: `studio` is a name, never a
  hostname. A configuration maps it to real servers (§4). No hostnames or secrets are written into
  layers. Plain HTTP is the exception: its URLs are real addresses (§5.1).
- **A name maps to a stack of servers.** The first one that has a file wins, per file. This is how
  a TEST server overrides single assets of PROD (§5).
- **The resolved path stays a URI, and names the stack.** Never the cache file, and never one
  server (§5).
- **Each protocol ships as a usdcc plugin with only a `usd/` folder** (§11). It has no usdcc code:
  the settings page, login dialog and browsing are the app's, written once for every protocol.
- **Read-only first.** Writing is the last phase.

## 2. The parts

| Part | Where | Holds |
|---|---|---|
| `usdcc_assets` (DLL, no Qt) | `src/cpp/assets/`, in the app's `bin/` and in the SDK | Config and environments, stacks, the context, the cache, credentials, the HTTP client, `CachingResolver`, the transport registry |
| A protocol plugin | `src/plugins/http/`, `src/plugins/webdav/` | An `ArResolver` subclass of `CachingResolver`, its transport and its `plugInfo.json` |
| usdcc | `usdcc_usd`, `usdcc_usd_ui` | Binding a context to each stage, the Assets settings page, the login dialog, download progress, the asset picker |

`usdcc_assets` is a DLL and not a static library, because the credential provider, the stack
registry and the transport registry must exist once in the process. It depends on OpenUSD (`ar`,
`tf`, `plug`, `js`) and not on Qt or `usdcc_core`, so `usdcat` and `usdview` can load a protocol
plugin with only `bin/` on the library path. It reports through `TF_WARN` and `TF_DEBUG`, which
usdcc already shows in its Log ([LOGGING.md](LOGGING.md)).

## 3. Asset paths

```
webdav://studio/chars/hero/hero.usd     name "studio", path /chars/hero/hero.usd
chars/hero/tex/albedo.png               relative, anchored against the layer that contains it
```

- **Names** are lowercase letters, digits, `-`, `_` and `.`. An empty name (`webdav:///…`) is an
  error.
- **Identifiers** (`_CreateIdentifier`) are normalized: scheme and name lowercased, `.` and `..`
  collapsed, slashes deduplicated, percent-encoding made canonical. Two spellings of one asset give
  one identifier, as Sdf's layer registry needs.
- **Relative paths** never escape above the name's root. A `..` past it is an error.
- **An identifier is always logical.** USD anchors a relative path against the resolved path of
  the layer it's in, which carries a stack id (§5). The identifier made from it drops the id.
- **`_GetExtension`** comes from the path, never from a query string.
- **`_IsContextDependentPath`** is `true` for a logical path, because the same name can mean
  different servers in different contexts.

## 4. Endpoints, environments and stacks

An **endpoint** is one real server location. An **environment** says, for each name, which
endpoints make up its stack, top first.

```json
{
  "environment": "prod",
  "endpoints": {
    "prod": { "scheme": "webdav", "url": "https://dav.studio.local/assets",
              "credential": "studio-dav", "readOnly": true,
              "mirrors": ["https://dav2.studio.local/assets"] },
    "test": { "scheme": "webdav", "url": "https://dav-test.studio.local/assets",
              "credential": "studio-dav-test" }
  },
  "environments": {
    "prod": { "webdav": { "studio": ["prod"] } },
    "test": { "webdav": { "studio": ["test", "prod"] } }
  }
}
```

- **A stack holds endpoints of one protocol.** `webdav://studio` can only stack WebDAV endpoints.
- **`mirrors`** are other URLs with the same content, tried when the first is unreachable. This is
  failover, and is separate from the stack (§5).
- **`readOnly`** endpoints are skipped when saving (§5).
- **`optional`** (default `false`) lets a stack carry on when that endpoint is unreachable.

**Where the configuration comes from:** the file named by `USDCC_ASSETS_CONFIG`, or else
`assets.json` in the user's usdcc data folder (`%LOCALAPPDATA%/usdcc` on Windows). usdcc's settings
page writes that file, so tools outside usdcc see the same servers.

**Which environment is used:** `USDCC_ASSET_ENV`, or else the file's `"environment"`. usdcc can
also pick one per stage (§10).

A direct protocol (§5.1) has no endpoints or environments here. Plain HTTP keeps its hosts'
credentials and its allow list in an `"http"` section of the same file
([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §4).

**The context.** One context type for every protocol, `usdcc::assets::Context`
(`AR_DECLARE_RESOLVER_CONTEXT`): the environment's name and its stacks. It is made three ways,
all giving the same context:

1. **By default**, when nothing else is given (`_CreateDefaultContext`): from the configuration
   above. This is what makes `usdcat webdav://studio/shot.usd` work.
2. **From a string** (`CreateContextFromString("webdav", "env=test")`), for scripts and tests.
3. **Explicitly**, from C++ or Python, which is how usdcc does it.

Contexts are hashed, compared, printed and saved in sessions, so they hold credential ids and never
secrets (§8). `_RefreshContext` reloads the file and drops what was remembered for that context.

## 5. Resolving through a stack

`_Resolve` asks each endpoint of the stack, top first, whether it has the path. The first that does
wins. If none does, the result is empty, as for a missing local file.

**The resolved path names the stack:** `webdav://studio~7c21/chars/hero/hero.usd`. `7c21` is a short
hash of the stack's endpoints, in order. A process-wide registry maps it back, filled whenever a
context is made. Two reasons:

- **Relative references go through the whole stack again.** USD anchors a layer's relative
  references against its resolved path. Naming one server there would pin them to it, and TEST
  could not override a single texture beside a PROD layer. *Checked:* a `hero.usda` taken from TEST
  got its sibling `geo.usda` from PROD.
- **Opening works with no context bound.** Hydra loads textures on worker threads with no context,
  so the resolved path alone must say where to look. *Checked* that a resolved path opens with no
  context bound; *not checked* in a viewport.

**The id is only ever in a resolved path.** Identifiers stay logical (§3), so a referenced layer is
`webdav://studio/chars/hero.usd` to Sdf, resolved through whichever context is bound. *Checked:*

- **A context is bound whenever USD resolves an identifier:** during composition, on its worker
  threads too, and when an asset-valued attribute is read.
- **Two stages on the same root in two environments coexist.** Sdf keeps two layers with one
  identifier and different resolved paths, and each stage composes its own.
- **Keeping the id in identifiers leaks it into files.** With it, `usdcat --flatten` wrote
  `@webdav://studio~7c21/chars/tex.png@` for a relative asset path; with logical identifiers it
  writes `@webdav://studio/chars/tex.png@`.

One place still shows the id: `UsdStage::Flatten()` puts the root layer's resolved path in the
result's `doc` string. usdcc's Flatten to File rewrites it. usdcc strips the id wherever it shows a
resolved path.

| Operation | Behaviour |
|---|---|
| Resolve | The first endpoint that has the file wins, decided per file |
| Relative references | Resolved through the whole stack |
| Folder listing | The union of all endpoints, the upper one winning, each entry marked with its origin |
| Open | Downloads from the winner into the cache (§7) and returns an `ArFilesystemAsset` on that file |
| Save (phase 6) | Goes to the first endpoint not marked `readOnly`. Saving a layer that came from PROD writes a copy to TEST |
| Timestamp | Changes whenever the winner or its ETag changes, so `SdfLayer::Reload` notices a file that appears in TEST |
| Asset info | The version is the ETag; the repository path is the real URL on the winning endpoint |

**Unreachable is not missing.** A 404 moves on to the next endpoint. A network error does not: it
tries the endpoint's mirrors, and if none answers the resolve fails with a warning naming the
endpoint. Falling through would silently hand out PROD assets while TEST is down. `optional`
endpoints are the exception.

**The origin is shown.** Wherever usdcc shows a resolved path it also says which endpoint delivered
it (§10).

**Limits of the first version:** an upper endpoint can add and replace files but not hide a lower
one's; and changing the environment of an open stage means reopening it.

### 5.1 Direct protocols

A protocol can skip names and stacks, and take the URL as the real address. Plain HTTP does
([HTTP_RESOLVER.md](HTTP_RESOLVER.md)): `https://assets.example.com/chars/hero.usd` means that
host. `CachingResolver` asks its subclass for the stack of a path; a named protocol answers from
the context, a direct one with a single endpoint made from the URL.

| | Named (`webdav://`) | Direct (`https://`) |
|---|---|---|
| The authority is | A logical name | The real host |
| Environments and overlays | Yes | No |
| Resolved path | The identifier with a stack id | The identifier |
| Context-dependent | Yes | No |
| Credentials | Per endpoint | Per host, in the configuration; anonymous by default |
| Which hosts may be contacted | The configured endpoints | An allow list ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §4) |

The cache, offline mode, progress and cancel are the same for both.

## 6. The transport

What a protocol implements. Every call gets the endpoint (URL, credential) and a cancel token, may
be called from any thread, and reports unreachable separately from missing.

```cpp
namespace usdcc::assets {

struct Entry {
    std::string name;
    bool isFolder = false;
    uint64_t size = 0;
    double modified = 0;      // seconds since the epoch
    std::string etag;         // or the nearest thing the server has
};

class Transport {
public:
    virtual ~Transport();
    virtual Result<Entry> stat(const Endpoint&, const std::string& path, Cancel&) = 0;
    // "Not supported" unless overridden: plain HTTP can't list.
    virtual Result<std::vector<Entry>> list(const Endpoint&, const std::string& folder, Cancel&);
    // Writes to `file`. With `knownEtag`, "not modified" is a result and nothing is written.
    virtual Result<Entry> fetch(const Endpoint&, const std::string& path, const std::filesystem::path& file,
                                const std::string& knownEtag, Progress&, Cancel&) = 0;
    // Phase 6. `knownEtag` empty means "only if it doesn't exist yet".
    virtual Result<Entry> put(const Endpoint&, const std::string& path, const std::filesystem::path& file,
                              const std::string& knownEtag, Progress&, Cancel&);
};

}  // namespace usdcc::assets
```

`CachingResolver` implements every `ArResolver` method from these four. A protocol's resolver
subclass only says which scheme and which transport. Only `stat` and `fetch` are required: without
`list`, resolve asks `stat` per file and the protocol can't be browsed; without `put`, it is
read-only.

A transport registers itself with `usdcc_assets` when its library loads. USD loads a resolver's
library lazily, so to browse a server before anything was resolved, `usdcc_assets` loads the plugin
of every resolver type whose `plugInfo.json` entry says `"usdccAssetsProtocol": true`
(`PlugRegistry::GetDataFromPluginMetaData`).

## 7. The cache

**Listings are what resolve asks for.** One listing of a folder answers "does it exist" for every
file in it, with its ETag, time and size. Composition asks about many siblings and UDIM asks about
tiles that don't exist, so a listing per folder is far fewer round trips than a request per file.
The asset picker uses the same call. A folder over a size limit (to be measured) falls back to
`stat` per file, which is also all a protocol without `list` has. Those answers, missing ones too,
are remembered the same way.

- **In a scope** (`_BeginCacheScope`/`_EndCacheScope`, which a stage open or recomposition makes),
  listings and winners are fixed, so one composition sees one consistent state.
- **Outside a scope** they are remembered for a few seconds (configurable).

**Files on disk are stored per version:** `<cache>/<endpoint id>/<path>@<etag hash>.<ext>`, with a
small record beside each (ETag, modified time, size, last use). A new version is a new file, never
an overwrite:

- Windows can't replace a file the `.usdc` reader has memory-mapped.
- An open layer keeps the bytes it loaded until Reload.
- TEST's and PROD's versions of one path sit side by side, so switching environment downloads
  nothing twice.

The extension is kept, for renderers that are handed the cache file (§10).

- **Revalidation:** before a cached version is used, its ETag is compared with the listing's. Equal
  means no request at all; different means a conditional fetch.
- **Downloads** go to a temporary file renamed when complete. However many threads of a process
  want one version, it downloads once. Two processes may each download it, to a file of their own,
  and the first to finish is kept; a lock file to make that one download too is phase 3.
- **A path that can't be a file name** as it is (too long for Windows with the cache's folder in
  front, or with characters Windows refuses) is stored under a hash of it, in a `_` folder.
- **Location:** `USDCC_ASSET_CACHE`, or else `asset-cache` in the user's usdcc data folder. Shared
  by every usdcc process and tool.
- **Size limit:** default 20 GB, least recently used first. A version in use isn't removed.
- **Offline mode** (configuration, or the settings page): resolve and open from the cache only,
  with a warning in the Log. What isn't cached is missing.

## 8. HTTP and credentials

**libcurl with OpenSSL**, the same on every platform, linked statically into `usdcc_assets` (§12).
On Windows `CURLSSLOPT_NATIVE_CA` trusts the Windows certificate store, including company CAs.
One easy handle per thread and host, a share handle for connections, DNS and TLS sessions.
Timeouts: 5 s to connect, 60 s with no data, both configurable. Redirects only on the same host,
unless the protocol says otherwise, as plain HTTP does; credentials never follow one to another host.
Proxies come from the system or the usual environment variables. No Qt Network: resolver calls come
from any thread and from tools without an event loop.

libcurl signs S3 requests itself (`CURLOPT_AWS_SIGV4`), which is most of what an S3 transport would
need. *Not tried.*

**Credentials.** The configuration names a credential id. A provider turns it into a user and
password or a token. Providers are tried in order:

1. **A callback the host registers.** usdcc uses it to show a login dialog after a 401 (§10).
2. **The OS credential store:** Windows Credential Manager (target `usdcc-assets:<id>`), Keychain
   and libsecret later.
3. **Environment variables:** `USDCC_ASSETS_<ID>_USER`, `_PASSWORD`, `_TOKEN`, for CI and tools.

Secrets are never logged, never in a context's hash or debug string, never in a session. A 401 or
403 names the credential id that failed.

## 9. Threads, progress and cancel

- **Any thread.** USD resolves and opens from worker threads. Everything in `usdcc_assets` is
  thread-safe.
- **Progress.** A listener gets "download started, bytes so far, finished" for each file. usdcc
  shows it in the stage opener's progress dialog and the status bar ([PANEL_BUSY.md](PANEL_BUSY.md)).
- **Cancel.** Every transport call takes a token. Cancelling a stage open stops its downloads.
- **Not on the UI thread.** See §10.

## 10. usdcc integration

- **A context per stage.** `StageManager` opens each stage with a context for its environment
  ([stage_manager.cpp](../src/cpp/usd/src/stage_manager.cpp)). The default environment is a
  setting; **Open Stage with Options** can pick another, so two open stages can differ. It has to
  be `UsdStage::Open(path, context)`: *checked,* the `UsdStage::Open(path)` used today resolves and
  opens the root layer before any context is bound, so the root would always come from the default
  environment. The overload with a context binds it for the root layer too.
- **Resolving outside composition.** These call `ArGetResolver().Resolve()` on the UI thread with
  no context bound:
  - [composition_arcs.cpp](../src/cpp/usd/src/composition_arcs.cpp) (resolved paths of arcs)
  - [dependencies.cpp](../src/cpp/usd/src/dependencies.cpp) (Dependencies panel)
  - [property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp) (the asset field's second line)
  - [usda_view.cpp](../src/cpp/usd/src/usda_view.cpp) (following links)

  Each binds the stage's context, and none may wait for a server. A helper answers at once for a
  local path or one the cache knows, and otherwise resolves on a worker and calls back, the field
  showing "Resolving…" meanwhile.
- **Places that assume a local file.** A search for `QFileInfo::exists` finds about a dozen that
  would call a `webdav://` path missing: the session's missing-file check
  ([session_store.cpp](../src/cpp/ui/src/session_store.cpp)), the Reveal buttons (Dependencies,
  Layer Editor, composition arcs, USDA view), the USDA view's file check and
  `layer_actions.cpp`'s path rewriting. They go through one `isLocalFile()` helper; Reveal is off
  for a remote path, and **Copy URL** takes its place.
- **Showing the origin.** The asset field's second line, the Dependencies panel and the Layer
  Editor show the endpoint that delivered a file, for example `Resolved · test ·
  webdav://studio/chars/hero/tex/albedo.png`.
- **Settings > Assets** ([SETTINGS.md](SETTINGS.md)): endpoints (name, protocol, URL, mirrors,
  credential id, read-only), environments and their stacks, the default environment, a **Test**
  button per endpoint, the HTTP hosts that may be contacted and their credentials, the cache's size
  with **Clear cache**, and offline mode. It writes the configuration file of §4. With no protocol
  plugin installed it says so.
- **Login dialog.** usdcc registers the credential callback. On a 401 it asks for user and password
  or a token on the UI thread, and offers to save them to the OS credential store.
- **Host question.** A second callback asks before a stage contacts an HTTP host that isn't on the
  allow list ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §4).
- **Sessions** ([SESSIONS.md](SESSIONS.md)) save each stage's root as its logical URL and its
  environment's name. No secrets.
- **Opening and choosing files:** [ASSET_PICKER.md](ASSET_PICKER.md).
- **Renderers.** Check that the RTX and V-Ray plugins get paths they can read. Anything that
  doesn't read through `ArAsset` (those two, Alembic) needs the cache file, which
  `usdcc::assets::localFile(resolvedPath)` returns after downloading it.
- **Python** ([SCRIPTING.md](SCRIPTING.md)): the context for `Usd.Stage.Open`
  (`ArWrapResolverContextForPython`), and `usdcc.assets` to list and stat.

## 11. A protocol as a plugin

```
webdav/
  usdcc-plugin.json            "usd": ["usd"], no "native", no "python"
  usd/
    plugInfo.json
    webdavResolver.dll
```

- **Registration already happens early enough for schemas.** `plugins.scan()` in
  [main.cpp](../src/cpp/app/main.cpp) registers every plugin's `usd/` folder with
  `PlugRegistry::RegisterPlugins()`, before the window and before any stage
  ([PLUGINS.md](PLUGINS.md) §3.1). *Checked.*
- **Its `plugInfo.json` says three things** beyond its scheme: `"implementsContexts": true`,
  `"implementsScopedCaches": true` (§13.2) and `"usdccAssetsProtocol": true` (§6).
- **A resolver needs it before the first `ArGetResolver()` call,** because OpenUSD collects its URI
  resolvers once. *Checked:* a resolver in a plugin's `usd/` folder, found through `--plugin-path`,
  had its scheme registered and opened the stage given on the command line.
- **The library must match the app's OpenUSD exactly,** as any C++ plugin must. The `built_for`
  check only covers a plugin's native library today. A manifest gains an optional `usd_built_for`
  (platform, compiler, OpenUSD version), and `scan()` doesn't register a `usd/` folder that was
  built for another, saying why in View > Plugins.
- **Turning it off** in View > Plugins works like any plugin: at the next start, its folder isn't
  registered and `webdav://` paths don't resolve.
- **Outside usdcc:** `PXR_PLUGINPATH_NAME=<plugin>/usd`, with usdcc's `bin/` on the library path.
- **Not covered:** the crash marker only protects `activate()`. A resolver library that crashes
  when USD loads it isn't caught.
- **Other resolvers.** A resolver that isn't built on `usdcc_assets` still works as a `usd/`
  folder. It gets no settings page, and browsing only if its plugin registers an asset source
  itself ([ASSET_PICKER.md](ASSET_PICKER.md) §2).

## 12. Build and deploy

- **vcpkg:** `curl` (default features off, `openssl` on) and `pugixml`, for the XML that WebDAV
  and S3 listings come in. OpenSSL is already there through Qt. New licences: curl and pugixml,
  both MIT-style; `tools/deploy.py` collects them.
- **Static curl and OpenSSL** inside `usdcc_assets.dll`. PySide6 ships its own
  `libcrypto-3-x64.dll`, the same file name as vcpkg's, and Windows reuses a DLL already loaded by
  that name.
- **Worktrees:** a new vcpkg port needs a private install root until it is on master, as the
  shared install tree is reset by other checkouts.
- **CMake:** `usdcc_add_asset_protocol()` builds a resolver library into a plugin folder with its
  manifest and `plugInfo.json`, beside `usdcc_add_plugin()`
  ([UsdccPlugin.cmake](../cmake/modules/UsdccPlugin.cmake)).
- **Deploy:** `bin/usdcc_assets.dll`, `plugins/http/`, `plugins/webdav/`, and the headers and
  import library in `sdk`.

## 13. Phases

| Phase | Builds | Done when |
|---|---|---|
| **0. Spike** | A dummy URI resolver in a plugin's `usd/` folder | **Done** (§13.1). It resolves a path given on the command line, and a relative reference inside a layer it delivered is anchored against the resolved path with its stack id |
| **1. `usdcc_assets`** | Configuration, environments, stacks, the context, the cache, `CachingResolver`, credentials from the environment | **Done** (§13.2). Its tests pass with a fake transport and no network: overlay order, unreachable against missing, versions side by side, one download for two threads |
| **2. HTTP, then WebDAV, read-only** | Both transports and plugins. HTTP first ([HTTP_RESOLVER.md](HTTP_RESOLVER.md)): it is the least code that puts `usdcc_assets` on a real network. Then WebDAV ([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md)) | `usdcat http://localhost:<port>/shot.usda` prints a shot with a relative reference, from the test server; `usdcat webdav://studio/shot.usda` prints one whose model comes from PROD and one texture from TEST |
| **3. Robustness** | Mirrors, offline mode, the size limit, `RefreshContext`, progress and cancel | Reopening a large stage makes no downloads, and pulling the network cable falls back to the cache with a warning |
| **4. usdcc** | Everything in §10 but the picker | A stage opened from WebDAV shows correct resolved paths and origins in the Attribute Editor, Dependencies panel and USDA view, renders in Storm, and comes back from a session |
| **5. Asset picker** | [ASSET_PICKER.md](ASSET_PICKER.md) | A reference to a file on the server is added by browsing to it |
| **6. Writing** | `put`, `_CanWriteAssetToPath`, `_OpenAssetForWrite`, Save in the picker | Save on a layer from PROD, in the test environment, writes it to TEST; saving over a file changed meanwhile is a clear error |
| **Later** | An S3 plugin | It needs no change to `usdcc_assets` beyond what S3 itself forces |

### 13.1 What the spike found

A throwaway resolver for `spike://`, 200 lines, with two folders on disk standing in for TEST and
PROD and a log of every call USD made. Run on 2026-11-16 with `usdcat --flatten` from the local
OpenUSD build, and in usdcc's Release build (offscreen, a test profile) with the plugin on
`--plugin-path`, a `spike://` stage on the command line and a script reading the result. The spike
isn't in the repository; phase 1's tests cover the same ground with the fake transport.

| Question | Result |
|---|---|
| Is a resolver in a usdcc plugin's `usd/` folder registered in time? | Yes (§11) |
| Does USD anchor relative paths against the resolved path? | Yes, with the stack id in it |
| Does the overlay work per file? | Yes: the shot from PROD, its model from TEST, the model's sublayer from PROD again |
| Should an anchored identifier keep the stack id? | **No, a change to the plan:** it leaks into flattened files. Identifiers are logical, and the id is only in resolved paths (§3, §5) |
| Is a context bound when a logical identifier is resolved? | Yes, during composition (worker threads included) and when reading asset attributes. **Not for the root layer** with `UsdStage::Open(path)`: usdcc has to pass the context (§10) |
| Can two stages use two environments at once? | Yes, on the same root file too |
| Does `ArGetResolver().Resolve()` with no context bound, as usdcc's UI does today, work? | It answers from the default environment, so those call sites must bind the stage's context (§10) |

**Not checked:** textures loaded by Hydra in a viewport, which needs a real OpenGL window; and
anything about the network, the cache or credentials, which the spike has none of.

### 13.2 Phase 1 as built

[src/cpp/assets](../src/cpp/assets): `usdcc_assets`, a DLL linking only OpenUSD's `ar`, `plug`,
`js`, `vt`, `tf` and `arch`.

| Header | Holds |
|---|---|
| [uri.h](../src/cpp/assets/include/usdcc/assets/uri.h) | Parsing, normalizing and anchoring asset paths (§3) |
| [config.h](../src/cpp/assets/include/usdcc/assets/config.h) | The configuration file, with every problem in it named (§4) |
| [context.h](../src/cpp/assets/include/usdcc/assets/context.h) | `Context`, stacks and their ids, the default configuration (§4, §5) |
| [transport.h](../src/cpp/assets/include/usdcc/assets/transport.h) | `Transport`, `Endpoint`, `Entry`, the registry of transports by scheme (§6) |
| [lookup.h](../src/cpp/assets/include/usdcc/assets/lookup.h) | Finding a file in a stack, listing a folder as the union of one, and what servers answered (§5, §7) |
| [disk_cache.h](../src/cpp/assets/include/usdcc/assets/disk_cache.h) | The disk cache, a file per version (§7) |
| [credentials.h](../src/cpp/assets/include/usdcc/assets/credentials.h) | The host's callback, then environment variables (§8) |
| [caching_resolver.h](../src/cpp/assets/include/usdcc/assets/caching_resolver.h) | `CachingResolver`, the `ArResolver` a protocol derives from |

**Found while building it:**

- **A protocol's `plugInfo.json` must say `"implementsScopedCaches": true`.** Without it OpenUSD
  never tells a URI resolver about a cache scope, and every resolve during composition asks the
  server again. A test caught it: four files of one folder cost six listings, not two.
- **The timestamp** is the file's time in whole seconds with the winning endpoint and the version
  in the fraction, so a file that appears on TEST with PROD's time still reads as changed.
- **A transport is registered for a scheme,** not owned by a resolver. The tests' transport lives in
  the test program, where its switches are, while the resolver for `fake://` is a plugin of its own.

**Not in phase 1,** all planned for later: the HTTP client and any real protocol (phase 2), direct
protocols (§5.1, with HTTP), mirrors, offline mode, the size limit, `RefreshContext`, the
cross-process download lock (phase 3), the OS credential store, and writing. `Progress` and
`Cancel` are in the transport's calls but nothing passes real ones yet.

**Known gaps:**

- **Two threads asking about one folder at once both ask the server.** Answers are shared once
  one arrives, but the requests aren't joined, as downloads are.
- **Only run on Windows.** Nothing in it is Windows-only by intent, but Linux hasn't built it.
- **The app doesn't link it and the deploy doesn't carry it** until phase 4.

## 14. Testing

- **`usdcc_assets`** ([assets_test.cpp](../tests/assets_test.cpp), 18 tests, in a program of its
  own, `usdcc_assets_tests`, with no Qt: the protocol has to be registered before anything asks
  OpenUSD for its resolver). A fake transport over folders on disk, with switches for
  "unreachable", "slow" and "can't list", and the resolver for `fake://` as a real plugin
  ([assets_fake_protocol.cpp](../tests/assets_fake_protocol.cpp)). Identifier normalization and
  anchoring, configuration parsing, context equality and hashing, the overlay rules of §5 through
  `UsdStage::Open`, two stages in two environments, the cache's versions, one download for many
  threads, cache scopes, and that no secret reaches a debug string.
- **WebDAV:** [WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §6.
- **In usdcc:** stages through `UsdStage::Open`, textures opened with no context bound, as Hydra
  does, and the UI call sites with a slow transport, to show the UI doesn't wait.
- **CI** ([CI.md](CI.md)): no external network.

## 15. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | One configuration file, or a studio's file with the user's on top? | Start with one. A studio sets `USDCC_ASSETS_CONFIG`; layering can come when someone needs it |
| 2 | Should an unreachable overlay endpoint fail the resolve, or warn and carry on? | Fail, unless the endpoint is `optional` (§5) |
| 3 | Can an upper endpoint hide a lower one's file? | Not in the first version. A marker file could do it later |
| 4 | How big may a folder be before resolve stops listing it? | Measure in phase 2 against a folder of 10,000 files |
| 5 | Range requests, to read part of a large `.usdc` without downloading all of it? | Later. Whole files first: the usdc reader maps a local file |
| 6 | Which authentication? | Per protocol; see [WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §7 |
| 7 | May a stage contact any HTTP host? | No: an allow list, with a question in usdcc for a new host ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §4, §8) |

## 16. Not planned

- **NVIDIA Nucleus.** Dropped. It has no documented protocol, only NVIDIA's client library.
- **A neutral scheme** (`asset://studio/…`, with the protocol only in the configuration). Rejected:
  the scheme stays the protocol's. The cost is that moving a library from WebDAV to another
  protocol means rewriting the absolute URLs in its layers. Relative references are unaffected.
- **A stack mixing protocols.** Follows from the above.
- **WebDAV `LOCK`,** or any other locking between users.
