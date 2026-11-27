// The resolver and transport for s3:// (docs/S3_RESOLVER.md). A named protocol, as
// WebDAV: s3://<name>/<path>, the name's stack of S3 endpoints coming from the context.
// When the name isn't one, s3://<bucket>/<key> is that bucket on AWS (§3.2), if the
// bucket is allowed. Requests are signed by libcurl (§2); keys are escaped strictly
// (§8.1); listings are ListObjectsV2.
#include "aws_credentials.h"
#include "xml_names.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/caching_resolver.h"
#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/credentials.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/sources.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include <pxr/pxr.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/resolvedPath.h>

#include <pugixml.hpp>

#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace usdcc::assets;
PXR_NAMESPACE_USING_DIRECTIVE

// ---- Where an endpoint's objects are ------------------------------------------------

// A bucket's region, once an answer named it: by the URL its requests go to.
struct Regions {
    std::mutex mutex;
    std::map<std::string, std::string> byUrl;
};

Regions& regions() {
    static Regions instance;
    return instance;
}

std::string knownRegion(const std::string& url) {
    Regions& all = regions();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = all.byUrl.find(url);
    return found == all.byUrl.end() ? std::string() : found->second;
}

void rememberRegion(const std::string& url, const std::string& region) {
    Regions& all = regions();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.byUrl[url] = region;
}

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A bucket name as S3 has them: 3 to 63 lowercase letters, digits, dots and hyphens,
// starting and ending with a letter or digit.
bool isBucketName(const std::string& name) {
    if (name.size() < 3 || name.size() > 63) return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return std::isalnum(static_cast<unsigned char>(name.front())) && std::isalnum(static_cast<unsigned char>(name.back()));
}

// Servers named by their address that turned out to have no TLS: by origin
// ("https://nas1:9000"), asked over http:// from then on (§3.3).
struct PlainServers {
    std::mutex mutex;
    std::set<std::string> origins;
};

PlainServers& plainServers() {
    static PlainServers instance;
    return instance;
}

bool isPlain(const std::string& origin) {
    PlainServers& all = plainServers();
    std::lock_guard<std::mutex> lock(all.mutex);
    return all.origins.count(origin) > 0;
}

void rememberPlain(const std::string& origin) {
    PlainServers& all = plainServers();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.origins.insert(origin);
}

// Where one path's requests go (§3.1): the bucket, its base URL and the key.
struct Target {
    std::string path;        // the path on the endpoint this is for, as given
    std::string base;        // up to the bucket's root: https://b.s3.eu-central-1.amazonaws.com, http://host:9000/b
    std::string origin;      // scheme://host[:port] of an endpoint that names a server, not a bucket
    std::string bucket;      // empty: the path names none, and listing it lists the server's buckets
    std::string key;         // the path's key in the bucket: "prod/chars/hero.usda"
    std::string region;      // signed for; us-east-1 until an answer names another
    std::string regionKey;   // what the region is remembered by
    bool regionGiven = false;  // the configuration says it
    bool address = false;      // a server named by its address in the asset path (§3.3)
};

std::string trimSlashes(std::string text) {
    while (!text.empty() && text.front() == '/') text.erase(0, 1);
    while (!text.empty() && text.back() == '/') text.pop_back();
    return text;
}

// "prod" + "/chars/hero.usda" -> "prod/chars/hero.usda".
std::string joinKey(const std::string& prefix, const std::string& relative) {
    if (prefix.empty()) return relative;
    return relative.empty() ? prefix : prefix + "/" + relative;
}

