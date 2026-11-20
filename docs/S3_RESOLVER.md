# S3 Resolver (plan)

A USD asset resolver plugin that loads assets from Amazon S3 and the stores that speak its API,
through `s3://` asset paths such as `s3://studio/chars/hero/hero.usd`.

**Status:** planned (2026-11-20), nothing built. The caching, the server names, the environments
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
- **Read-only first.** Writing comes with the shared library's phase 6 (§8).

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
- **`region`** is part of the signature. On AWS it may be left out: S3 names a bucket's region in
  the `x-amz-bucket-region` header of any answer about it, even a refusal, and it's remembered for
  the session. Other stores get `us-east-1` unless they say otherwise.
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
- **Only AWS.** A bucket on another store needs its URL, so a name in the configuration.

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
| `list` | `GET /?list-type=2&prefix=<folder>/&delimiter=/` (`ListObjectsV2`) | `Contents` are files (`Key`, `Size`, `LastModified`, `ETag`), `CommonPrefixes` folders. A page holds 1,000; `continuation-token` asks for the next. The empty `folder/` objects some tools make for folders are left out |
| `stat` | `HEAD /<key>` | `Content-Length`, `Last-Modified`, `ETag`. Used where a listing isn't allowed (below) |
| `fetch` | `GET /<key>`, with `If-None-Match` | A 304 writes nothing |
| `put` (phase 6) | `PUT /<key>`, with `If-None-Match: *` for a new file and `If-Match` over an old one | S3 has had conditional writes since 2024, so saving over a file changed meanwhile is the clear error phase 6 plans |

- **Folders don't exist in S3,** only keys with slashes. A folder is there when a key starts with
  it. A file asked for as a folder, or the other way round, is told apart by the listing.
- **ETags:** an object uploaded in parts has an ETag ending in `-<parts>` that isn't its MD5. It
  still changes with the content, which is all the cache needs.
- **Resolving lists folders,** as WebDAV does: one request answers for every file in a folder, and
  tells "not in TEST" from "TEST is down" cheaply.
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
| **S3.0 Spike** | Signed `HEAD`, `GET` and `ListObjectsV2` through `httpRequest`, against AWS and MinIO | Keys with a space, `+`, `=`, `%`, `~` and accented letters read on both; a listing of more than 1,000 keys pages; a region is found from a refusal; a session token works |
| **S3.1 Read-only** | §6, the transport and plugin, by name, with usdcc credential ids; S3 in the test server (§10) | `usdcat s3://studio/shot.usda` prints a shot whose model comes from PROD and one texture from TEST, with a listing per folder and nothing downloaded on the second open |
| **S3.2 AWS credentials** | §4.2: the environment, the shared files, `credential_process`, EC2's role, renewal | A profile using AWS single sign-on through `credential_process` opens a stage, and keeps working past its credentials' expiry |
| **S3.3 By bucket** | §3.2, finding the region, Allowed buckets | `usdcat s3://<bucket>/shot.usda` works with only the AWS command line set up; a stage naming another bucket is refused with the toast |
| **S3.4 usdcc** | §7 | A reference to a file in a bucket is added by browsing to it in the picker |
| **With phase 6** | `put` (§5) | Save on a layer from PROD, in the test environment, writes it to the TEST bucket |
| **Later** | Reading part of a large file; pinning an object version (`versionId`); requester pays | |

## 9. Not planned

- **AWS's single sign-on flow itself,** and assuming roles (`role_arn` with `source_profile`):
  `credential_process` covers both.
- **Uploads in parts,** needed above 5 GB per file. A layer or a texture isn't that big.
- **SSE-C,** encryption with keys the client supplies.

## 10. Testing

- **Test server** ([assets_test_server.h](../tests/assets_test_server.h)) gains S3's requests:
  `ListObjectsV2` with paging over a folder, `HEAD`, `GET` and 304, and a switch for 403 on a
  missing key. It checks that a request carries a SigV4 `Authorization` header, not that the
  signature is right: that would mean writing SigV4 a second time.
- **Tests** in [assets_net_test.cpp](../tests/assets_net_test.cpp), as WebDAV's: list, stat and
  fetch with awkward keys, a missing folder, a 304, paging; a stage through `UsdStage::Open` from
  two buckets; the 403 message; the shared files and `credential_process` from a test folder.
- **Signatures for real:** a test that runs only when `USDCC_TEST_S3_URL` and its credentials are
  set, against MinIO (`minio server <folder>`, one binary, no install) or a real bucket. Run by hand
  before a release, as the WebDAV servers are.

## 11. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should `s3://bucket/key` work beside names? | Yes (§3.2): it's how S3 paths are written everywhere else, and WebDAV has the same fallback for addresses |
| 2 | Should any bucket be allowed by bucket name? | No: an allow list, as for HTTP hosts. A signed request tells the bucket's owner who opened the stage |
| 3 | Is `credential_process` enough for single sign-on? | Yes, for now. AWS's own command line provides it; building the flow means OAuth with AWS's endpoints |
| 4 | Should a 403 from `HEAD` count as missing, for buckets that don't allow listing? | No. Require `s3:ListBucket` and say so in the error; otherwise a typo in a key and a missing permission look the same |
| 5 | Region per endpoint in the configuration, or always found? | Found when left out, which works on AWS only; written for other stores |
