# Remote Assets (plan)

Loading USD assets from servers instead of the local disk: a shared library that does the caching,
the server names and the TEST-over-PROD overlays, and one small resolver plugin per protocol on top
of it. The first two protocols are plain HTTP ([HTTP_RESOLVER.md](HTTP_RESOLVER.md)) and WebDAV
([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md)); the asset picker that browses servers is in
[ASSET_PICKER.md](ASSET_PICKER.md).

**Status:** planned 2026-11-16. **Phases 0 to 4 are done:** the spike (§13.1), the shared library
`usdcc_assets` (§13.2), the HTTP and WebDAV protocols with mirrors, offline mode, the cache's
limit, `RefreshContext`, progress and cancel (§13.3), and usdcc itself (§13.4): stages open in an
environment, the panels show servers' paths, sessions keep both, and the deploy ships it all.
Phase 4 left out parts of the plan, each marked *not built* in §10. "Checked" below means run in the spike or the tests,
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
  layers. Plain HTTP is the exception: its URLs are real addresses (§5.1). WebDAV takes an address
  too, when the name isn't configured ([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §2.1).
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
| Asset info | The version is the ETag; the repository path is the logical path (`webdav://studio/a.usdz`), and which endpoint delivered it is the resolver info. *Changed 2026-11-17:* the plan had the server's real URL as the repository path, but Sdf anchors the relative paths inside a package (a `.usdz`'s textures) against it, so they went to the plain HTTP resolver |

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

`webdav://` with an address in place of a name (`webdav://192.168.1.5:5000/a.usd`) is direct in
the same way ([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §2.1).

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
  want one version, it downloads once. Another process wanting it waits: a lock folder beside the
  file, which its holder touches every few seconds, and which is taken over when nobody has for
  half a minute (a crash).
- **A path that can't be a file name** as it is (too long for Windows with the cache's folder in
  front, or with characters Windows refuses) is stored under a hash of it, in a `_` folder.
- **Location:** `USDCC_ASSET_CACHE`, or else `asset-cache` in the user's usdcc data folder. Shared
  by every usdcc process and tool.