// The endpoint's URL is one of (§3.1, §3.3):
//   s3://<bucket>[/<prefix>]                      a bucket on AWS, or where AWS_ENDPOINT_URL_S3 says
//   http(s)://<host>/<bucket>[/<prefix>]           a bucket on an S3-compatible store
//   http(s)://<bucket>.<host>[/<prefix>]           the same, with "bucketInHost": true
//   http(s)://<host>                               a whole server: the path's first part is the bucket
// `path` may spell characters with escapes; the key has them as they are.
bool targetOf(const Endpoint& endpoint, const std::string& path, Target* target, std::string* why) {
    Target made;
    made.path = path;
    const std::string& url = endpoint.url;
    const auto option = [&endpoint](const char* key) {
        const auto found = endpoint.options.find(key);
        return found == endpoint.options.end() ? std::string() : found->second;
    };
    made.region = option("region");
    made.regionGiven = !made.region.empty();
    made.address = option("address") == "true";
    const std::string decoded = percentDecode(path);
    const std::string relative = trimSlashes(decoded);
    if (lowered(url.substr(0, 5)) == "s3://") {
        const std::string rest = url.substr(5);
        const size_t slash = rest.find('/');
        made.bucket = lowered(rest.substr(0, slash));
        if (!isBucketName(made.bucket)) {
            *why = "'" + made.bucket + "' isn't a bucket's name";
            return false;
        }
        made.key = joinKey(slash == std::string::npos ? std::string() : trimSlashes(rest.substr(slash)), relative);
        made.regionKey = "s3://" + made.bucket;
        if (!made.regionGiven) made.region = knownRegion(made.regionKey);
        const std::string override = usdcc::s3::awsEndpointOverride();
        if (!override.empty()) {
            made.base = override + "/" + made.bucket;
        } else if (made.region.empty()) {
            // The global name, until an answer says where the bucket is.
            made.base = made.bucket.find('.') == std::string::npos ? "https://" + made.bucket + ".s3.amazonaws.com"
                                                                    : "https://s3.amazonaws.com/" + made.bucket;
        } else if (made.bucket.find('.') == std::string::npos) {
            made.base = "https://" + made.bucket + ".s3." + made.region + ".amazonaws.com";
        } else {
            // A dot in the name doesn't match AWS's certificate for *.s3.<region>.amazonaws.com.
            made.base = "https://s3." + made.region + ".amazonaws.com/" + made.bucket;
        }
    } else {
        const size_t scheme = url.find("://");
        if (scheme == std::string::npos) {
            *why = "its URL isn't s3://<bucket> or http(s)://<host>[/<bucket>]";
            return false;
        }
        const size_t pathStart = std::min(url.find('/', scheme + 3), url.size());
        std::string origin = url.substr(0, pathStart);
        // A server by its address that has no TLS: as http://, as it answered before.
        if (made.address && isPlain(origin)) origin = "http://" + origin.substr(scheme + 3);
        const std::string urlPath = trimSlashes(url.substr(pathStart));
        if (option("bucketInHost") == "true") {
            const std::string host = url.substr(scheme + 3, pathStart - scheme - 3);
            made.bucket = lowered(host.substr(0, host.find('.')));
            made.key = joinKey(urlPath, relative);
            made.base = origin;
        } else if (!urlPath.empty()) {
            const size_t slash = urlPath.find('/');
            made.bucket = urlPath.substr(0, slash);
            made.key = joinKey(slash == std::string::npos ? std::string() : trimSlashes(urlPath.substr(slash)), relative);
            made.base = origin + "/" + made.bucket;
        } else {
            // A whole server: the bucket is the path's first part, none for its root.
            made.origin = origin;
            const size_t slash = relative.find('/');
            made.bucket = relative.substr(0, slash);
            made.key = slash == std::string::npos ? std::string() : relative.substr(slash + 1);
            made.base = made.bucket.empty() ? origin : origin + "/" + percentEncodeStrict(made.bucket, false);
        }
        made.regionKey = url.substr(0, pathStart) + "/" + made.bucket;
        if (!made.regionGiven) made.region = knownRegion(made.regionKey);
    }
    if (made.region.empty()) made.region = "us-east-1";
    *target = std::move(made);
    return true;
}

// ---- Requests --------------------------------------------------------------------------

// S3's error: <Error><Code>AccessDenied</Code><Message>Access Denied</Message>...</Error>.
struct S3Error {
    std::string code;
    std::string message;
    std::string region;
};

S3Error errorOf(const HttpResponse& response) {
    S3Error error;
    pugi::xml_document document;
    if (response.body.empty() || !document.load_buffer(response.body.data(), response.body.size())) return error;
    const pugi::xml_node node = xml::childNamed(document, "Error");
    error.code = xml::textOf(node, "Code");
    error.message = xml::textOf(node, "Message");
    error.region = xml::textOf(node, "Region");
    return error;
}

