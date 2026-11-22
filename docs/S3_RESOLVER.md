# S3 Resolver

A USD asset resolver plugin that loads assets from Amazon S3 and the stores that speak its API,
through `s3://` asset paths such as `s3://studio/chars/hero/hero.usd`.

**Status:** reading is built (2026-11-20), phases S3.1 to S3.4, in
[src/plugins/s3](../src/plugins/s3), tested against the test program's S3 server (§10) after the
spike (§8.1) ran against rclone's and Garage. Not yet run against AWS itself. §8.2 is what was built
and where it differs from the plan; writing (phase 6) is built too (2026-11-22). The caching, the server names, the environments
and the usdcc integration are the shared library's, in [REMOTE_ASSETS.md](REMOTE_ASSETS.md). This
document is only what is S3's own, and the few things S3 needs from the shared library (§6).

## 1. What it is

- **A usdcc plugin with only a `usd/` folder** ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §11), like
  WebDAV's: a resolver library and its `plugInfo.json`, shipped in `plugins/s3/`. No Qt and no
  usdcc code, so `usdcat`, `usdview` and scripts can use it.
- **`S3Resolver` derives from `usdcc::assets::CachingResolver`** and adds its scheme, its
  transport (§5) and the AWS credential lookup (§4). Resolving, stacks, the cache, offline mode,
  progress, cancel and contexts are the base class's.
- **The same disk cache** as WebDAV and HTTP ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §7). An S3
  server is an `Endpoint` like any other, so its copies sit under its own hashed folder. The names
  being hashes matters more here than anywhere: an S3 key can be 1,024 bytes long and hold almost
  any character, and none of it reaches the file system.
- **S3-compatible stores come with it:** MinIO, Ceph's RADOS Gateway, Cloudflare R2, Wasabi,
  Backblaze B2, and Google Cloud Storage through its XML API with HMAC keys. They differ in URL,
  region and where the bucket goes (§3), not in requests.
- **Read-only first.** Writing came with the shared library's phase 6 (§8; built 2026-11-22, [REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13.6).

## 2. Signing: libcurl, not an SDK

S3 wants every request signed with AWS Signature Version 4. libcurl does that itself
(`CURLOPT_AWS_SIGV4`, since 7.75; the vcpkg baseline has 8.21), from the URL, the headers and the
access key and secret.

| Option | Verdict |
|---|---|
| **libcurl's SigV4** | Chosen. No new dependency. The HTTP client's timeouts, proxies, progress, cancel and Windows' own TLS ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §8) stay the same for every protocol |
| aws-sdk-cpp (vcpkg 1.11.860) | Every AWS way of logging in, retries, all of S3. But it brings `aws-crt-cpp` and ten `aws-c-*` libraries with an HTTP stack of their own: a second set of timeouts, proxies, progress and cancel to keep in step, and a much bigger deploy |
| minio-cpp (vcpkg 0.4.0) | Small, but needs OpenSSL, which §8 keeps out of the process beside PySide6's, and curlpp |
| OpenDAL ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13.5) | Still the plan for Azure, GCS's own API and SFTP, not for S3 |

What libcurl leaves to us: the AWS credential lookup (§4), finding a bucket's region, and the
session token header. To check first, in the spike (§8): keys with characters that need escaping
(the signature covers the path exactly as sent), the sorted query string of a listing, and the
`x-amz-content-sha256` header S3 requires.

## 3. From an asset path to a request

### 3.1 By name

```
s3://studio/chars/hero.usd
```

`studio` is a name from the configuration, as with WebDAV: environments and TEST over PROD work
the same ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §4, §5). An S3 endpoint:

```json
"endpoints": {
  "prod": { "scheme": "s3", "url": "s3://studio-assets/prod", "region": "eu-central-1",
            "credential": "aws" },
  "test": { "scheme": "s3", "url": "https://minio.studio.local:9000/assets/test",
            "credential": "minio-test" }
},
"environments": {
  "prod": { "s3": { "studio": ["prod"] } },
  "test": { "s3": { "studio": ["test", "prod"] } }
}
```