- **Size limit:** default 20 GB (`"cacheLimitGB"`), least recently used first, checked after
  every 64 MB downloaded. A version handed out in the last minute stays, as its path may not be
  opened yet; an open one is safe anyway (Windows refuses to remove it, and elsewhere its bytes stay
  until it's closed).
- **Offline mode** (`"offline": true`, or the settings page): resolve and open from the cache
  only, each path as the version of it used last. What isn't cached is missing, and nothing can be
  browsed.

## 8. HTTP and credentials

**libcurl,** with Windows' own TLS (Schannel) there and OpenSSL elsewhere. Schannel trusts the
Windows certificate store, company CAs included, with nothing to configure, and brings no second
OpenSSL into a process that already has PySide6's (§12). One handle per thread, which keeps its
connections between requests.
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
   and libsecret. *Not built yet; with the login dialog, phase 4.*
3. **Environment variables:** `USDCC_ASSETS_<ID>_USER`, `_PASSWORD`, `_TOKEN`, for CI and tools.

A server named by its address (§5.1) has its address as its credential id: `host:port` when a
login is known under that, else `host`. A 401 says whether no login is saved under the id or the
server refused the one that is.

A user and password go as Basic authentication with the first request, a token as a bearer token.
Digest and the Windows schemes aren't built.

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

Built in phase 4 (§13.4) but for what is marked *not built*.

- **A context per stage.** `StageManager::loadUsdStage()` opens every stage with
  `UsdStage::Open(path, context)` ([stage_manager.cpp](../src/cpp/usd/src/stage_manager.cpp)): the
  environment's servers first, then whatever OpenUSD's own resolvers give the asset, so a local
  file's folder still counts. *Checked* in the spike that `UsdStage::Open(path)` alone would
  resolve the root before any context is bound. The default environment is **Settings > Assets >
  Environment**, else the configuration's; **Open Stage with Options** offers the others when
  there is more than one, so two open stages can differ. Helpers in
  [asset_servers.h](../src/cpp/usd/include/usdcc/usd/asset_servers.h).
- **Resolving outside composition.** These bind the stage's context:
  - [composition_arcs.cpp](../src/cpp/usd/src/composition_arcs.cpp) (resolved paths of arcs)
  - [dependencies.cpp](../src/cpp/usd/src/dependencies.cpp) (Dependencies panel)
  - [property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp) (the asset field's
    second line: `EditorHints::resolverContext`)
  - [usda_view.cpp](../src/cpp/usd/src/usda_view.cpp) (following links, and a linked file)

  *Not built:* the plan had them never wait for a server, resolving on a worker meanwhile. They
  resolve on the UI thread, as they did. A server's answers are remembered for a few seconds, and
  the asset field keeps its own until the path changes, so it asks once per field; but a slow or
  dead server holds the UI for up to the connect timeout (5 s) the first time.
- **Paths as shown.** A resolved path on a server carries a stack id (§5); everything the UI shows
  goes through `displayAssetPath()`, without it. `usdcc::usd::displayPath()`
  ([asset_settings.h](../src/cpp/usd/include/usdcc/usd/asset_settings.h)) also keeps
  `webdav://` from becoming `webdav:\\` where the panels show Windows separators: Dependencies,
  Layer Editor, USDA view, the close dialog and Open Recent.
- **Places that assume a local file.** The session's missing-file check and the session dialog
  skip a path with a scheme. The Reveal buttons were already off for a path `QFileInfo` doesn't
  find, which a server's is; Copy Path copies the server's path. The USDA view opens a server's
  texture from its copy in the cache (`usdcc::assets::localFile()`). `layer_actions.cpp` already
  left URIs alone.
- **Showing the origin.** The asset field's second line names the endpoint that delivered the
  file: `Resolved · test · webdav://studio/chars/hero/tex/albedo.png`, and in its tooltip.
  *Not built:* the same in the Dependencies panel and the Layer Editor.
- **Settings > Assets** ([asset_settings.cpp](../src/cpp/usd/src/asset_settings.cpp)): the
  default environment, Work offline, Allowed hosts for plain HTTP, and the cache's size. The
  settings dialog builds its pages from typed settings and has no tables or buttons, so **the
  servers and environments stay in the configuration file** (§4), whose path the page names.
  *Planned:* the Asset Servers editor for them, with a Test button (§10.1). *Not built:* Clear
  cache.
- **Login.** **Edit > Asset Server Login...** saves a credential id's user and password, or a token,
  in Windows Credential Manager (`usdcc-assets:<id>`), which the library reads (§8). A 401 says
  in the Log which credential the server refused. *Changed from the plan:* no dialog pops up on a
  401. The library asks for credentials on whichever thread resolves, which may be one the UI
  thread is waiting for, so a question there could hang the app.
- **Allowed hosts.** For the same reason a stage asking for an HTTP host that isn't allowed
  doesn't get a question: the host is refused, and a toast says how to allow it.
- **Downloads** show in the status bar, with their size, while they run.
- **Sessions** ([SESSIONS.md](SESSIONS.md)) save a stage's root on a server by its logical path,
  and its environment (`"assetEnvironment"`). One the configuration no longer has opens in the
  default. No secrets.
- **Opening:** **File > Open Stage from URL...** takes a typed or pasted `webdav://`, `http://` or
  `https://` path. Browsing is [ASSET_PICKER.md](ASSET_PICKER.md).
- **Renderers.** *Checked:* Storm renders a texture from a server, through the resolver on
  Hydra's own threads with no context bound: a quad textured from TEST in the test environment,
  from PROD in prod (§13.4). *Not checked:* the RTX and V-Ray plugins, and Alembic, which don't
  read through `ArAsset`. `usdcc::assets::localFile()` is there for them.
- **Python** *(not built):* the context for `Usd.Stage.Open` (`ArWrapResolverContextForPython`),
  and `usdcc.assets` to list and stat. A protocol written in Python is §11.1.

### 10.1 The Asset Servers editor

*Planned 2026-11-17, not built.* A dialog for what the configuration file (§4) holds, so nobody
has to edit its JSON by hand: the servers, the environments and their stacks, and the hosts of
plain HTTP. In the picker ([ASSET_PICKER.md](ASSET_PICKER.md)), the roots a protocol lists are the
names an environment has; this is where they're made.

**Opened from** **Edit > Asset Servers...**, a server's context menu in the picker's Places
(**Edit Servers...**), and the description on Settings > Assets. A dialog of its own: the settings
dialog builds its pages from typed values, and has no tables or buttons.

```
+--------------------------------------------------------------------------+
| Asset Servers                     C:/Users/.../usdcc/assets.json        |
+--------------------------------------------------------------------------+
| [Servers] [Environments] [Web hosts]                                     |
|                                                                          |
| Id      Protocol  URL                               Credential  Flags    |
| prod    webdav    https://dav.studio.local/assets   studio-dav  read-only|
| test    webdav    https://dav-test.studio.local/... studio-dav           |
|                                                                          |
| [+] [-]  Mirrors: [ https://dav2.studio.local/assets  ] [+] [-]          |
|          [Test] [Login...]   prod: answered in 84 ms, 12 entries         |
+--------------------------------------------------------------------------+
| Default environment: [ prod v ]                    [Save] [Cancel]       |
+--------------------------------------------------------------------------+
```

**Servers** (the configuration's `"endpoints"`): a table of id, protocol, URL, credential id,
**read-only** and **optional**, with the selected one's mirrors below it.

- **Protocol** is a dropdown of the protocols `usdcc_assets` has a transport for: the named ones
  (`webdav` today, `s3` later), not plain HTTP, which has no servers (§5.1).
- **Test** asks the server for its root folder's listing, on a worker thread, through the same
  transport and credentials a stage would use, and says what came back: answered (with the time
  and how many entries), not allowed (naming the credential, with **Login...** beside it), missing,
  or unreachable (and which of its URLs, mirrors included). Nothing is saved first: it tests what
  the table says.
- **Login...** opens Edit > Asset Server Login on the row's credential id.
- **An id that's in use** by an environment can't be removed without saying where; renaming one
  renames it in the stacks too.

**Environments** (`"environments"`): the environments on the left, and the selected one's names
on the right, each with its stack of servers in order, top first.

- **Adding a name** asks for the name and its first server. Names follow §3's rule (lowercase
  letters, digits, `-`, `_`, `.`), checked as they're typed.
- **A stack is reordered by dragging** its servers: TEST over PROD is TEST dragged above PROD.
  Only servers of the name's protocol can be added, as a stack holds one protocol (§4).
- **Duplicate** makes a new environment from the selected one, which is how a test environment
  usually starts: the production one, with a test server dragged on top.
- **The default environment** (`"environment"`) is the dropdown at the bottom; Settings > Assets
  can still override it for this user.

**Web hosts** (`"http"`): the hosts of plain HTTP, each with its credential id and **insecure**,
and the allow list. The user's own Allowed hosts stay in Settings > Assets; this is the
configuration's, which tools outside usdcc read too.

**Saving:**

- **Checked first,** with `Config::parse()` on what would be written: every problem it names is
  shown at its row, and **Save** stays off until there are none.
- **Written as JSON the way the file has it,** keys in a fixed order, with any key the editor
  doesn't know kept as it was, so a newer usdcc's settings survive an older one's save.
- **Changed on disk meanwhile?** If the file changed since the dialog read it (someone else, or a
  text editor), saving asks first, offering to reload it instead.
- **Applies at once,** as `RefreshContext` does: the configuration is read again and what servers
  answered is forgotten. **Open stages keep the servers they were opened with** (their context
  holds the stacks); the dialog says how many are open in an environment that changed, and
  reopening them uses the new servers.
- **No secrets** are ever in the file or the dialog's tables: a credential is an id, and its
  password lives in the system's store (§8).

**A studio's file is read-only here.** When the configuration comes from `USDCC_ASSETS_CONFIG` and
can't be written, or the user can't write it, the dialog shows it, with Test and Login working, and
says where it comes from. **Save a copy as mine** writes it to the user's own `assets.json` and
uses that from then on (the studio's variable still wins when set, so the dialog says that too).
Layering a user's file over a studio's is open question 1.

**Not in it:** the user's own Settings > Assets (environment override, offline, allowed hosts, cache
size), which stay settings; and anything about one stage, which is Open Stage with Options.

**Testing** (`tests/asset_servers_editor_test.cpp`): the model round-trips a configuration file
unchanged, unknown keys included; adding, renaming and removing a server updates the stacks or is
refused; a stack reordered by dragging is written in that order; Duplicate; each problem
`Config::parse()` names shown at its row with Save off; a file changed on disk is noticed; a
read-only one opens read-only; Test against the tests' own WebDAV server answering, refusing a
missing login (naming the credential) and unreachable; and saving reloads the library's
configuration, an open stage keeping its servers.

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

### 11.1 A protocol in Python

*Planned 2026-11-17, nothing built.* A studio's own asset system (a database, a tracking tool's
API) is easiest to reach from Python. Such a protocol is a transport (§6) written in Python. It
is not an `ArResolver` written in Python.