// "HTTP 403 AccessDenied: Access Denied", and what to do about it where that's known.
std::string describe(const HttpResponse& response, const std::string& hint = {}) {
    if (response.status != Status::Ok) return response.message;
    std::string text = "HTTP " + std::to_string(response.code);
    const S3Error error = errorOf(response);
    if (!error.code.empty()) text += " " + error.code + (error.message.empty() ? "" : ": " + error.message);
    if (!hint.empty()) text += " (" + hint + ")";
    return text;
}

// What a refusal says about the credential: where AWS's lookup found one, or didn't.
std::string credentialHint(const Endpoint& endpoint) {
    if (endpoint.credential.empty()) return "sent unsigned: the endpoint names no credential";
    if (!usdcc::s3::isAwsCredentialId(endpoint.credential)) {
        return credentialFor(endpoint.credential) ? "signed with credential '" + endpoint.credential + "'"
                                                  : "sent unsigned: nothing is saved under credential '" + endpoint.credential +
                                                        "' (Edit > Asset Servers > Logins)";
    }
    std::string source;
    const bool found = usdcc::s3::awsCredential(endpoint.credential, &source).has_value();
    return found ? "signed with the AWS credentials of " + source : "sent unsigned: no AWS credentials, looked for in " + source;
}

template <class T>
Result<T> failureOf(const HttpResponse& response, const std::string& hint = {}) {
    if (response.status != Status::Ok) return Result<T>::failure(response.status, response.message);
    Status status = statusOfHttpCode(response.code);
    // A redirect S3 didn't name a region for, or a 400: neither is the file being missing.
    if (status == Status::Ok) status = Status::Error;
    return Result<T>::failure(status, describe(response, hint));
}

// For a bucket named by itself, which goes to AWS: a word should the user have meant a
// server on the network, as s3://nas1/ looks like one (§3.3).
template <class T>
Result<T> withBucketHint(Result<T> result, const Endpoint& endpoint) {
    const auto byBucket = endpoint.options.find("byBucket");
    if (result.ok() || byBucket == endpoint.options.end() || result.status == Status::Missing) return result;
    result.message += ". s3://" + endpoint.id + "/ is the bucket '" + endpoint.id +
                      "' on AWS; a server on your network takes its address with the port: s3://" + endpoint.id +
                      ":<port>/<bucket>/";
    return result;
}

class S3Transport final : public Transport {
public:
    // A HEAD of the key. Resolving asks this for each file, not a listing of its folder
    // (list() isn't supported): one cheap request, where a folder of thousands is a request
    // per thousand, each one slow on some servers (§5).
    Result<Entry> stat(const Endpoint& endpoint, const std::string& path, Cancel& cancel) override {
        Target target;
        std::string why;
        if (!targetOf(endpoint, path, &target, &why)) return Result<Entry>::failure(Status::Error, why);
        if (target.bucket.empty() || target.key.empty() || trimSlashes(percentDecode(path)).empty()) {
            // A server's root, or a bucket's: folders.
            Entry root;
            root.name = target.bucket;
            root.isFolder = true;
            return Result<Entry>::success(std::move(root));
        }
        const HttpResponse response = send(endpoint, target, "HEAD", objectPath(target.key), {}, {}, cancel);
        if (response.status == Status::Ok && response.code == 200) return Result<Entry>::success(entryOf(response, path));
        if (response.status == Status::Ok && response.code == 404) return Result<Entry>::failure(Status::Missing);
        return withBucketHint(failureOf<Entry>(response, response.code == 403 ? credentialHint(endpoint) +
                                                                                    "; a HEAD's answer gives no reason; "
                                                                                    "reading needs s3:GetObject, and a "
                                                                                    "missing key s3:ListBucket"
                                                                              : std::string()),
                              endpoint);
    }

    bool canList() const override { return true; }

    Result<std::vector<Entry>> list(const Endpoint&, const std::string&, Cancel&) override {
        return Result<std::vector<Entry>>::failure(Status::NotSupported, "S3 asks for each file");
    }

