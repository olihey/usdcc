# WebDAV Resolver (plan)

A USD asset resolver plugin that loads assets from WebDAV servers through `webdav://` asset
paths. The context says which server each name means and which credentials go with it. A
stage can mix several servers, and local files keep going through USD's default resolver.

**Status:** planned. Nothing is built yet.

It is a standalone USD plugin (a `plugInfo.json` with an `ArResolver` type), not a usdcc
plugin. That way `usdcat`, `usdview` and scripts outside usdcc can open the same files, as long as
`PXR_PLUGINPATH_NAME` includes it. usdcc ships it, binds a context for each stage, and gives it a
settings page (§7).

## 1. Decisions

- **The server is named in the URL's authority:** `webdav://<server>/<path>`. `<server>` is a
  name such as `assets` or `lib`, not a hostname. The context maps each name to one or more endpoints
  (§3). Files stay portable: no hostnames or secrets are written into layers, and another site
  or a mirror only needs a different context.
- **One server name can have several endpoints**, tried in order. This covers failover, mirrors
  and an override server in front of the main one.
- **A URI resolver, not the primary resolver.** It's registered with `"uriSchemes": ["webdav"]`,
  so it sits beside `ArDefaultResolver` instead of replacing it. Local paths behave exactly as
  they do today, and another studio's primary resolver can be used alongside it.
- **The resolved path stays a `webdav://` URI.** It is never the local cache file (§4). USD
  anchors relative references inside a layer against its resolved path, so a cache path would
  hand siblings to the default resolver and they would fail.
- **Credentials are referenced by the context, not stored in it.** Contexts are hashed, compared,
  printed by `GetDebugString()` and saved in usdcc sessions. The context holds a credential id,
  and a credential provider turns it into a secret (§5). Inline secrets are allowed for tests only.
- **HTTP through libcurl, using OpenSSL on every platform.** The same code and the same TLS
  library everywhere. On Windows, `CURLSSLOPT_NATIVE_CA` makes OpenSSL trust the Windows certificate store,
  including company CAs, so there's no CA bundle to maintain. No Qt in the plugin: Qt Network
  needs an event loop, and resolver calls come from any thread and from tools without Qt.
- **Read-only first.** Writing (saving a layer to WebDAV) is the last phase (§9).

### Rejected alternatives for multiple servers

| Option | Why not |
|---|---|
| A scheme per server (`webdav-lib://`) | `uriSchemes` is read from `plugInfo.json` when USD starts; every new server would need a new build |
| The real hostname in the URL (`webdav://dav.studio.local/…`) | Bakes infrastructure into files; breaks when moving between sites |
| One server per context, one context per stage | A stage couldn't reference assets from two servers |
| Searching every server for every path | Ambiguous, and every miss costs a round trip per server |

## 2. Asset paths

```
webdav://assets/chars/hero/hero.usd     server "assets", path /chars/hero/hero.usd
webdav:///chars/hero/hero.usd           empty authority: the context's default server
chars/hero/tex/albedo.png               relative, anchored against the layer that contains it
```

- **Identifiers** (`CreateIdentifier`) are normalized: the scheme and server name are lowercased, `.` and
  `..` are collapsed, slashes are deduplicated and percent-encoding is made canonical. Two spellings of the same asset
  give the same identifier, as Sdf's layer registry needs.
- **Relative paths** anchor against a `webdav://` anchor and never escape above the server's root.
  A `..` past the root is an error, not a different server.
- **`_GetExtension`** is overridden, so the extension comes from the path and never from a query
  string.
- **Context-dependent paths:** `_IsContextDependentPath` is `true` for `webdav://<server>/…`
  and `webdav:///…`, because the same name can point to different endpoints in different contexts.
  Without this, Sdf would treat both as the same layer.

## 3. The context

```cpp
struct WebDavEndpoint {
    std::string baseUrl;        // https://dav.studio.local/assets
    std::string credentialId;   // "studio-dav"; empty for anonymous
};

class WebDavResolverContext {   // AR_DECLARE_RESOLVER_CONTEXT
public:
    std::map<std::string, std::vector<WebDavEndpoint>> servers;   // name → endpoints, in order
    std::string defaultServer;
    // operator==, operator<, hash_value; GetDebugString() prints names and URLs, never secrets
};
```