- **Python sits behind `CachingResolver`.** A C++ `PythonTransport` in `usdcc_scripting`
  implements `stat`, `fetch`, `list` and `put` by taking the GIL and calling a Python object. A
  Python plugin registers one for its scheme: `usdcc.assets.register_transport("tracker",
  TrackerTransport())`, through `registerTransport()`, which already replaces a scheme's transport.
  The resolver for the scheme is still a small C++ library in the plugin's `usd/` folder (§11), as
  OpenUSD needs a registered type for every scheme.
- **What runs most often stays in C++.** `_CreateIdentifier` and `_IsContextDependentPath` are
  string work, called for every path. Answers are remembered in cache scopes and for a few seconds
  outside them (§7). `_OpenAsset` hands out the file `fetch` wrote to the cache, so `ArAsset::Read`,
  called most often and from many threads at once, never runs Python. Python runs only when the
  cache doesn't know the answer: where a file is, and fetching it.
- **The GIL makes Python calls take turns.** USD resolves from TBB workers during composition and
  from Hydra's texture threads. Only one of them runs Python at a time. Waiting on the network
  (`urllib`, `requests`, sockets) releases the GIL, so downloads still overlap; parsing a large
  answer in Python doesn't. A long script in the console holds the GIL and stalls stage loading and
  texture loading until it ends.