    Result<EntryPage> listPage(const Endpoint& endpoint, const std::string& folder, const std::string& from,
                               Cancel& cancel) override {
        using Paged = Result<EntryPage>;
        Target target;
        std::string why;
        if (!targetOf(endpoint, folder, &target, &why)) return Paged::failure(Status::Error, why);
        EntryPage page;
        if (target.bucket.empty()) {
            if (!from.empty()) return Paged::success(std::move(page));
            Result<std::vector<Entry>> buckets = listBuckets(endpoint, target, cancel);
            if (!buckets.ok()) return Paged::failure(buckets.status, buckets.message);
            page.entries = std::move(buckets.value);
            return Paged::success(std::move(page));
        }
        bool marker = false;
        const Result<bool> listed = listOnePage(endpoint, target, target.key, from, cancel, &page.entries, &marker, &page.next);
        if (!listed.ok()) return withBucketHint(Paged::failure(listed.status, listed.message), endpoint);
        // S3 has no folders, only keys, so a folder with none in it and one that isn't there
        // are the same: empty. Some servers list a folder that has none, too: one that keeps
        // objects as files lists an empty directory (S3_RESOLVER.md §8.2).
        return Paged::success(std::move(page));
    }

    Result<Entry> fetch(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& into,
                        const std::string& knownEtag, Progress& progress, Cancel& cancel) override {
        Target target;
        std::string why;
        if (!targetOf(endpoint, path, &target, &why)) return Result<Entry>::failure(Status::Error, why);
        if (target.bucket.empty() || target.key.empty()) return Result<Entry>::failure(Status::Missing);
        std::vector<std::string> headers;
        if (!knownEtag.empty()) headers.push_back("If-None-Match: " + knownEtag);
        const HttpResponse response = send(endpoint, target, "GET", objectPath(target.key), headers, into, cancel, &progress);
        if (response.status != Status::Ok || statusOfHttpCode(response.code) != Status::Ok) {
            if (response.code == 304) return Result<Entry>::failure(Status::NotModified);
            return failureOf<Entry>(response, response.code == 403 ? credentialHint(endpoint) : std::string());
        }
        return Result<Entry>::success(entryOf(response, path));
    }

    bool canPut() const override { return true; }

    // A signed PUT of the key, only if the object is the version `knownEtag` names, or
    // isn't there at all: S3's conditional writes (docs/S3_RESOLVER.md §5). S3 has no
    // folders to make.
    Result<Entry> put(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& from,
                      const std::string& knownEtag, Progress& progress, Cancel& cancel) override {
        Target target;
        std::string why;
        if (!targetOf(endpoint, path, &target, &why)) return Result<Entry>::failure(Status::Error, why);
        if (target.bucket.empty() || target.key.empty() || target.key.back() == '/') {
            return Result<Entry>::failure(Status::Error, "a file is written to a key in a bucket, and " + path + " names none");
        }
        std::vector<std::string> headers;
        if (knownEtag.empty()) {
            headers.push_back("If-None-Match: *");
        } else if (knownEtag != "*") {
            headers.push_back("If-Match: " + knownEtag);
        }
        const HttpResponse response =
            send(endpoint, target, "PUT", objectPath(target.key), headers, {}, cancel, &progress, from);
        if (response.status == Status::Ok && statusOfHttpCode(response.code) == Status::Conflict) {
            // 412 PreconditionFailed, or 409 ConditionalRequestConflict: another write got there first.
            return Result<Entry>::failure(Status::Conflict, describe(response));
        }
        if (response.status != Status::Ok || statusOfHttpCode(response.code) != Status::Ok) {
            return failureOf<Entry>(response, response.code == 403 ? credentialHint(endpoint) + "; writing needs s3:PutObject"
                                                                   : std::string());
        }
        Entry entry = entryOf(response, path);
        std::error_code error;
        entry.size = std::filesystem::file_size(from, error);
        // S3 answers a PUT with the ETag; its time comes from a HEAD, as a listing has it.
        const Result<Entry> now = stat(endpoint, path, cancel);
        if (now.ok()) entry = now.value;
        return Result<Entry>::success(std::move(entry));
    }

private:
    static std::string objectPath(const std::string& key) { return "/" + percentEncodeStrict(key, true); }

