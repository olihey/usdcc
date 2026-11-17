# WebDAV Resolver (plan)

A USD asset resolver plugin that loads assets from WebDAV servers through `webdav://` asset paths,
such as `webdav://studio/chars/hero/hero.usd`.

**Status:** reading is built (2026-11-17), in [src/plugins/webdav](../src/plugins/webdav), and
tested ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13.3); writing is phase 6. Not in the app or a deploy
yet. The caching, the server names, the environments and the usdcc integration are the shared
library's, in [REMOTE_ASSETS.md](REMOTE_ASSETS.md). This document is only what is WebDAV's own.

## 1. What it is

- **A usdcc plugin with only a `usd/` folder** ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §11): a
  resolver library and its `plugInfo.json`, shipped in `plugins/webdav/`. It has no Qt and no usdcc
  code, so `usdcat`, `usdview` and scripts can use it with `PXR_PLUGINPATH_NAME` pointing at that
  folder.
- **A URI resolver, not the primary one.** It is registered for the scheme `webdav`, beside
  `ArDefaultResolver`. Local paths behave exactly as today, and a studio's own primary resolver can
  be used alongside it.
- **Not the same as an `https://` URL to the same server.** That goes through the HTTP resolver
  ([HTTP_RESOLVER.md](HTTP_RESOLVER.md)): it reads the file, but with the real hostname in the
  layer, and without names, environments, browsing or saving.
- **`WebDavResolver` derives from `usdcc::assets::CachingResolver`** and adds nothing but its scheme
  and its transport (§3). Resolving, stacks, the cache, contexts and credentials are the base
  class's.

```json
{ "Plugins": [{
    "Type": "library", "Name": "usdccWebDavProtocol", "LibraryPath": "usdccWebDavProtocol.dll",
    "Root": ".", "ResourcePath": ".",
    "Info": { "Types": { "UsdccWebDavResolver": {
        "bases": ["ArResolver"], "uriSchemes": ["webdav"],
        "implementsContexts": true, "implementsScopedCaches": true,
        "usdccAssetsProtocol": true
    } } }
}] }
```

`usdccAssetsProtocol` is how `usdcc_assets` finds the protocols to browse before USD has loaded
their libraries ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §6).

## 2. From an asset path to a request

`webdav://studio/chars/hero/hero.usd` in the test environment of
[REMOTE_ASSETS.md](REMOTE_ASSETS.md) §4 lists these folders, in order, and takes `hero.usd` from the
first that has it:

```
https://dav-test.studio.local/assets/chars/hero/      (endpoint "test")
https://dav.studio.local/assets/chars/hero/           (endpoint "prod")
```

The endpoint's URL is a prefix: the asset's path is appended to it, percent-encoded segment by
segment. Query strings and fragments in an asset path are refused.

## 3. The transport

| Call | Request | Notes |
|---|---|---|
| `list(folder)` | `PROPFIND`, `Depth: 1` | Asks for `resourcetype`, `getetag`, `getlastmodified`, `getcontentlength` only. A 207 reply is parsed with pugixml. 404 means the folder is missing |
| `stat(path)` | `PROPFIND`, `Depth: 0` | For folders too big to list |
| `fetch(path, file, knownEtag)` | `GET`, with `If-None-Match` when given an ETag | 304 is "not modified". The resolver gives none: the listing told it which version it wants |
| `put(path, file, knownEtag)` (phase 6) | `MKCOL` for missing folders, then `PUT` with `If-Match`, or `If-None-Match: *` for a new file | 412 is "someone changed it", reported as such |

A timeout, a refused connection, a TLS failure and any 5xx are "unreachable". 401 and 403 are
"not allowed", naming the credential id. Everything else that isn't 2xx, 304 or 404 is an error
with its status.

## 4. Server differences to handle

- **No ETag.** Some servers send none. The version is then made from the modified time and the
  size.
- **Weak ETags** (`W/"…"`) are compared as they are; they aren't valid in `If-Match`, so a save
  against one checks with a `PROPFIND` first.
- **`Depth: infinity` is never used.** Many servers refuse it.
- **Hrefs in a reply** come absolute or relative, encoded differently from the request, and with
  or without a trailing slash on folders. They are normalized before comparing.
- **Case.** Paths are compared exactly. A server that ignores case is the server's business.
- **Redirects** aren't followed: a 3xx is an error naming its code. Following them on the same
  host only, as planned, isn't built.

## 5. Build

- **Source:** [src/plugins/webdav](../src/plugins/webdav): `webdav_protocol.cpp`, the resolver and
  its transport, built with `usdcc_add_asset_protocol()` ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §12),
  which writes the `plugInfo.json` and the plugin's manifest.
- **Links:** `usdcc_assets`, whose HTTP client it uses, and pugixml for the replies to `PROPFIND`.

## 6. Testing

- **Test server:** inside the test program ([assets_test_server.h](../tests/assets_test_server.h)):
  it answers `PROPFIND` with a multistatus made from a folder, with a second `propstat` under a 404
  as Apache sends, and switches for no ETags, a 503 and a password. Not WsgiDAV, as planned: no
  Python and no install in the tests.
- **Tests** ([assets_net_test.cpp](../tests/assets_net_test.cpp), the three named
  `assets_webdav_…`, and the mirror, offline and refresh ones, which run over it): list, stat and
  fetch, with a name with a space, a missing folder, a file asked for as a folder, a 304, and a
  server without ETags; a stage through `UsdStage::Open` whose model comes from TEST and its
  sublayer from PROD, with a listing per folder and no download on the second open; a password,
  and a server answering 503 not being skipped.
- **By hand:** `usdcat` through two `rclone serve webdav` servers
  ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13.3).
- **Not done:** recorded replies from Apache `mod_dav`, nginx, Nextcloud and WsgiDAV; weak ETags;
  Digest.
- **Phase 6:** saving, saving into TEST a layer read from PROD, and a 412.

## 7. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Which authentication do the servers need? | Basic and bearer tokens are built. Digest next. NTLM and Kerberos only if a server needs them (libcurl has them through SSPI and GSSAPI) |
| 2 | Plain `http://` endpoints? | Allowed, with a warning in the Log when credentials are sent over one |
| 3 | Which servers must it work with? | WsgiDAV in CI. Apache, nginx and Nextcloud by their recorded replies, and by hand before a release |