- **The danger is a deadlock.** A thread that holds the GIL and waits for a USD worker hangs when
  that worker needs the GIL. Example: a script calls `Usd.Stage.Open()`, `layer.Reload()` or
  `prim.Load()` while holding the GIL. Composition hands work to TBB workers and waits for them. A
  worker misses the cache, enters `PythonTransport` and waits for the GIL forever. TBB lets the
  waiting thread run queued tasks itself, which can take the GIL again on that thread, but that
  doesn't help: tasks a worker has already started never finish. *Not checked:* whether OpenUSD's
  Python wrappers release the GIL around these calls; the plan assumes they don't. Stages opened from
  C++ are safe, because the main thread holds the GIL only while it runs Python
  ([python_runtime.cpp](../src/cpp/scripting/src/python_runtime.cpp)). Scripts, scripted panels
  and scripted tools that load USD content are the exposed paths.
- **How the deadlock is avoided,** cheapest first:
  1. usdcc's own bindings (`usdcc.assets`, `usdcc.app`, any helper that opens or reloads a stage)
     release the GIL around calls that can resolve.
  2. A Python transport never calls Sdf or Usd. Opening a layer inside a resolve goes back into the
     resolver and starts parallel work while the GIL is held.
  3. `PythonTransport` gives up waiting for the GIL after a timeout (configurable, default 30 s) and
     reports the endpoint as unreachable ("the Python transport is busy"). A hang becomes a failed
     resolve, which says so in the Log.
  4. If one is needed, the transport runs in a separate Python process that C++ talks to over a pipe.
     Nothing waits on the GIL, calls really run in parallel, and a crashing script can't end usdcc.
     It costs a process per scheme and a small protocol over the pipe. This is the choice for a
     transport a studio uses in production; 1 to 3 are enough to try one out.
- **Before Python, and after it.** OpenUSD can make the resolver, and `loadProtocolPlugins()` can
  load it, before Python is up. Until a transport is registered the scheme answers "no transport",
  not missing. Stages can be destroyed while Python finalizes or after it, so `PythonTransport`
  checks that the interpreter is still alive before taking the GIL, and fails the call if it isn't.
- **Errors.** Every Python exception is caught in `PythonTransport`, logged with its traceback
  through `usdcc.log`, and turned into a `Result` error. Nothing may escape into a TBB task, as that
  ends the process. Returning `None` from `stat` means missing; raising means unreachable (§5).
- **Cancel and progress** are passed to Python as objects with `cancelled()` and
  `advanced(done, total)`. A transport that never checks `cancelled()` still works, but cancelling
  a stage open then waits for its fetch to finish.