    static Entry entryOf(const HttpResponse& response, const std::string& path) {
        Entry entry;
        entry.name = percentDecode(path.substr(path.rfind('/') + 1));
        entry.etag = response.header("etag");
        entry.modified = parseHttpDate(response.header("last-modified"));
        const std::string length = response.header("content-length");
        if (!length.empty() && std::isdigit(static_cast<unsigned char>(length[0]))) entry.size = std::stoull(length);
        return entry;
    }

    // One request to the endpoint's bucket, signed as its credential says. When S3 answers
    // that the bucket is in another region, that region is remembered and asked again.
    static HttpResponse send(const Endpoint& endpoint, Target& target, const std::string& method, const std::string& path,
                             const std::vector<std::string>& headers, const std::filesystem::path& into, Cancel& cancel,
                             Progress* progress = nullptr, const std::filesystem::path& uploadFrom = {}) {
        HttpResponse response;
        for (int attempt = 0; attempt < 3; ++attempt) {
            HttpRequest request;
            request.method = method;
            request.url = target.base + path;
            request.headers = headers;
            request.into = into;
            request.uploadFrom = uploadFrom;
            // A signature gives no secret away over plain http; a session token would go
            // as it is, with a warning in the Log.
            request.credentialOverHttp = true;
            if (!endpoint.credential.empty()) {
                if (usdcc::s3::isAwsCredentialId(endpoint.credential)) {
                    request.explicitCredential = usdcc::s3::awsCredential(endpoint.credential);
                } else {
                    request.credential = endpoint.credential;
                }
                request.awsSigV4 = "aws:amz:" + target.region + ":s3";
            }
            Progress none;
            response = httpRequest(request, progress ? *progress : none, cancel);
            // A server by its address that has no TLS (a NAS on the network, say) is asked
            // over http:// instead, from now on: only when https:// got no TLS at all.
            if (response.tlsFailed && target.address && attempt == 0 && target.base.rfind("https://", 0) == 0) {
                const std::string origin = endpoint.url.substr(0, std::min(endpoint.url.find('/', 8), endpoint.url.size()));
                rememberPlain(origin);
                TF_WARN("%s has no TLS: asked over plain http:// from now on", origin.substr(8).c_str());
                std::string why;
                targetOf(endpoint, target.path, &target, &why);
                continue;
            }
            if (response.status != Status::Ok || target.regionGiven || attempt > 1) break;
            if (response.code != 301 && response.code != 307 && response.code != 400) break;
            std::string region = response.header("x-amz-bucket-region");
            if (region.empty()) region = errorOf(response).region;
            if (region.empty() && method == "HEAD") {
                // A HEAD's refusal has no body to name the region in (Garage sends no header
                // either): the first byte asked for instead, whose refusal does.
                HttpRequest probe = request;
                probe.method = "GET";
                probe.into.clear();
                probe.headers.push_back("Range: bytes=0-0");
                Progress quiet;
                const HttpResponse asked = httpRequest(probe, quiet, cancel);
                region = asked.header("x-amz-bucket-region");
                if (region.empty()) region = errorOf(asked).region;
            }
            if (region.empty() || region == target.region) break;
            // A downloaded error body isn't the file.
            if (!into.empty()) {
                std::error_code error;
                std::filesystem::remove(into, error);
            }
            rememberRegion(target.regionKey, region);
            std::string why;
            targetOf(endpoint, target.path, &target, &why);
        }
        return response;
    }