There are three ways to make one, all of which give the same context:

1. **Explicitly**, from C++ or Python: `UsdStage::Open(path, ArResolverContext(ctx))`. This is how
   usdcc does it (§7).
2. **From a string**, for scripts, tests and the command line. Via `ArGetResolver().CreateContextFromString("webdav",
   "assets=https://dav.studio.local/assets@studio-dav;lib=https://lib.example.com/dav;default=assets")`.
   `@<id>` names the credential, and repeating a name adds a fallback endpoint.
3. **By default**, when nothing else is given (`_CreateDefaultContext`/`_CreateDefaultContextForAsset`):
   read from `USDCC_WEBDAV_CONFIG`, or else `~/.usdcc/webdav.json`:

   ```json
   {
     "default": "assets",
     "servers": {
       "assets": [{ "url": "https://dav.studio.local/assets", "credential": "studio-dav" }],
       "lib": [
         { "url": "https://lib.example.com/dav/usd", "credential": "lib-token" },
         { "url": "https://lib-mirror.example.com/dav", "credential": "lib-token" }
       ]
     }
   }
   ```

   This is what makes `usdcat webdav://assets/shot.usd` work outside usdcc.

`_RefreshContext` reloads the config file and drops cached resolves for that context, so changing
a server doesn't need a restart.

## 4. Resolving and opening

**`_Resolve`** finds the server's endpoints in the bound context. For each endpoint in order, it
sends `PROPFIND Depth: 0` (or `HEAD`), and the first one that answers 2xx wins. 404 moves on to the next endpoint. A network
error moves on too, and the endpoint is marked down for a short time (§6). If none has the file, the
result is an empty `ArResolvedPath`, as for a missing local file.