- **Not a way around the GIL:** Python 3.13's free-threaded build. OpenUSD's Python modules and
  pybind11 extensions aren't ready for it.

## 12. Build and deploy

- **vcpkg:** `curl` (default features off, `ssl` on: Schannel on Windows, OpenSSL elsewhere) and
  `pugixml`, for the XML that WebDAV listings come in. New licences: curl and pugixml, both
  MIT-style; `tools/deploy.py` collects them.
- **No static linking, which the plan had.** It was to keep a second `libcrypto-3-x64.dll` out of a
  process with PySide6's. With Schannel there is no OpenSSL in libcurl on Windows at all, and making
  OpenSSL static in vcpkg would have rebuilt Qt. `libcurl.dll` and `z.dll` go beside
  `usdcc_assets.dll`, and `pugixml.dll` where the WebDAV plugin finds it (phase 4, with the deploy).
- **Worktrees:** a new vcpkg port needs a private install root until it is on master, as the
  shared install tree is reset by other checkouts.
- **CMake:** `usdcc_add_asset_protocol()` builds a resolver library into a plugin folder with its
  manifest and `plugInfo.json`, beside `usdcc_add_plugin()`
  ([UsdccPlugin.cmake](../cmake/modules/UsdccPlugin.cmake)).
- **Deploy:** `bin/usdcc_assets.dll` with `libcurl.dll`, `z.dll` and `pugixml.dll` beside it (not
  beside a plugin: §13.4), `plugins/http/`, `plugins/webdav/`, and the headers and import library
  in `sdk`. *Done.*

## 13. Phases

| Phase | Builds | Done when |
|---|---|---|
| **0. Spike** | A dummy URI resolver in a plugin's `usd/` folder | **Done** (§13.1). It resolves a path given on the command line, and a relative reference inside a layer it delivered is anchored against the resolved path with its stack id |
| **1. `usdcc_assets`** | Configuration, environments, stacks, the context, the cache, `CachingResolver`, credentials from the environment | **Done** (§13.2). Its tests pass with a fake transport and no network: overlay order, unreachable against missing, versions side by side, one download for two threads |
| **2. HTTP, then WebDAV, read-only** | Both transports and plugins. HTTP first ([HTTP_RESOLVER.md](HTTP_RESOLVER.md)): it is the least code that puts `usdcc_assets` on a real network. Then WebDAV ([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md)) | **Done** (§13.3). `usdcat http://localhost:<port>/shot.usda` prints a shot with a relative reference, from the test server; `usdcat webdav://studio/shot.usda` prints one whose model comes from PROD and one texture from TEST |
| **3. Robustness** | Mirrors, offline mode, the size limit, `RefreshContext`, progress and cancel | **Done** (§13.3), but for one thing: offline is a switch, and a network that goes away does not turn it on. Reopening a stage makes no downloads |
| **4. usdcc** | Everything in §10 but the picker | **Done** (§13.4), with gaps marked in §10. A stage opened from WebDAV shows correct resolved paths and origins in the Attribute Editor, Dependencies panel and USDA view, renders in Storm, and comes back from a session |
| **5. Asset picker** | [ASSET_PICKER.md](ASSET_PICKER.md), with favourites and the scheme dropdown | **Done** (ASSET_PICKER.md §9), but for dragging, completion and Python. A reference to a file on the server is added by browsing to it |
| **5.1 Asset Servers editor** | §10.1 | A test environment is made from production in the dialog (Duplicate, a test server dragged on top), Test answers for both servers, and a stage opened in it takes a file from TEST, with no JSON edited by hand |
| **6. Writing** | `put`, `_CanWriteAssetToPath`, `_OpenAssetForWrite`, Save in the picker | Save on a layer from PROD, in the test environment, writes it to TEST; saving over a file changed meanwhile is a clear error |
| **Later** | An S3 plugin | It needs no change to `usdcc_assets` beyond what S3 itself forces |
| **Later** | More storage backends through Apache OpenDAL (§13.5): one plugin, one transport per scheme | A stage opens from a second backend (Azure Blob or SFTP, say) through the same plugin, and the deploy ships the full notice list for its Rust dependencies |
| **Later** | A transport in Python (§11.1): `PythonTransport`, `usdcc.assets.register_transport()`, the GIL timeout | A Python transport over a folder serves a stage in the tests; a script opening that stage while holding the GIL fails the resolve after the timeout instead of hanging |

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
- **Only run on Windows.** On Linux it builds (CI, GCC 13), but its tests haven't run there (§13.3).
- **The app doesn't link it and the deploy doesn't carry it** until phase 4.

