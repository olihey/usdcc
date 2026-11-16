# HTTP Resolver (plan)

A USD asset resolver plugin for plain `http://` and `https://` URLs, such as
`https://assets.example.com/chars/hero/hero.usd`. Read-only, and the simplest protocol built on the
shared library of [REMOTE_ASSETS.md](REMOTE_ASSETS.md).

**Status:** planned 2026-11-16. Nothing is built yet.

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
    "Type": "library", "Name": "httpResolver", "LibraryPath": "httpResolver.dll",
    "Root": ".", "ResourcePath": ".",
    "Info": { "Types": { "HttpResolver": {
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
| `fetch(path, file, knownEtag)` | `GET`, with `If-None-Match`, or `If-Modified-Since` when there is no ETag | 304 is "not modified" |
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

Most HTTP assets are public, so the default is anonymous. A host can be given a credential id in
the configuration ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §4, §8):

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

- **In usdcc,** a host not on the list asks once, "This stage loads from `example.com`. Allow?",
  with **Always**, **This time** and **No**. Always adds it to the list.
- **Outside usdcc,** with no one to ask, a host not on the list is refused with a message naming
  it. `"allow": ["*"]` or `USDCC_HTTP_ALLOW=*` turns the check off.

Plain `http://` works, with one warning in the Log per host, and never with credentials unless the
host's entry says `"insecure": true`.

## 5. Limits

- **Signed URLs** (a signature and expiry in the query string) work, but each new signature is a
  new URL, so the cache downloads the file again, and a relative reference loses the signature.
- **No change notice between requests.** A file is revalidated when it's resolved again, on Reload.
- **Servers without `ETag` or `Last-Modified`** give nothing to compare: the file is downloaded
  again on every open or Reload.

## 6. Build

`src/plugins/http/`: `http_resolver.cpp`, `http_transport.cpp`, `plugInfo.json`, built with
`usdcc_add_asset_protocol()`. It links `usdcc_assets` and OpenUSD's `ar`, and needs no XML parser.

## 7. Testing

- **Test server:** Python's own `http.server`, started by the fixture on a free port. It sends
  `Last-Modified` and no `ETag`, which covers the no-ETag path. A second, small handler in the
  fixture adds ETags, redirects, a 405 on `HEAD`, a slow reply and a 503.
- **Unit tests** (`tests/http_transport_test.cpp`): URL normalization, relative paths, query
  strings, extensions.
- **Against the server:** stat and fetch; a stage through `UsdStage::Open` with a relative
  reference; a 304 on the second open; a redirect to another port without the credential; a host
  not on the allow list refused.

## 8. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Check hosts against an allow list at all? | Yes, as in §4. A file that can make the app call any address is a real risk, and one question per new host is a small price |
| 2 | Should a missing file be asked for at all, or should resolve just say yes and let the open fail? | Ask. USD treats an unresolved path and a failed open differently, and the answers are cached |
| 3 | Custom request headers per host? | Later, if a server needs them |
