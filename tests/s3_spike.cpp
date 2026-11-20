// The S3 spike (docs/S3_RESOLVER.md §8, phase S3.0): signed HEAD, GET and
// ListObjectsV2 through usdcc_assets' httpRequest, against a real S3 or one that
// speaks it. Not a test: it prints what each request got, for the plan's findings.
//
//     usdcc_s3_spike --url http://127.0.0.1:8333 --bucket spike [--region us-east-1]
//                    [--host-style] [--expect <folder>] [--many <prefix>]
//
// Credentials from USDCC_SPIKE_S3_KEY, USDCC_SPIKE_S3_SECRET and, for temporary ones,
// USDCC_SPIKE_S3_TOKEN. With --expect, each object's bytes are compared with the file
// of its key in <folder>/<bucket>.

#include "usdcc/assets/credentials.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace usdcc::assets;

namespace {

struct Options {
    std::string url;  // http://127.0.0.1:8333, or https://s3.eu-central-1.amazonaws.com
    std::string bucket;
    std::string region = "us-east-1";
    bool hostStyle = false;  // the bucket in the host name, as AWS prefers
    fs::path expect;
    std::string many;  // a prefix holding more than 1,000 keys, to page through
};

int failures = 0;

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// Every byte but A-Z a-z 0-9 - . _ ~ and '/' escaped: what S3's canonical request
// wants of a key.
std::string strictEncode(const std::string& text, bool keepSlash) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || (keepSlash && c == '/')) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string bucketUrl(const Options& options) {
    if (!options.hostStyle) return options.url + "/" + options.bucket;
    const size_t scheme = options.url.find("://");
    return options.url.substr(0, scheme + 3) + options.bucket + "." + options.url.substr(scheme + 3);
}

std::optional<Credential> credential(bool wrongSecret = false) {
    Credential found;
    found.user = env("USDCC_SPIKE_S3_KEY");
    found.password = env("USDCC_SPIKE_S3_SECRET");
    found.token = env("USDCC_SPIKE_S3_TOKEN");
    if (found.user.empty()) return std::nullopt;
    if (wrongSecret) found.password += "x";
    return found;
}

HttpResponse send(const Options& options, const std::string& method, const std::string& url, bool sign = true,
                  bool wrongSecret = false, const std::string& region = {}) {
    HttpRequest request;
    request.method = method;
    request.url = url;
    if (sign) {
        request.explicitCredential = credential(wrongSecret);
        request.awsSigV4 = "aws:amz:" + (region.empty() ? options.region : region) + ":s3";
        request.credentialOverHttp = true;
    }
    Progress progress;
    Cancel cancel;
    return httpRequest(request, progress, cancel);
}

// S3's error body: <Error><Code>…</Code><Message>…</Message></Error>.
std::string s3Error(const HttpResponse& response) {
    pugi::xml_document document;
    if (!document.load_string(response.body.c_str())) return response.body.substr(0, 120);
    const pugi::xml_node error = document.child("Error");
    if (!error) return "";
    return std::string(error.child_value("Code")) + ": " + error.child_value("Message");
}

void report(const char* what, bool ok, const std::string& detail) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what, detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

std::string describe(const HttpResponse& response) {
    if (response.status != Status::Ok) return "no answer: " + response.message;
    std::string text = "HTTP " + std::to_string(response.code);
    const std::string error = s3Error(response);
    if (!error.empty() && response.code >= 300) text += " " + error;
    return text;
}

struct Listed {
    std::vector<std::string> keys;
    std::vector<std::string> folders;
    int pages = 0;
    bool ok = true;
    std::string message;
};

// ListObjectsV2 of one folder, every page. The query's parameters in the order given:
// sorted, or not, to see whether libcurl sorts them for the signature.
Listed list(const Options& options, const std::string& prefix, bool sortedQuery = true, int maxKeys = 0) {
    Listed listed;
    std::string token;
    do {
        std::vector<std::pair<std::string, std::string>> query = {
            {"list-type", "2"}, {"prefix", prefix}, {"delimiter", "/"}};
        if (maxKeys) query.push_back({"max-keys", std::to_string(maxKeys)});
        if (!token.empty()) query.push_back({"continuation-token", token});
        if (sortedQuery) std::sort(query.begin(), query.end());
        std::string url = bucketUrl(options) + "/?";
        for (size_t i = 0; i < query.size(); ++i) {
            url += (i ? "&" : "") + query[i].first + "=" + strictEncode(query[i].second, false);
        }
        const HttpResponse response = send(options, "GET", url);
        ++listed.pages;
        if (response.status != Status::Ok || response.code != 200) {
            listed.ok = false;
            listed.message = describe(response);
            return listed;
        }
        pugi::xml_document document;
        document.load_string(response.body.c_str());
        const pugi::xml_node result = document.child("ListBucketResult");
        for (const pugi::xml_node& each : result.children("Contents")) listed.keys.push_back(each.child_value("Key"));
        for (const pugi::xml_node& each : result.children("CommonPrefixes")) listed.folders.push_back(each.child_value("Prefix"));
        token = std::string(result.child_value("IsTruncated")) == "true" ? result.child_value("NextContinuationToken") : "";
    } while (!token.empty() && listed.pages < 100);
    return listed;
}

std::string readFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