### 13.3 Phases 2 and 3 as built

| Part | Where |
|---|---|
| The HTTP client, over libcurl | [http_client.h](../src/cpp/assets/include/usdcc/assets/http_client.h) |
| Downloads as the host sees them, cancelling, offline mode, the HTTP host list | [activity.h](../src/cpp/assets/include/usdcc/assets/activity.h) |
| Mirrors, in `withMirrors()` | [lookup.h](../src/cpp/assets/include/usdcc/assets/lookup.h) |
| The cache's limit, lock and "any version" lookup | [disk_cache.h](../src/cpp/assets/include/usdcc/assets/disk_cache.h) |
| Direct protocols, through `CachingResolver::_Locate()` | [caching_resolver.h](../src/cpp/assets/include/usdcc/assets/caching_resolver.h) |
| The HTTP protocol | [src/plugins/http](../src/plugins/http) |
| The WebDAV protocol | [src/plugins/webdav](../src/plugins/webdav) |
| `usdcc_add_asset_protocol()` | [UsdccPlugin.cmake](../cmake/modules/UsdccPlugin.cmake) |

Both protocols build into `build/plugins/http` and `build/plugins/webdav`, each a usdcc plugin
with only a `usd/` folder.

**Checked by hand, with `usdcat --flatten` and no usdcc** (2026-11-17):

- **`http://`** against Python's own `http.server`: a shot, its reference and a file with a space
  in its name. The second run sent three `HEAD`s and no `GET`. With `USDCC_HTTP_ALLOW` unset the
  host was refused, by name.
- **`webdav://`** against two `rclone serve webdav` servers (rclone 1.72), PROD behind a password
  and TEST without: in the test environment the model came from TEST and its sublayer from PROD,
  in prod everything from PROD; a second run downloaded nothing; a wrong password gave "the server
  'prod' is not allowed: HTTP 401".

**Differs from the plan:**

- **TLS and linking,** as §8 and §12 now say.
- **`RefreshContext` reloads the configuration** and forgets what servers answered, then sends
  `ArNotice::ResolverChanged`. A context a stage was opened with keeps the stacks it was made
  with, so a changed environment still means reopening the stage (§5).
- **A mirror is tried when the endpoint's own URL doesn't answer,** and a URL that didn't is tried
  after the others for half a minute. An endpoint with no mirror is asked again each time.
- **The tests' servers are inside the test program** (§14), not WsgiDAV and `http.server` started
  beside it: no Python, no install, and switches for what real servers do wrong.

**Not checked, or not built:**

- **`https://`.** Every test and both checks by hand used `http://` on this machine. The TLS path
  (Schannel, certificates, a company CA) has never run.
- **Other WebDAV servers:** Apache `mod_dav`, nginx, Nextcloud, WsgiDAV. Their replies differ in
  ways the parser is written for but hasn't met.
- **Linux.** Phase 1 compiles and links there, in CI. Nothing of it has run: CI's Linux build
  stops at a link error in the physics tests (`typeinfo for JPH::GroupFilter`), which is not this
  work's and has failed every build since 2026-11-16. Phases 2 and 3, where the HTTP client uses
  OpenSSL instead of Schannel, haven't been built there yet.
- **A folder too big to list** still gets listed: the fallback to `stat` per file (§7) is only
  taken for a protocol that can't list.
- **Two threads asking about one folder at once** both ask the server, as in phase 1.
- **`usd_built_for`** (§11): a protocol built for another OpenUSD is registered and then fails to
  load, with OpenUSD's message.
- **Redirects on a WebDAV server** aren't followed, not even on the same host (§8).

### 13.4 Phase 4 as built

What §10 describes, in [asset_servers.h](../src/cpp/usd/include/usdcc/usd/asset_servers.h)
(Qt-free, in `usdcc_usd`) and [asset_settings.h](../src/cpp/usd/include/usdcc/usd/asset_settings.h)
(in `usdcc_usd_ui`), with `usdcc_usd` now linking `usdcc_assets`. Beside it:

