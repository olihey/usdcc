# WebDAV Resolver (plan)

A USD asset resolver plugin that loads assets from WebDAV servers through `webdav://` asset paths,
such as `webdav://studio/chars/hero/hero.usd`.

**Status:** planned. Nothing is built yet. Revised 2026-11-16: the caching, the server names, the
environments and the usdcc integration moved to a shared library, planned in
[REMOTE_ASSETS.md](REMOTE_ASSETS.md). This document is only what is WebDAV's own.

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
    "Type": "library", "Name": "webdavResolver", "LibraryPath": "webdavResolver.dll",
    "Root": ".", "ResourcePath": ".",
    "Info": { "Types": { "WebDavResolver": {
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
| `fetch(path, file, knownEtag)` | `GET`, with `If-None-Match` when there is an ETag | 304 is "not modified" |
| `put(path, file, knownEtag)` (phase 6) | `MKCOL` for missing folders, then `PUT` with `If-Match`, or `If-None-Match: *` for a new file | 412 is "someone changed it", reported as such |

A timeout, a refused connection, a TLS failure and any 5xx are "unreachable". 401 and 403 are
"not allowed", naming the credential id. Everything else that isn't 2xx, 304 or 404 is an error
with its status.

## 4. Server differences to handle

- **No ETag.** Some servers send none. The version is then made from the modified time and the
  size, and a conditional `GET` uses `If-Modified-Since`.
- **Weak ETags** (`W/"…"`) are compared as they are; they aren't valid in `If-Match`, so a save
  against one checks with a `PROPFIND` first.
- **`Depth: infinity` is never used.** Many servers refuse it.
- **Hrefs in a reply** come absolute or relative, encoded differently from the request, and with
  or without a trailing slash on folders. They are normalized before comparing.
- **Case.** Paths are compared exactly. A server that ignores case is the server's business.
- **Redirects** are followed only on the same host, and a `PUT` is never redirected.

## 5. Build

- **Source:** `src/plugins/webdav/`: `webdav_resolver.cpp`, `webdav_transport.cpp`, `plugInfo.json`,
  built with `usdcc_add_asset_protocol()` ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §12).
- **Links:** `usdcc_assets` and OpenUSD's `ar`. libcurl and pugixml come through `usdcc_assets`, which
  wraps both, so the plugin has no third-party code of its own.

## 6. Testing

- **Test server:** [WsgiDAV](https://github.com/mar10/wsgidav) (MIT, Python), installed with `uv`
  like the other test tools. The fixture starts instances on free ports: two with Basic auth, as
  TEST and PROD, and one answering 503.
- **Unit tests** (`tests/webdav_transport_test.cpp`): parsing recorded 207 replies from several
  servers (WsgiDAV, Apache `mod_dav`, nginx, Nextcloud), href normalization, the no-ETag and
  weak-ETag cases.
- **Against the server:** list, stat and fetch; a stage opened through `UsdStage::Open` whose model
  comes from PROD and one texture from TEST; a 304 on the second open; the 503 instance as an
  unreachable endpoint; anonymous, Basic and Digest auth.
- **Phase 6:** saving, saving into TEST a layer read from PROD, and a 412.

## 7. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Which authentication do the servers need? | Basic and bearer tokens over HTTPS first, then Digest. NTLM and Kerberos only if a server needs them (libcurl has them through SSPI and GSSAPI) |
| 2 | Plain `http://` endpoints? | Allowed, with a warning in the Log when credentials are sent over one |
| 3 | Which servers must it work with? | WsgiDAV in CI. Apache, nginx and Nextcloud by their recorded replies, and by hand before a release |
