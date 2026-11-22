# HTTP Resolver (plan)

A USD asset resolver plugin for plain `http://` and `https://` URLs, such as
`https://assets.example.com/chars/hero/hero.usd`. Read-only, and the simplest protocol built on the
shared library of [REMOTE_ASSETS.md](REMOTE_ASSETS.md).

**Status:** built 2026-11-17, in [src/plugins/http](../src/plugins/http), tested
([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13.3), and shipped with the app (§13.4). **`https://` was
first run on 2026-11-22**, on Windows (curl with Schannel), with `usdcat --flatten` and
`USDCC_HTTP_ALLOW`: a public asset on `raw.githubusercontent.com` (a root layer, four referenced
layers and textures by relative path) flattened in 3 s, all five layers anchored on `https://`,
and a second open took 0.6 s with no new files in the cache. A self-signed certificate
(`self-signed.badssl.com`) is refused with Schannel's `SEC_E_UNTRUSTED_ROOT` in the message, but
reported as *unreachable*, like a server that's down (`statusOfCurlCode()` in
`http_client.cpp`). Not run: `https://` on Linux (OpenSSL), WebDAV over `https://`, a host
without an allow-list entry other than the refusal.

## 1. What it is, and isn't

- **The URL is the real address.** Unlike `webdav://studio/…`, there is no logical name, no
  environment and no stack: `https://assets.example.com/…` means that host. It is a *direct*
  protocol ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §5.1).
- **Read-only.** There is no `put`. Saving a layer that came from HTTP is refused, and Save As
  offers somewhere else.
- **No browsing.** Plain HTTP can't list a folder. A URL is typed or pasted
  ([ASSET_PICKER.md](ASSET_PICKER.md) §3).
- **It still gets the cache,** offline mode, credentials, progress and cancel from `usdcc_assets`.
- **A usdcc plugin with only a `usd/` folder,** shipped in `plugins/http/`, usable from `usdcat`
  and `usdview` like the WebDAV one.

```json
{ "Plugins": [{
    "Type": "library", "Name": "usdccHttpProtocol", "LibraryPath": "usdccHttpProtocol.dll",
    "Root": ".", "ResourcePath": ".",
    "Info": { "Types": { "UsdccHttpResolver": {
        "bases": ["ArResolver"], "uriSchemes": ["http", "https"],
        "implementsContexts": true, "implementsScopedCaches": true,
        "usdccAssetsProtocol": true
    } } }
}] }
```

Another plugin may already claim these schemes. OpenUSD uses one resolver per scheme and warns
about the other, so this plugin is turned off in View > Plugins when a studio's own should win.

## 2. Asset paths

```
https://assets.example.com/chars/hero/hero.usd
https://assets.example.com/chars/hero/hero.usd?v=12      a query string is part of the address
tex/albedo.png                                            relative, anchored against its layer
```

- **Identifiers** are normalized as URLs: scheme and host lowercased, the default port dropped, `.`
  and `..` collapsed, percent-encoding made canonical. The query string is kept as it is; a
  fragment is refused.
- **Relative paths** are resolved as a browser does: against the layer's URL, dropping its query
  string.
- **The extension** comes from the path, never from the query string.
- **The resolved path is the identifier.** There is no stack id, and no path is context-dependent.

## 3. The transport

| Call | Request | Notes |
|---|---|---|
| `stat(path)` | `HEAD` | `ETag`, `Last-Modified`, `Content-Length`. A server that refuses `HEAD` (405, 501) gets a `GET` with `Range: bytes=0-0` |
| `fetch(path, file, knownEtag)` | `GET`, with `If-None-Match` when given an ETag | 304 is "not modified". The resolver gives none: it knows from `stat` which version it wants, and only downloads one the cache doesn't have |
| `list(folder)` | Not supported | Resolve asks `stat` per file ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §7) |
| `put` | Not supported | |