    // A server's buckets, as folders: ListBuckets (GET /).
    static Result<std::vector<Entry>> listBuckets(const Endpoint& endpoint, Target& target, Cancel& cancel) {
        using Listed = Result<std::vector<Entry>>;
        const HttpResponse response = send(endpoint, target, "GET", "/", {}, {}, cancel);
        if (response.status != Status::Ok || response.code != 200) {
            return failureOf<std::vector<Entry>>(
                response, response.code == 403 ? credentialHint(endpoint) + "; listing the buckets needs s3:ListAllMyBuckets"
                                               : std::string());
        }
        pugi::xml_document document;
        if (!document.load_buffer(response.body.data(), response.body.size())) return Listed::failure(Status::Error, "its answer isn't XML");
        const pugi::xml_node result = xml::childNamed(document, "ListAllMyBucketsResult");
        if (!result) return Listed::failure(Status::Error, "its answer isn't a ListAllMyBucketsResult");
        std::vector<Entry> buckets;
        for (const pugi::xml_node& node : xml::childNamed(result, "Buckets").children()) {
            if (std::strcmp(xml::localName(node), "Bucket") != 0) continue;
            Entry bucket;
            bucket.name = xml::textOf(node, "Name");
            bucket.isFolder = true;
            bucket.modified = parseIsoDate(xml::textOf(node, "CreationDate"));
            if (!bucket.name.empty()) buckets.push_back(std::move(bucket));
        }
        return Listed::success(std::move(buckets));
    }

    // One page of ListObjectsV2 of the folder `key` ("prod/chars"; "" for the bucket's root),
    // from `token` (empty: the start). `next` is where the next page starts, empty when that
    // was the last. `marker` says the folder's own empty object was among the keys.
    static Result<bool> listOnePage(const Endpoint& endpoint, Target& target, const std::string& key, const std::string& token,
                                    Cancel& cancel, std::vector<Entry>* entries, bool* marker, std::string* next) {
        const std::string prefix = key.empty() ? std::string() : key + "/";
        std::string query = "/?list-type=2&delimiter=%2F&prefix=" + percentEncodeStrict(prefix, false);
        if (!token.empty()) query += "&continuation-token=" + percentEncodeStrict(token, false);
        const HttpResponse response = send(endpoint, target, "GET", query, {}, {}, cancel);
        if (response.status != Status::Ok || response.code != 200) {
            return failureOf<bool>(response, response.code == 403 ? credentialHint(endpoint) + "; listing needs s3:ListBucket"
                                                                  : std::string());
        }
        pugi::xml_document document;
        if (!document.load_buffer(response.body.data(), response.body.size())) {
            return Result<bool>::failure(Status::Error, "its listing isn't XML");
        }
        const pugi::xml_node result = xml::childNamed(document, "ListBucketResult");
        if (!result) return Result<bool>::failure(Status::Error, "its answer isn't a ListBucketResult");
        for (const pugi::xml_node& node : result.children()) {
            const char* name = xml::localName(node);
            if (std::strcmp(name, "Contents") == 0) {
                const std::string full = xml::textOf(node, "Key");
                if (full == prefix) {
                    *marker = true;
                    continue;
                }
                const std::string own = full.substr(std::min(prefix.size(), full.size()));
                if (own.empty() || own.find('/') != std::string::npos) continue;
                Entry entry;
                entry.name = own;
                entry.etag = xml::textOf(node, "ETag");
                entry.modified = parseIsoDate(xml::textOf(node, "LastModified"));
                const std::string size = xml::textOf(node, "Size");
                if (!size.empty() && std::isdigit(static_cast<unsigned char>(size[0]))) entry.size = std::stoull(size);
                entries->push_back(std::move(entry));
            } else if (std::strcmp(name, "CommonPrefixes") == 0) {
                std::string own = xml::textOf(node, "Prefix");
                own = own.substr(std::min(prefix.size(), own.size()));
                while (!own.empty() && own.back() == '/') own.pop_back();
                if (own.empty() || own.find('/') != std::string::npos) continue;
                Entry entry;
                entry.name = own;
                entry.isFolder = true;
                entries->push_back(std::move(entry));
            }
        }
        const bool truncated = std::string(xml::textOf(result, "IsTruncated")) == "true";
        next->clear();
        // Not trusting a server to move on (rclone doesn't, §8.1): the token it was given
        // again ends the listing, as a page with nothing new does for the caller.
        if (truncated) {
            const std::string given = xml::textOf(result, "NextContinuationToken");
            if (given != token) *next = given;
        }
        return Result<bool>::success(true);
    }
};

// ---- A server by its address (§3.3) ---------------------------------------------------

// s3://<host>[:<port>]/<bucket>/<key>, as WebDAV's addresses (WEBDAV_RESOLVER.md §2.1).
struct AddressUrl {
    std::string host;  // lowercase, without the port
    std::string port;  // empty for none
    std::string path;  // normalized: "/<bucket>/<key>"