- **`s3://bucket/prefix`** is a bucket on AWS, and the folder in it that the name's root is.
  Requests go to `https://<bucket>.s3.<region>.amazonaws.com/<prefix>/<path>`.
- **`https://…`** is any S3-compatible store. The bucket is the path's first part (path style,
  which MinIO and Ceph use by default), or the host name's first part with `"bucketInHost": true`.
- **`region`** is part of the signature. It may be left out: a request signed for the wrong
  region is refused with `AuthorizationHeaderMalformed`, whose error body names the right one in
  `<Region>` (Garage does, §8.1), and AWS also sends it in an `x-amz-bucket-region` header. So the
  transport asks with a `GET` (a `HEAD`'s answer has no body), takes the header or else the
  element, and remembers it for the session. Without either, `us-east-1`.
- **`mirrors`** are replica buckets, in other regions too: each URL finds its own region.

### 3.2 By bucket

```
s3://my-bucket/chars/hero.usd
```

How the AWS command line and other tools write S3 paths. When the part after `//` isn't a name the
environment has, it's a bucket on AWS, as an address is for WebDAV
([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §2.1):

- **A configured name wins** over a bucket of the same name.
- **A single endpoint:** no environment, no stack, no stack id. The resolved path is the
  identifier.
- **The region** is found as above; **the credential** is AWS's own lookup (§4.2).
- **It has to be allowed,** like a plain HTTP host. A stage can name any bucket, and a signed
  request shows the bucket's owner who sent it: the access key, and with it the account. Settings >
  Assets gets **Allowed buckets**, and a bucket the user opens (File > Open, Open Stage from URL,
  Open Recent, the command line, a session's stage) is allowed for the session. Any other is
  refused, with a toast saying how to allow it.
- **Only AWS.** A bucket on another store needs its URL: a name in the configuration, or the
  server's address (§3.3). A refusal from AWS for a bucket named by itself says so, since
  `s3://nas1/` looks like a server and is a bucket.

### 3.3 By address

```
s3://nas1:9000/media/shots/s010.usda
s3://192.168.178.53:3900/
```

A server on the network, a NAS say, named by its address, as WebDAV's are
([WEBDAV_RESOLVER.md](WEBDAV_RESOLVER.md) §2.1). Added 2026-11-20, after `s3://nas1/` in the
picker went to a bucket of that name on AWS.

- **When it's an address:** the part after `//` has a port (no bucket name has a `:`), is an IP
  address (no bucket name may be one), or is `localhost`. `s3://nas1/` without a port is still a
  bucket on AWS.
- **The whole server:** the path's first part is the bucket. `s3://nas1:9000/` lists the server's
  buckets (`ListBuckets`), so the picker can start there. A configured endpoint whose URL is a
  server without a bucket (`"url": "http://nas1:9000"`) works the same way.
- **https:// first.** A server that connects but never completes TLS (most NAS S3 servers have
  none) is asked over `http://` from then on, with a warning in the Log once. A server whose TLS
  fails for another reason, such as a certificate Windows doesn't trust, isn't.
- **Its login** is saved under the address, as WebDAV's: `nas1:9000` when one is saved for that
  port, else `nas1` (Edit > Asset Server Login, or `USDCC_ASSETS_NAS1_USER` and `_PASSWORD`): the
  access key ID and the secret. A `"hosts"` entry in the configuration's `"http"` section can name
  another credential.
- **A host like plain HTTP's** ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §4): allowed, as a stage can
  name any address, when opened by the user or on Allowed hosts. Not Allowed buckets.
- **No stack, no stack id;** a relative path stays on the same server. It writes the address into
  what is saved, as WebDAV's do.

## 4. Credentials

### 4.1 A usdcc credential id

As for WebDAV ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §8): the login dialog, Windows Credential
Manager (`usdcc-assets:<id>`), or `USDCC_ASSETS_<ID>_USER`, `_PASSWORD` and `_TOKEN`. For S3:

| `Credential` field | S3 |
|---|---|
| `user` | the access key ID |
| `password` | the secret access key |
| `token` | a session token, for temporary credentials (sent as `x-amz-security-token`) |

Edit > Asset Server Login labels them so for a credential id that an S3 endpoint uses.

### 4.2 AWS's own lookup

For `"credential": "aws"`, `"aws:<profile>"`, and a bucket named by itself (§3.2). The places AWS's
own tools look, in their order, so a machine set up for the AWS command line needs nothing more:

1. **The environment:** `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`, and
   `AWS_PROFILE` for the profile.
2. **The shared files:** `~/.aws/credentials` and `~/.aws/config` (or `AWS_SHARED_CREDENTIALS_FILE`
   and `AWS_CONFIG_FILE`), with profiles and their `region`.
3. **`credential_process`** in a profile: a command that prints credentials as JSON. AWS's single
   sign-on is covered by it (`aws configure export-credentials --format process`), so its own
   flow isn't built (§9).
4. **The machine's role on EC2** (the instance metadata service, version 2), for render farms on
   AWS. Asked only when nothing above answered, with a short timeout: off EC2 there is nothing
   there.

Temporary credentials (a session token, from 3 or 4) are kept until five minutes before they
expire, then asked for again. An endpoint without a credential, and a bucket that's public, go
unsigned.

**This lookup lives in the S3 plugin.** `usdcc_assets` knows nothing of AWS.

## 5. The transport

| Call | Request | Notes |
|---|---|---|
| `listPage` | `GET /?list-type=2&prefix=<folder>/&delimiter=/` (`ListObjectsV2`) | For browsing, a page at a time. `Contents` are files (`Key`, `Size`, `LastModified`, `ETag`), `CommonPrefixes` folders. A page holds 1,000; `continuation-token` asks for the next. The empty `folder/` objects some tools make for folders are left out. A server's root (§3.3) lists its buckets |
| `list` | not supported | So resolving asks `stat` for each file (below) |
| `stat` | `HEAD /<key>` | `Content-Length`, `Last-Modified`, `ETag`. What resolving asks |
| `fetch` | `GET /<key>`, with `If-None-Match` | A 304 writes nothing |
| `put` (phase 6, built) | `PUT /<key>`, signed, the body `UNSIGNED-PAYLOAD`, with `If-None-Match: *` for a new file and `If-Match` over an old one; a `HEAD` afterwards | S3 has had conditional writes since 2024, so saving over a file changed meanwhile is the clear error phase 6 plans: 412 `PreconditionFailed` and 409 `ConditionalRequestConflict` are `Status::Conflict`. Other errors carry S3's code; a 403 names `s3:PutObject` |

- **Folders don't exist in S3,** only keys with slashes. A folder is there when a key starts with
  it. A file asked for as a folder, or the other way round, is told apart by the listing.
- **ETags:** an object uploaded in parts has an ETag ending in `-<parts>` that isn't its MD5. It
  still changes with the content, which is all the cache needs.
- **Resolving asks for each file** (`HEAD`), not for its folder's listing as WebDAV does. *Changed
  2026-11-20:* a NAS's `mesh/` of thousands of folders took 7 to 30 seconds a page to list, so
  resolving one file in it waited minutes; a `HEAD` is one cheap request whatever the folder holds.
  A `HEAD` still tells "not in TEST" (404) from "TEST is down".
- **Browsing goes a page at a time** (`listPage`): the asset picker shows the first page as it
  comes, with **Show More** for the next ([ASSET_PICKER.md](ASSET_PICKER.md) §9).
- **403 against missing.** Without the right to list a bucket (`s3:ListBucket`), S3 answers a
  missing key with 403, not 404. A TEST bucket would then stop PROD's file instead of letting it
  through. So a bucket must allow listing, and when it doesn't, the 403 says so: "can't list
  <bucket>: TEST over PROD needs s3:ListBucket". A 403 from `HEAD` is never taken for missing.
- **Encryption** with keys S3 or KMS manage is invisible to a reader. Keys the client supplies
  (SSE-C) aren't supported.
- **Requester pays** buckets aren't supported at first.

## 6. What S3 needs from `usdcc_assets`

The S3 phase of [REMOTE_ASSETS.md](REMOTE_ASSETS.md) §13 asks for no change "beyond what S3 itself
forces". This is what it forces:

- **`Endpoint::options`:** the configuration's keys the library doesn't know, as strings
  (`region`, `bucketInHost`). Kept in the configuration file as they are, and edited in the Asset
  Servers editor (§7). OpenDAL needs the same later. Not part of the cache id: a region doesn't
  change which files a bucket holds.
- **`HttpRequest::awsSigV4`:** `"aws:amz:<region>:s3"`. Set, the request is signed instead of
  sent with Basic or a bearer token, and a session token goes as `x-amz-security-token`.
- **`HttpRequest` takes a credential itself,** as well as a credential id, so the plugin can pass
  in what AWS's lookup found. Still only sent to the request's own host.
- **`Credential::expires`,** for temporary credentials.
- **ISO 8601 dates** (`2026-11-20T09:15:00.000Z`, S3's listings), beside `parseHttpDate`.
- **The XML helpers** WebDAV has (a node's local name, a child by name) move to a header both
  plugins use. pugixml is already deployed.

## 7. In usdcc

- **The asset picker** browses buckets with no change: it uses any transport that can list, and
  its scheme dropdown lists every registered scheme ([ASSET_PICKER.md](ASSET_PICKER.md)).
- **Edit > Asset Server Login** labels the fields Access key ID, Secret access key and Session
  token for an S3 credential id (§4.1).
- **Settings > Assets > Allowed buckets** (§3.2), beside Allowed hosts.
- **The Asset Servers editor** ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §10.1) gains S3's fields:
  bucket and prefix or URL, region, bucket in the host name. Its Test button lists the root.
- **Errors say what S3 said:** its XML error body carries a code (`AccessDenied`,
  `NoSuchBucket`, `SignatureDoesNotMatch`, `ExpiredToken`) and a message, which go into the Log
  and the toast with the HTTP status.

## 8. Phases

| Phase | Builds | Done when |
|---|---|---|
| **S3.0 Spike** | Signed `HEAD`, `GET` and `ListObjectsV2` through `httpRequest`, against AWS and MinIO. **Run against rclone's S3 server and Garage** (§8.1; MinIO's image is no longer public); AWS still to do | Keys with a space, `+`, `=`, `%`, `~` and accented letters read on both; a listing of more than 1,000 keys pages; a region is found from a refusal; a session token works |
| **S3.1 Read-only** | §6, the transport and plugin, by name, with usdcc credential ids; S3 in the test server (§10). **Built** (§8.2) | `usdcat s3://studio/shot.usda` prints a shot whose model comes from PROD and one texture from TEST, with a listing per folder and nothing downloaded on the second open |
| **S3.2 AWS credentials** | §4.2: the environment, the shared files, `credential_process`, EC2's role, renewal. **Built**; EC2's role and renewal untested | A profile using AWS single sign-on through `credential_process` opens a stage, and keeps working past its credentials' expiry |
| **S3.3 By bucket** | §3.2, finding the region, Allowed buckets. **Built**, against the test server through `AWS_ENDPOINT_URL_S3`, not AWS | `usdcat s3://<bucket>/shot.usda` works with only the AWS command line set up; a stage naming another bucket is refused with the toast |
| **S3.4 usdcc** | §7. **Built**, but for the Asset Servers editor's fields: the editor isn't built yet | A reference to a file in a bucket is added by browsing to it in the picker |
| **With phase 6** | `put` (§5). **Built** (REMOTE_ASSETS.md §13.6), against the test server: `assets_s3_writes` | Save on a layer from PROD, in the test environment, writes it to the TEST bucket |
| **Later** | Reading part of a large file; pinning an object version (`versionId`); requester pays | |

### 8.1 What the spike found

[tests/s3_spike.cpp](../tests/s3_spike.cpp) (`usdcc_s3_spike`, built with the tests, run by hand)
sends signed requests through `httpRequest` and prints what each got. `HttpRequest` gained
`awsSigV4` and `explicitCredential` for it (§6), which the plugin will use as they are.

Run 2026-11-20 against two servers that check signatures, each with a bucket of 13 awkwardly named
files, a folder with a space, and a folder of 1,100 files:

- **`rclone serve s3`** (rclone 1.72) over a local folder.
- **Garage** v2.5.0 (`dxflrs/garage` in Docker), one node, region `eu-central-1`, the files uploaded
  through Windows' curl with `--aws-sigv4`. In place of MinIO: `minio/minio` is gone from Docker
  Hub and `quay.io/minio/minio` wants a login.

What they showed:

- **Keys must be escaped strictly, by the plugin.** Everything but `A-Z a-z 0-9 - . _ ~` and `/`
  escaped, and every key read: a space, `+ = % ~ ( ) $ , ; @ & '` and `café`. Escaped the way
  `percentEncodePath()` does, which leaves `+ = ( ) $ , ; @ & '` as they are (valid in a URL
  path), they failed the signature (403 on both servers), and a bare `%` was a 400 on rclone and a
  403 on Garage. libcurl's source says why: for
  S3 it signs the path exactly as sent and doesn't escape it again, while S3 escapes the key
  strictly when it checks. So the transport decodes the path it's given and escapes each key
  itself, not with `percentEncodePath()`.
- **libcurl sorts and escapes the query string for the signature,** so a listing's parameters can
  go in any order. Values are escaped strictly too (a prefix with a space worked).
- **libcurl adds `x-amz-content-sha256`** for the service `s3` itself: the hash of the empty body
  for `GET` and `HEAD`. For an upload from a file (phase 6) usdcc sends `UNSIGNED-PAYLOAD` itself, which libcurl then signs with: a streamed body isn't hashed ahead.
- **`If-None-Match` with the ETag gives 304,** as the cache needs. ETags are quoted, as HTTP's.
- **Paging:** a folder of 1,100 files came in two pages of the default 1,000 on both, all keys
  once, and Garage paged the root two keys at a time correctly (8 pages; folders count towards a
  page). With two keys a page, rclone looped: it lists keys out of order, and its continuation
  token, the last key of a page, then skips back. rclone's bug, but the transport stops when a
  token comes back or a page brings no new key, rather than trusting the server.
- **Errors come as S3's XML** with a code and a message: `NoSuchKey` on a 404; on a 403,
  `SignatureDoesNotMatch` (rclone) or `AccessDenied: Invalid signature` (Garage) for a wrong secret,
  and `AccessDenied` for an unsigned request on Garage (rclone said 400 `UnsupportedAlgorithm`).
  Worth showing as they are (§7). A `HEAD`'s answer has no body, so only its status.
- **The region:** rclone ignores it (signed for the wrong one, it answered 200). Garage refused with
  400 `AuthorizationHeaderMalformed`, named the scope it expected, and put the right region in a
  `<Region>` element; no `x-amz-bucket-region` header. Hence §3.1's way of finding it.
- **The bucket in the host name** (`spike.s3.localhost:3900`, Garage's `root_domain`) read as the
  path style did. libcurl resolves `*.localhost` to the machine itself.

**Still to run, on AWS:** a session token (neither server has STS), `x-amz-bucket-region`, AWS's own
virtual-hosted URLs, and its answer to a missing key without `s3:ListBucket` (§5). It needs a test
bucket and keys: `USDCC_SPIKE_S3_KEY`, `USDCC_SPIKE_S3_SECRET`, and `USDCC_SPIKE_S3_TOKEN` for
temporary ones.

### 8.2 S3.1 to S3.4 as built

**Where:** [s3_protocol.cpp](../src/plugins/s3/s3_protocol.cpp) (the transport, the resolver,
a bucket by itself), [aws_credentials.cpp](../src/plugins/s3/aws_credentials.cpp) (§4.2),
and the shared library's part (§6).

**What the shared library gained** (§6, as planned, and):

- `percentEncodeStrict()` in `uri.h`, for keys and query values.
- `HttpRequest::connectTimeout`, for EC2's metadata service, which off EC2 never answers: one
  second, and asked once a process.
- The bucket allow list in `activity.h`: `bucketAllowed()`, `setExtraAllowedBuckets()`,
  `setBucketRefusedCallback()`, the configuration's `"s3": { "allow": [...] }` and
  `USDCC_S3_ALLOW`. A pattern is a name with `*` for any characters (`studio-*`), not the
  `*.domain` of hosts. `allowHostOfUrl()` of an `s3://` URL allows its bucket.
- The XML helpers in [src/plugins/common/xml_names.h](../src/plugins/common/xml_names.h),
  header-only, which WebDAV uses too.
- A plain `http://` request that's signed gives no warning about its credential: the secret
  doesn't go over the wire. With a session token, which does, it still warns.

**How it works:**

- **The region.** Asked for with `us-east-1` (on AWS, the bucket's global name,
  `<bucket>.s3.amazonaws.com`) until an answer names another: a 301 or 400 with
  `x-amz-bucket-region`, or `<Region>` in the error body. That one request is sent again for the
  named region, and the region is remembered for the session, by the bucket's URL. A configured
  `region` is never second-guessed.
- **On AWS,** a bucket with a dot in its name goes path style
  (`s3.<region>.amazonaws.com/<bucket>`): AWS's certificate doesn't cover the dots.
- **A bucket by itself** goes to AWS, or to `AWS_ENDPOINT_URL_S3` (else `AWS_ENDPOINT_URL`), path
  style, as AWS's tools do. The tests use that to send it to the test server.
- **Folders.** S3 can't tell a folder with no keys from one that isn't there, so both list as
  empty, and the picker says "Nothing here". A NAS's server that keeps objects as files was seen
  listing `mesh/11003001/` as a folder of `mesh/` while no key at all is under it: an empty
  directory on its disk. A path that isn't a key is missing to `stat`, but an endpoint's or a
  bucket's root, which is a folder:
  resolving asks for files, and asking whether a folder exists would be a listing again.
- **Paging** stops at the token it was given coming back, or a page with nothing new (§8.1).
- **The region from a `HEAD`:** its refusal has no body to name the region in, and Garage sends
  no `x-amz-bucket-region`, so the same key's first byte is asked for (`Range: bytes=0-0`), whose
  refusal names it.
- **`credential_process`** runs through `cmd.exe /c` on Windows, without a console window (usdcc
  is a windowed program), and is waited for a minute at most; elsewhere through the shell.
- **Refusals** say how the request went: signed with which credential, or unsigned and why
  (nothing saved under the id; no AWS credentials, and where they were looked for), and the
  permission a 403 needs.

**In usdcc** (§7): Settings > Assets > S3 > Allowed buckets; a sticky toast once per bucket a stage
was refused; Edit > Asset Server Login labels an S3 server's credential Access key ID and Secret
access key, without the token choice, and leaves out `aws` and `aws:<profile>`; Open Stage from URL
names `s3://` as an example. The picker browses buckets with nothing added, and lists a bucket by
itself only when it's allowed.

**Differences from the plan:**

- **Session tokens with a usdcc credential id** come only from `USDCC_ASSETS_<ID>_TOKEN`: Windows
  Credential Manager keeps a user and a password. They expire within hours, so saving one is of
  little use; AWS's own lookup (§4.2) is the way to temporary credentials.
- **No question for a bucket,** as there's none for a host: a note, and Allowed buckets.
- **The Asset Servers editor's S3 fields** wait for the editor (REMOTE_ASSETS.md §10.1).

**Tests** ([assets_net_test.cpp](../tests/assets_net_test.cpp), `assets_s3_…`, and
[asset_picker_test.cpp](../tests/asset_picker_test.cpp), `asset_settings_allowed_buckets`):

- list, stat and fetch through a prefix, with `+ = ( ) @` and a space in keys, a 304, paging two
  keys a page, a missing folder and a file asked for as one, the region found from one refusal and
  remembered;
- a missing key without `s3:ListBucket` (403) and a refused listing, saying why; a credential id
  nothing knows;
- a stage through `UsdStage::Open` from two buckets, TEST over PROD, and a texture with a space;
- a profile's keys, a profile's `credential_process` with a session token, a profile that isn't
  there, and the environment's keys for `aws`;
- a bucket by itself: refused, then allowed by opening it, resolving with no stack id, opening with
  its reference, listing for the picker; a configured name never taken for a bucket;
  `USDCC_S3_ALLOW` patterns; the setting reaching the library, and the refusal being told.

- a server by its address (§3.3): refused without a login, naming the credential to save it
  under; its buckets listed at its root; a stage opened from it, with a relative reference and a
  texture with a space; https:// tried first and http:// taken when it had no TLS.

**Not tested:** AWS itself (§8.1's list), EC2's role, renewal before expiry, a dotted bucket's
path style, `bucketInHost`, and a real NAS.

## 9. Not planned

- **AWS's single sign-on flow itself,** and assuming roles (`role_arn` with `source_profile`):
  `credential_process` covers both.
- **Uploads in parts,** needed above 5 GB per file. A layer or a texture isn't that big.
- **SSE-C,** encryption with keys the client supplies.

## 10. Testing

- **Test server** ([assets_test_server.h](../tests/assets_test_server.h), `serveS3()`): S3's
  requests over a folder, path style: `ListObjectsV2` with paging, `HEAD`, `GET` and 304, and
  switches for 403 on a missing key and on listing. It checks what it can short of the signature:
  an `Authorization` header with the access key and the server's region (the wrong one refused
  with `<Region>`, as Garage does), `x-amz-content-sha256`, and the key escaped strictly, as S3's
  signature needs it. Checking the signature itself would mean writing SigV4 a second time.
- **Garage in Docker** (`dxflrs/garage`) as the server that checks signatures for real, as in the
  spike (§8.1): one container, a bucket and a key made with its command line.
- **Tests** in [assets_net_test.cpp](../tests/assets_net_test.cpp), as WebDAV's: list, stat and
  fetch with awkward keys, a missing folder, a 304, paging; a stage through `UsdStage::Open` from
  two buckets; the 403 message; the shared files and `credential_process` from a test folder.
- **Signatures for real:** a test that runs only when `USDCC_TEST_S3_URL` and its credentials are
  set, against Garage or a real bucket. Run by hand before a release, as the WebDAV servers are.

## 11. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should `s3://bucket/key` work beside names? | Yes (§3.2): it's how S3 paths are written everywhere else, and WebDAV has the same fallback for addresses |
| 2 | Should any bucket be allowed by bucket name? | No: an allow list, as for HTTP hosts. A signed request tells the bucket's owner who opened the stage |
| 3 | Is `credential_process` enough for single sign-on? | Yes, for now. AWS's own command line provides it; building the flow means OAuth with AWS's endpoints |
| 4 | Should a 403 from `HEAD` count as missing, for buckets that don't allow listing? | No. Require `s3:ListBucket` and say so in the error; otherwise a typo in a key and a missing permission look the same |
| 5 | Region per endpoint in the configuration, or always found? | Found when left out, which works on AWS only; written for other stores |