404 and 410 are "missing". A timeout, a refused connection, a TLS failure and any 5xx are
"unreachable". 401 and 403 are "not allowed".

**One request per file.** With no listings, every file composition asks about costs a `HEAD`, and
a UDIM texture asks about tiles that don't exist. Answers, missing ones too, are remembered for the
scope of a stage open and a few seconds outside it, as for every protocol. A large stage over HTTP
will still open more slowly than over WebDAV.

**Redirects** are followed, up to five, across hosts too: CDNs and download links depend on it.
Credentials are never sent on to another host, and `https` is never redirected to `http`. The cache
knows a file by the URL that was asked for, not the one it ended at.

## 4. Credentials and hosts

Most HTTP assets are public, so the default is anonymous. A host's login is looked for under its
address, `host:port` and then `host`, and a host can be given another credential id in the
configuration ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §4, §8):

```json
{
  "http": {
    "hosts": {
      "assets.example.com": { "credential": "example-token" }
    },
    "allow": ["assets.example.com", "*.studio.local"]
  }
}
```

**A stage can make usdcc contact any server.** A `.usd` file from somewhere else can reference
`http://anything/…`, and opening it sends a request there: to a tracker, or to a machine inside the
company network. So hosts are checked against `allow`:

- **A host the user opens a stage on** (File > Open, Open Stage from URL, Open Recent, the command
  line, a session's stage) is allowed for the rest of the session. What that stage names is
  checked as usual.
- **In usdcc,** a host not on the list is refused, and a toast says to add it in **Settings >
  Assets > Allowed hosts** and reload the stage. *Changed from the plan,* which asked "Allow?":
  the library asks on whichever thread resolves, which may be one the UI thread waits for, so a
  question there could hang the app.
- **Outside usdcc,** with no one to ask, a host not on the list is refused with a message naming
  it. `"allow": ["*"]` or `USDCC_HTTP_ALLOW=*` turns the check off.

Plain `http://` works, with one warning in the Log per host, and never with credentials unless the
host's entry says `"insecure": true`.

## 5. Limits

- **Signed URLs** (a signature and expiry in the query string) work, but each new signature is a
  new URL, so the cache downloads the file again, and a relative reference loses the signature.
- **No change notice between requests.** A file is revalidated when it's resolved again, on Reload.
- **Servers without `ETag`** have a file's version told by its `Last-Modified` and size. With
  neither, every file is one version for ever: a change on the server isn't seen until the cache is
  cleared.

## 6. Build

[src/plugins/http](../src/plugins/http): `http_protocol.cpp`, the resolver and its transport,
built with `usdcc_add_asset_protocol()`, which writes the `plugInfo.json` and the plugin's manifest.
It links `usdcc_assets` only, and needs no XML parser.

## 7. Testing

- **Test server:** inside the test program ([assets_test_server.h](../tests/assets_test_server.h)),
  with switches for no ETags, a 405 on `HEAD`, a slow reply, a 503 and redirects.
- **Tests** ([assets_net_test.cpp](../tests/assets_net_test.cpp), the four named `assets_http_…`
  and the progress one): a stage through `UsdStage::Open` with a reference, a sublayer and a file
  with a space in its name, and no download on the second open; a host not on the list refused
  without a request, the callback asked once per host, `USDCC_HTTP_ALLOW`; a server without `HEAD`
  or ETags, a query string, a redirect to another server without the credential; a 404, a 503 and
  a port nothing listens on; URL normalization and relative paths.
- **By hand:** `usdcat` against Python's `http.server` ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13.3).

## 8. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Check hosts against an allow list at all? | Yes, as in §4. A file that can make the app call any address is a real risk, and one question per new host is a small price |
| 2 | Should a missing file be asked for at all, or should resolve just say yes and let the open fail? | Ask. USD treats an unresolved path and a failed open differently, and the answers are cached |
| 3 | Custom request headers per host? | Later, if a server needs them |