    std::string authority() const { return port.empty() ? host : host + ":" + port; }
    std::string text() const { return "s3://" + authority() + path; }
};

bool isIpv4(const std::string& host) {
    int dots = 0;
    for (const char c : host) {
        if (c == '.') {
            ++dots;
        } else if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return dots == 3;
}

// An address, not a bucket: it has a port (no bucket name has a ':'), is an IP address
// (no bucket name may be one), or is localhost. "nas1" alone is a bucket on AWS.
bool parseAddress(const std::string& text, AddressUrl* url) {
    if (lowered(text.substr(0, 5)) != "s3://" || text.find_first_of("?#") != std::string::npos) return false;
    const size_t slash = std::min(text.find('/', 5), text.size());
    std::string authority = text.substr(5, slash - 5);
    if (authority.empty() || authority.find_first_of("@~") != std::string::npos) return false;
    AddressUrl parsed;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']', colon) == std::string::npos) {
        parsed.port = authority.substr(colon + 1);
        authority.erase(colon);
        if (parsed.port.empty() || parsed.port.find_first_not_of("0123456789") != std::string::npos) return false;
    }
    parsed.host = lowered(authority);
    if (parsed.host.empty()) return false;
    if (parsed.port.empty() && !isIpv4(parsed.host) && parsed.host != "localhost" && parsed.host.front() != '[') return false;
    if (!normalizePath(text.substr(slash), &parsed.path)) return false;
    *url = std::move(parsed);
    return true;
}

// What an address means: one endpoint for the whole server, its buckets the path's first
// part, its login saved under the address unless the configuration names one for the host.
bool addressEndpoint(const std::string& uri, Endpoint* endpoint, std::string* path, std::string* normalized) {
    AddressUrl url;
    if (!parseAddress(uri, &url)) return false;
    Endpoint made;
    made.id = url.authority();
    made.scheme = "s3";
    // https:// first; a server without TLS is asked over http:// once it showed it has none.
    made.url = "https://" + url.authority();
    made.options["address"] = "true";
    made.readOnly = true;
    const Config config = defaultConfig();
    const auto host = config.httpHosts.find(url.host);
    if (host != config.httpHosts.end()) made.credential = host->second.credential;
    if (made.credential.empty()) made.credential = credentialIdForAddress(url.host, url.port);
    *endpoint = std::move(made);
    *path = url.path;
    *normalized = url.text();
    return true;
}

// ---- A bucket named by itself (§3.2) ---------------------------------------------------

// s3://<bucket>/<path> as one endpoint: the bucket on AWS, with AWS's own credential
// lookup. False for anything else.
bool bucketEndpoint(const std::string& uri, Endpoint* endpoint, std::string* path, std::string* normalized) {
    if (lowered(uri.substr(0, 5)) != "s3://" || uri.find_first_of("?#") != std::string::npos) return false;
    const std::string rest = uri.substr(5);
    const size_t slash = std::min(rest.find('/'), rest.size());
    const std::string bucket = lowered(rest.substr(0, slash));
    if (!isBucketName(bucket)) return false;
    std::string normalizedPath;
    if (!normalizePath(rest.substr(slash), &normalizedPath)) return false;
    Endpoint made;
    made.id = bucket;
    made.scheme = "s3";
    made.url = "s3://" + bucket;
    made.credential = "aws";
    made.options["byBucket"] = "true";
    made.readOnly = true;
    *endpoint = std::move(made);
    *path = normalizedPath;
    *normalized = "s3://" + bucket + normalizedPath;
    return true;
}

// Whether `name` is a server name of s3:// in any environment of the default
// configuration, or the id of an S3 server, which is a name by itself
// (docs/REMOTE_ASSETS.md §4.1): such a name is never taken for a bucket.
bool isConfiguredName(const std::string& name) {
    const Config config = defaultConfig();
    for (const auto& [id, endpoint] : config.endpoints) {
        // In its own configuration's environments: yours, or a source's ("studio-a/lib"), which
        // `id` stands for here, having the same alias.
        if (endpoint.scheme == "s3" && ownName(id, id) == name) return true;
    }
    for (const auto& [environment, schemes] : config.environments) {
        const auto s3 = schemes.find("s3");
        if (s3 != schemes.end() && s3->second.count(name)) return true;
    }
    return false;
}