**Resolved paths name the concrete endpoint:** `webdav://lib~3f9a/tex/wood.png`, where `3f9a` is
a stable id (a short hash of the endpoint's base URL and credential id). Hydra loads textures on
worker threads with no context bound, so opening a file has to work from the resolved path
alone. A process-wide **endpoint registry** maps the id to its base URL and credential id. It's
filled whenever a context is created, so it works with no context bound. Concrete paths are not context-dependent.

The consequence is that relative references inside a layer anchor against the resolved path, so they
resolve on the endpoint that delivered the layer. Sibling assets come from the same server as
their layer, but a fallback endpoint can't override a single sibling file.

**`_OpenAsset`** downloads the file with `GET` into the disk cache (§6) and returns an
`ArFilesystemAsset` on the cached file. A local file can be memory-mapped by the usdc reader, and it
gives renderers that need a real path one to use (the RTX and V-Ray plugins, §7).

**`_GetModificationTimestamp`** comes from `getlastmodified` (with `getetag` kept alongside) in
the `PROPFIND` reply, so `SdfLayer::Reload` and usdcc's "file changed" checks see server-side
changes.

**`_GetAssetInfo`** fills in the version from the ETag and the repository path from the
logical URI.

## 5. Credentials

The context carries a credential id. A **credential provider** turns it into what libcurl
needs (user and password for Basic/Digest, or a bearer token). Providers are tried in order:

1. **A callback the host registers**: `WebDavResolver::SetCredentialProvider(fn)`. usdcc uses this
   to show a login dialog after a 401 and remember the answer (§7).
2. **The OS credential store**: Windows Credential Manager (`CredReadW`, target
   `usdcc-webdav:<id>`), then Keychain or libsecret on other platforms later. This is the only
   OS-specific code, and it's small and isolated.
3. **Environment variables:** `USDCC_WEBDAV_<ID>_USER`, `_PASSWORD` or `_TOKEN`, for CI and tools.

Secrets are never logged, never written to sessions and never part of the context's hash or
debug string. A 401 or 403 reports which credential id failed, never the secret.

## 6. HTTP, caching and threads

**libcurl.** There's one easy handle per thread (`thread_local`) for each endpoint host, and a share handle
for connections, DNS and TLS sessions, so composing a stage reuses connections. `curl_global_init`
runs once, under `std::call_once`, when the plugin loads. Timeouts are 5 s to connect and 60 s
with no data, adjustable in the config. Redirects are followed only on the same host.

**Disk cache:** `<AppLocalData>/webdav-cache/<endpoint id>/<path>`, with an index of the ETag and
last-modified time for each file. Before a cached file is used, a conditional `GET` (`If-None-Match`)
revalidates it, and a 304 costs no download. Downloads go to a temporary file that is renamed when complete. Two
threads (or two usdcc processes) fetching the same file wait on a lock file, so the file is downloaded once.

**Scoped cache:** `_BeginCacheScope`/`_EndCacheScope` with `ArThreadLocalScopedCache` remember
resolves while a stage opens or recomposes, so composition asks the server about each path only
once. Outside a scope, each resolve is remembered for a few seconds (configurable).

**Offline mode** (config `"offline": true`, or automatically when every endpoint of a server is
down): resolve and open from the cache only, with a warning in the Log. Files that aren't cached are
reported as missing.

**Errors** go to `TF_WARN` with the server name, path and HTTP status, never credentials.
usdcc shows them in its Log ([LOGGING.md](LOGGING.md)).

## 7. usdcc integration

- **Binding the context.** `StageManager` opens each stage with the usdcc context (from settings)
  and keeps it as the stage's resolver context ([stage_manager.cpp](../src/cpp/usd/src/stage_manager.cpp)).
- **Resolving outside composition.** These call `ArGetResolver().Resolve()` with no context bound,
  so `webdav://` paths would show as unresolved. Each one gets an
  `ArResolverContextBinder(stage->GetPathResolverContext())`:
  - [composition_arcs.cpp](../src/cpp/usd/src/composition_arcs.cpp) (resolved paths of arcs)
  - [dependencies.cpp](../src/cpp/usd/src/dependencies.cpp) (Dependencies panel)
  - [property_editors.cpp](../src/cpp/usd/src/properties/property_editors.cpp) (asset path field's second line)
  - [usda_view.cpp](../src/cpp/usd/src/usda_view.cpp) (following links)
- **Settings page** ([SETTINGS.md](SETTINGS.md)): the server table (name, endpoints in order,
  credential id), the default server, a **Test** button for each endpoint, cache size with **Clear
  cache**, and offline mode. It writes the same JSON as §3, so tools outside usdcc see the same
  servers.
- **Login dialog.** usdcc registers the credential provider callback. On a 401 it asks for user and
  password (or token) on the UI thread, and offers to save them to Windows Credential Manager.
- **Sessions** ([SESSIONS.md](SESSIONS.md)) save each stage's context as its string form (§3),
  without secrets.
- **Opening files.** File > Open accepts a typed `webdav://` URL. Browsing a server (a `PROPFIND
  Depth: 1` dialog) is a later addition, not needed for phase 3.
- **Renderers.** Check that the RTX and V-Ray plugins get file paths they can read. If they
  resolve asset paths themselves, they may need the cached file's path, which the resolver can
  expose (`WebDavResolver::GetLocalPath(resolvedPath)`).

## 8. Build and deploy

- **vcpkg:** `{ "name": "curl", "default-features": false, "features": ["openssl"] }` in
  `vcpkg.json`. Default features off drops FTP, SMTP and the other non-HTTP protocols. OpenSSL is
  already installed by vcpkg and already in the release's licences ([RELEASE.md](RELEASE.md) §6), so
  curl's licence (`curl`, MIT-style) is the only new one. `tools/deploy.ps1` collects
  `licenses/vcpkg/curl.txt` automatically.
- **Source:** `src/usd_plugins/webdav_resolver/`, a `webdavResolver` shared library and its
  `resources/plugInfo.json`:

  ```json
  { "Plugins": [{
      "Type": "library", "Name": "webdavResolver", "LibraryPath": "../../webdavResolver.dll",
      "Root": "..", "ResourcePath": "resources",
      "Info": { "Types": { "WebDavResolver": {
          "bases": ["ArResolver"], "uriSchemes": ["webdav"], "implementsContexts": true
      } } }
  }] }
  ```

- **Deploy:** `bin/usd/webdavResolver/resources/plugInfo.json`, which the existing
  `bin/usd/plugInfo.json` (`"*/resources/"`) already picks up. Add it to deploy.ps1's required files.
- **Python:** `ArWrapResolverContextForPython<WebDavResolverContext>` and a small module, so
  scripts can make a context and pass it to `Usd.Stage.Open` ([SCRIPTING.md](SCRIPTING.md)).

## 9. Phases

1. **Read-only resolver, standalone.** Identifiers, the context (string and JSON), `Resolve` with
   endpoint order, `OpenAsset` through the disk cache, timestamps, endpoint registry, environment
   variable credentials. Tests run against a local WebDAV server started by the test fixture (§10).
   Done when `usdcat webdav://assets/shot.usda` prints a shot that references a model and a texture
   on two servers, with relative references inside the model resolving on its own server. **The first
   test checks the anchoring assumption in §1.**
2. **Performance and robustness.** The scoped cache, ETag revalidation, cross-process download locks,
   endpoint down-marking with failover, offline mode, and `RefreshContext`. Done when reopening a
   large stage makes only 304s, and pulling the network cable falls back to the cache with a
   warning.
3. **usdcc.** Context binding, the four call sites, the settings page, the login dialog with
   Credential Manager, sessions, typed URLs in File > Open, Python bindings, and the renderer check.
   Done when a stage opened from WebDAV shows correct resolved paths in the Attribute Editor,
   Dependencies panel and USDA view, and renders in Storm.
4. **Writing.** `_CanWriteAssetToPath` and `_OpenAssetForWrite`: write to a temporary file, then
   on close `MKCOL` any missing folders and `PUT` the file, with `If-Match` on the known ETag so
   someone else's newer version is never overwritten silently. The cache entry is updated after a
   successful upload. Done when Save on a layer opened from WebDAV writes it back, and saving over a
   changed file gives a clear error. WebDAV `LOCK` is out of scope.

## 10. Testing

- **Test server:** [WsgiDAV](https://github.com/mar10/wsgidav) (MIT, Python), installed with
  `uv` like the other test tools. The fixture starts two instances on free ports, with Basic auth,
  and a third that answers 503, for failover. Anonymous, Basic and Digest auth are each covered.
- **Unit tests** (`tests/webdav_resolver_test.cpp`): identifier normalization and anchoring,
  context string and JSON parsing, equality and hashing, concrete-path round trips through the
  registry, and that secrets never appear in debug strings or logs.
- **Integration tests:** opening stages through `UsdStage::Open`, textures opened with no
  context bound (as Hydra does), cache revalidation, offline mode and, in phase 4, saving.
- **CI** ([CI.md](CI.md)): runs on the Windows job. No external network is needed.

## 11. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Does an empty authority (`webdav:///…`) mean the default server, or must a server name always be given? | Allow it, for single-server setups; usdcc's settings page warns when files use it |
| 2 | Does a fallback endpoint apply only when the first is down, or also when a file is missing there (an override server)? | Both, in order: that's what makes an override server useful. Resolve stops at the first 2xx |
| 3 | Which authentication do the servers need: Basic, Digest, bearer tokens, NTLM/Kerberos? | Basic and bearer over HTTPS in phase 1, Digest in phase 2; NTLM/Kerberos only if a server needs them (libcurl supports them through SSPI/GSSAPI) |
| 4 | Link libcurl and OpenSSL statically into `webdavResolver.dll`? PySide6 ships its own `libcrypto-3-x64.dll`, the same file name as vcpkg's, and Windows reuses a DLL already loaded by that name | Yes: a static triplet override for `curl` and `openssl` in this plugin avoids the clash. Both licences allow static linking |
| 5 | Should the cache have a size limit? | Yes, default 20 GB, least-recently-used eviction, set on the settings page |
| 6 | Should the WebDAV server's directory listing be browsable from File > Open? | Later; phase 3 only needs typed URLs |