- **The protocols load as shipped plugins.** `usdcc_add_asset_protocol()` copies each beside the
  app, where `PluginManager::shippedFolder()` looks, as the physics plugin is; the deploy ships them
  in `plugins/http` and `plugins/webdav`.
- **Their libraries go in `bin`.** OpenUSD loads a plugin's library with plain `LoadLibraryW`,
  which doesn't look beside the plugin: `pugixml.dll` there wasn't found. So `usdcc_assets.dll`,
  `libcurl.dll`, `zlib`'s `z.dll` and `pugixml.dll` sit beside every program that loads
  `usdcc_usd`, and in the deploy's `bin`. A tool outside usdcc (`usdcat`, `usdview`, a script)
  needs usdcc's `bin` on `PATH`.
- **The SDK** has `usdcc/assets` headers and `usdcc_assets.lib`, and
  `usdcc_add_asset_protocol()` builds against it.
- **Credentials** in Windows Credential Manager (`storedCredential()`, `storeCredential()`), and
  `localFile()` for a resolved path's downloaded copy, in the library.

**Checked, 2026-11-17:**

- **In the app** (Release build, offscreen, a test profile), with two `rclone serve webdav`
  servers, PROD behind a password from environment variables: a `webdav://` stage given on the
  command line opened in the test environment, the model from TEST and its sublayer from PROD; the
  Attribute Editor's asset lines read `Resolved · test · webdav://studio/chars/hero.usda` and
  `… wood grain.txt`; the Dependencies panel listed `webdav://studio/chars/geo.usda`; with
  Settings > Assets > Environment set to prod, the same root opened again with PROD's model; a
  session saved both, closed them, and loaded them back, each in its own environment.
- **The same in a deploy** (`tools/deploy.py`), run with only Windows' own folders on `PATH`.
- **Storm,** outside the app: OpenUSD's `FrameRecorder` with Storm on a textured quad on the
  servers. The texture came from TEST in the test environment and from PROD in prod, read through
  the resolver on Hydra's threads.
- **Windows Credential Manager:** store, read back, replace with a token, forget, in
  `usdcc_assets_tests` (32 tests, 225 checks).
- **Every test program** of the build passes.

**Not checked:** the app's own viewport drawing a server's texture (offscreen has no OpenGL; the
Storm check above was outside it); the login dialog, Open Stage from URL, Open Stage with Options'
environment choice, the status bar and the host toast, which were built but not clicked; `https://`;
Linux; and a 401 in the app.

### 13.5 Later: more storage backends through OpenDAL

*Not tried; written down 2026-11-18 so it isn't lost.* There are far more network and cloud stores
than we want to write transports for. **Apache OpenDAL** puts about 50 behind one API: S3 and the
S3-compatible stores (MinIO, Ceph, R2, Wasabi), GCS, Azure Blob/Data Lake/Files, HDFS, SFTP, FTP,
WebDAV, HTTP, Swift, Aliyun OSS, Tencent COS, Huawei OBS, B2, Dropbox, Google Drive, OneDrive.

- **It fits the transport (§6) almost call for call.** `stat` gives size, modified time and ETag
  (an `Entry`); `list`; `read` with `if_none_match` for `fetch`'s `knownEtag`; `write` with
  `if_match` / `if_not_exists` for `put`. Not every backend has ETags or conditional writes;
  `versionKey()` already falls back to time and size.
- **One plugin, many schemes.** It registers a transport for each scheme it is built with
  (`s3://`, `gcs://`, `azblob://`, `sftp://` …) and turns an `Endpoint` and its credential into
  OpenDAL's key/value options for that service. Names, stacks, the cache, offline mode and the
  picker work unchanged. HTTP and WebDAV stay ours: they have mirrors and addresses (§5.1).
- **S3 first may not need it.** libcurl's SigV4 (§8) covers S3 and every S3-compatible store with
  no new dependency. OpenDAL is for when Azure, SFTP or consumer clouds are wanted too.