const struct RegisterTransport {
    RegisterTransport() {
        registerTransport("s3", std::make_shared<S3Transport>());
        registerAddressParser("s3", [](const std::string& uri, Endpoint* endpoint, std::string* path, std::string* normalized) {
            if (addressEndpoint(uri, endpoint, path, normalized)) return true;
            return bucketEndpoint(uri, endpoint, path, normalized) && !isConfiguredName(endpoint->id);
        });
    }
} kRegisterTransport;

}  // namespace

PXR_NAMESPACE_OPEN_SCOPE

class UsdccS3Resolver final : public usdcc::assets::CachingResolver {
public:
    UsdccS3Resolver() : CachingResolver("s3") {}

protected:
    // A name the environment knows, as every named protocol; else, when it isn't a name of
    // any environment, a bucket on AWS, if it's allowed.
    bool _Locate(const std::string& assetPath, Location* location, std::string* why) const override {
        if (CachingResolver::_Locate(assetPath, location, why)) return true;
        usdcc::assets::Endpoint endpoint;
        std::string path;
        std::string normalized;
        AddressUrl address;
        if (parseAddress(assetPath, &address)) {
            // A stage can name any address, as with plain HTTP (docs/HTTP_RESOLVER.md §4).
            if (!usdcc::assets::hostAllowed(address.host)) {
                *why = "the host '" + address.host +
                       "' isn't one this stage may contact (Settings > Assets > Allowed hosts, the configuration's \"http\" "
                       "\"allow\" list, or USDCC_HTTP_ALLOW)";
                return false;
            }
            addressEndpoint(assetPath, &endpoint, &path, &normalized);
            usdcc::assets::Stack stack;
            stack.scheme = "s3";
            stack.name = endpoint.id;
            stack.endpoints.push_back(std::move(endpoint));
            location->path = path;
            location->resolved = normalized;
            location->display = normalized;
            location->stack = std::make_shared<const usdcc::assets::Stack>(std::move(stack));
            return true;
        }
        if (!bucketEndpoint(assetPath, &endpoint, &path, &normalized) || isConfiguredName(endpoint.id)) return false;
        // A signed request tells the bucket's owner who sent it (docs/S3_RESOLVER.md §3.2).
        if (!usdcc::assets::bucketAllowed(endpoint.id)) {
            *why = "the bucket '" + endpoint.id +
                   "' isn't one a stage may name by itself (Settings > Assets > Allowed buckets, the configuration's "
                   "\"s3\" \"allow\" list, or USDCC_S3_ALLOW)";
            return false;
        }
        usdcc::assets::Stack stack;
        stack.scheme = "s3";
        stack.name = endpoint.id;
        stack.endpoints.push_back(std::move(endpoint));
        location->path = path;
        location->resolved = normalized;
        location->display = normalized;
        location->stack = std::make_shared<const usdcc::assets::Stack>(std::move(stack));
        return true;
    }

    // An address isn't a name, so it's anchored here: on the same server.
    std::string _CreateIdentifier(const std::string& assetPath, const ArResolvedPath& anchorAssetPath) const override {
        AddressUrl url;
        if (assetPath.find("://") != std::string::npos) {
            return parseAddress(assetPath, &url) ? url.text() : CachingResolver::_CreateIdentifier(assetPath, anchorAssetPath);
        }
        if (assetPath.empty() || !TfIsRelativePath(assetPath) || !parseAddress(anchorAssetPath.GetPathString(), &url)) {
            return CachingResolver::_CreateIdentifier(assetPath, anchorAssetPath);
        }
        const std::string folder = url.path.substr(0, url.path.rfind('/') + 1);
        if (!normalizePath(folder + assetPath, &url.path)) return assetPath;
        return url.text();
    }
};

AR_DEFINE_RESOLVER(UsdccS3Resolver, ArResolver);

PXR_NAMESPACE_CLOSE_SCOPE