fs::path fromUtf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--url") options.url = next();
        else if (arg == "--bucket") options.bucket = next();
        else if (arg == "--region") options.region = next();
        else if (arg == "--host-style") options.hostStyle = true;
        else if (arg == "--expect") options.expect = fromUtf8(next());
        else if (arg == "--many") options.many = next();
    }
    if (options.url.empty() || options.bucket.empty()) {
        std::fprintf(stderr, "usage: usdcc_s3_spike --url <url> --bucket <bucket> [--region r] [--host-style] [--expect dir] [--many prefix]\n");
        return 2;
    }
    std::printf("S3 spike: %s, region %s, %s, %s\n", bucketUrl(options).c_str(), options.region.c_str(),
                credential() ? "signed" : "no credentials", env("USDCC_SPIKE_S3_TOKEN").empty() ? "no session token" : "with a session token");

    std::printf("\nListing the bucket's root\n");
    const Listed root = list(options, "");
    report("ListObjectsV2, sorted query", root.ok, root.ok ? std::to_string(root.keys.size()) + " keys, " +
                                                               std::to_string(root.folders.size()) + " folders" : root.message);
    const Listed unsorted = list(options, "", false);
    report("ListObjectsV2, query not sorted (does libcurl sort it?)", unsorted.ok, unsorted.ok ? "" : unsorted.message);
    const Listed paged = list(options, "", true, 2);
    report("paging, two keys a page", paged.ok && paged.keys.size() == root.keys.size() && paged.pages > 1,
           std::to_string(paged.pages) + " pages, " + std::to_string(paged.keys.size()) + " keys" + (paged.ok ? "" : ": " + paged.message));
    if (!options.many.empty()) {
        const Listed many = list(options, options.many);
        report("a folder of more than 1,000 keys", many.ok && many.pages > 1 && many.keys.size() > 1000,
               std::to_string(many.pages) + " pages, " + std::to_string(many.keys.size()) + " keys" + (many.ok ? "" : ": " + many.message));
    }

    std::printf("\nEach key in the root, HEAD and GET\n");
    for (const std::string& key : root.keys) {
        std::printf(" %s\n", key.c_str());
        const HttpResponse strictHead = send(options, "HEAD", bucketUrl(options) + "/" + strictEncode(key, true));
        report("HEAD, every reserved character escaped", strictHead.status == Status::Ok && strictHead.code == 200,
               describe(strictHead) + ", ETag " + strictHead.header("etag"));
        const HttpResponse pcharHead = send(options, "HEAD", bucketUrl(options) + "/" + percentEncodePath(key));
        if (percentEncodePath(key) != strictEncode(key, true)) {
            std::printf("    (as percentEncodePath() writes it: %s -> HTTP %ld)\n", percentEncodePath(key).c_str(), pcharHead.code);
        }
        const HttpResponse get = send(options, "GET", bucketUrl(options) + "/" + strictEncode(key, true));
        std::string detail = describe(get) + ", " + std::to_string(get.body.size()) + " bytes";
        bool same = get.status == Status::Ok && get.code == 200;
        if (same && !options.expect.empty()) {
            same = get.body == readFile(options.expect / fromUtf8(options.bucket) / fromUtf8(key));
            detail += same ? ", the file's bytes" : ", NOT the file's bytes";
        }
        report("GET", same, detail);
        const std::string etag = get.header("etag");
        if (!etag.empty()) {
            HttpRequest request;
            request.url = bucketUrl(options) + "/" + strictEncode(key, true);
            request.explicitCredential = credential();
            request.awsSigV4 = "aws:amz:" + options.region + ":s3";
            request.credentialOverHttp = true;
            request.headers.push_back("If-None-Match: " + etag);
            Progress progress;
            Cancel cancel;
            const HttpResponse again = httpRequest(request, progress, cancel);
            report("GET with If-None-Match: 304", again.code == 304, describe(again));
        }
    }

    std::printf("\nEach folder in the root but the big one: listed by its prefix, its first key read\n");
    for (const std::string& folder : root.folders) {
        if (folder == options.many) continue;
        const Listed inside = list(options, folder);
        report(("ListObjectsV2 of '" + folder + "'").c_str(), inside.ok && !inside.keys.empty(),
               inside.ok ? std::to_string(inside.keys.size()) + " keys" : inside.message);
        if (inside.keys.empty()) continue;
        const HttpResponse get = send(options, "GET", bucketUrl(options) + "/" + strictEncode(inside.keys.front(), true));
        report(("GET " + inside.keys.front()).c_str(), get.code == 200, describe(get));
    }

    std::printf("\nWhat comes back when something is wrong\n");
    const HttpResponse missing = send(options, "HEAD", bucketUrl(options) + "/no-such-key.usda");
    report("a missing key: 404 (403 without ListBucket)", missing.code == 404 || missing.code == 403, describe(missing));
    const HttpResponse missingGet = send(options, "GET", bucketUrl(options) + "/no-such-key.usda");
    report("its GET names the error", !s3Error(missingGet).empty(), describe(missingGet));
    const HttpResponse anonymous = send(options, "GET", bucketUrl(options) + "/?list-type=2", false);
    report("unsigned: refused", anonymous.code >= 400, describe(anonymous));
    const HttpResponse wrongSecret = send(options, "GET", bucketUrl(options) + "/?list-type=2", true, true);
    report("a wrong secret: refused", wrongSecret.code == 403, describe(wrongSecret));
    const HttpResponse wrongRegion = send(options, "HEAD", bucketUrl(options), true, false,
                                          options.region == "us-west-2" ? "eu-west-1" : "us-west-2");
    report("signed for the wrong region: the bucket's region named", !wrongRegion.header("x-amz-bucket-region").empty(),
           describe(wrongRegion) + ", x-amz-bucket-region '" + wrongRegion.header("x-amz-bucket-region") + "'");

    std::printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