- **Build.** The core is Rust: the build needs a Rust toolchain, and OpenDAL needs its own overlay
  port (it isn't in vcpkg as far as we know). We'd use its C binding or the newer C++ one, both less
  mature than the Rust API: check first that they build on Windows and expose the ETag and
  conditional options. Build only the services wanted (Cargo features); that also keeps the
  dependency tree small. TLS: no second OpenSSL in the process (§12).
- **Licence.** OpenDAL is Apache-2.0: fine to ship with usdcc's BSD-3 code, closed or open. We must
  ship its licence text and the contents of its `NOTICE`, mark any files we patch, and not use
  its trademarks to suggest endorsement.
- **Its Rust dependencies are the real work.** Every crate is linked into the one DLL, so we
  distribute dozens to a few hundred of them, depending on the services. Most are MIT, Apache-2.0,
  BSD, ISC, Zlib or Unicode and only want their notices shipped. A few have unusual licences: the TLS
  stack's (CDLA-Permissive-2.0 root certificates, `ring`'s ISC/OpenSSL-style terms), and possibly
  an MPL-2.0 crate, whose source we must point to (the upstream crate, if unchanged).
  `cargo about generate` writes the notice file for the exact build; `cargo deny check licenses`
  with an allow list fails the build when a GPL, LGPL or AGPL crate comes in. The deploy puts
  `LICENSE`, `NOTICE` and that output in `licenses/opendal/`, and inside the plugin's own package
  if it is ever shipped alone.
- **The services' own terms.** The licence covers the code, not the services. Google Drive, Dropbox
  and OneDrive need an OAuth app registered with them under their developer terms; S3, Azure, GCS
  and SFTP take the user's own keys.
- **Not chosen instead:** rclone (about 70 providers, but a Go runtime in the process and its own
  credential system; better as a mount the default resolver reads); Arrow's `arrow::fs` (only S3,
  GCS, Azure and HDFS, and all of Arrow); one vendor SDK per provider (what this is meant to avoid).

## 14. Testing

- **`usdcc_assets`** ([assets_test.cpp](../tests/assets_test.cpp), 18 tests, in a program of its
  own, `usdcc_assets_tests`, with no Qt: the protocol has to be registered before anything asks
  OpenUSD for its resolver). A fake transport over folders on disk, with switches for
  "unreachable", "slow" and "can't list", and the resolver for `fake://` as a real plugin
  ([assets_fake_protocol.cpp](../tests/assets_fake_protocol.cpp)). Identifier normalization and
  anchoring, configuration parsing, context equality and hashing, the overlay rules of §5 through
  `UsdStage::Open`, two stages in two environments, the cache's versions, one download for many
  threads, cache scopes, and that no secret reaches a debug string.
- **The protocols and phase 3** ([assets_net_test.cpp](../tests/assets_net_test.cpp), 13 tests in
  the same program): a small HTTP server inside the test program
  ([assets_test_server.h](../tests/assets_test_server.h)), on 127.0.0.1 at a free port, serving a
  folder as a web server and as a WebDAV server, with switches: no ETags, no `HEAD`, a 503, a
  delay, a password, redirects. Stages through `UsdStage::Open` over both protocols, the host
  list, credentials and where they aren't sent, mirrors, offline mode, the cache's limit and lock,
  `RefreshContext`, progress and cancel. See [HTTP_RESOLVER.md](HTTP_RESOLVER.md) §7 and
  [WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §6.
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
| 7 | May a stage contact any HTTP host? | No: an allow list. A host the user opens a stage on is allowed for the session; any other refused, with a toast saying how to allow it ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §4) |
| 8 | A Python transport in usdcc's interpreter, or in a process of its own? | In usdcc's to try one out, with the GIL timeout; its own process for production (§11.1) |
| 9 | Should the Asset Servers editor write the studio's file when the user can? | Only when it's the file in use and writable. A studio that wants it locked makes it read-only; the dialog then offers a copy of the user's own (§10.1) |
| 10 | Should Test run when the dialog opens, for every server? | No: on request. A server that's down would slow the dialog's opening, as an unchecked favourite in the picker doesn't |

## 16. Not planned

- **NVIDIA Nucleus.** Dropped. It has no documented protocol, only NVIDIA's client library.
- **A neutral scheme** (`asset://studio/…`, with the protocol only in the configuration). Rejected:
  the scheme stays the protocol's. The cost is that moving a library from WebDAV to another
  protocol means rewriting the absolute URLs in its layers. Relative references are unaffected.
- **A stack mixing protocols.** Follows from the above.
- **WebDAV `LOCK`,** or any other locking between users.
