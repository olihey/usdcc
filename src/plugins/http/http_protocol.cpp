// The resolver and transport for plain http:// and https:// (docs/HTTP_RESOLVER.md).
// A direct protocol: the URL is the real address, so there are no names, environments
// or stacks, and the resolved path is the identifier. Read-only, and it can't list.
#include "usdcc/assets/activity.h"
#include "usdcc/assets/caching_resolver.h"
#include "usdcc/assets/credentials.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include <pxr/pxr.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/resolvedPath.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace {

using namespace usdcc::assets;

struct HttpUrl {
    std::string scheme;  // "http" or "https"
    std::string host;    // lowercase, without the port
    std::string port;    // empty for the scheme's own
    std::string path;    // "/chars/hero.usd", normalized
    std::string query;   // "?v=12", or empty

    std::string origin() const { return scheme + "://" + host + (port.empty() ? "" : ":" + port); }
    std::string text() const { return origin() + path + query; }
};

bool parseHttpUrl(const std::string& text, HttpUrl* url, std::string* why) {
    const auto fail = [why](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    const size_t colon = text.find("://");
    if (colon == std::string::npos) return fail("it has no scheme");
    HttpUrl parsed;
    for (const char c : text.substr(0, colon)) parsed.scheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (parsed.scheme != "http" && parsed.scheme != "https") return fail("it isn't http or https");
    if (text.find('#') != std::string::npos) return fail("it has a fragment");

    const size_t authorityStart = colon + 3;
    const size_t authorityEnd = std::min(text.find_first_of("/?", authorityStart), text.size());
    std::string authority = text.substr(authorityStart, authorityEnd - authorityStart);
    if (authority.find('@') != std::string::npos) return fail("it has a user name in it; give the host a credential instead");
    const size_t portColon = authority.rfind(':');
    // Not the colons of an IPv6 address in brackets.
    if (portColon != std::string::npos && authority.find(']', portColon) == std::string::npos) {
        parsed.port = authority.substr(portColon + 1);
        authority.erase(portColon);
    }
    if ((parsed.scheme == "http" && parsed.port == "80") || (parsed.scheme == "https" && parsed.port == "443")) parsed.port.clear();
    for (const char c : authority) parsed.host += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (parsed.host.empty()) return fail("it names no host");

    const size_t queryStart = text.find('?', authorityEnd);
    const std::string path = text.substr(authorityEnd, queryStart == std::string::npos ? std::string::npos : queryStart - authorityEnd);
    if (queryStart != std::string::npos) parsed.query = text.substr(queryStart);
    if (!normalizePath(path, &parsed.path)) return fail("it goes above the server's root");
    *url = std::move(parsed);
    return true;
}

std::string lastSegment(const std::string& path) {
    const std::string plain = path.substr(0, path.find('?'));
    return percentDecode(plain.substr(plain.rfind('/') + 1));
}

Entry entryOf(const HttpResponse& response, const std::string& path) {
    Entry entry;
    entry.name = lastSegment(path);
    entry.etag = response.header("etag");
    entry.modified = parseHttpDate(response.header("last-modified"));
    // "bytes 0-0/1234" from a ranged GET; otherwise the body's own length.
    const std::string range = response.header("content-range");
    const size_t slash = range.rfind('/');
    const std::string length = slash != std::string::npos ? range.substr(slash + 1) : response.header("content-length");
    if (!length.empty() && std::isdigit(static_cast<unsigned char>(length[0]))) entry.size = std::stoull(length);
    return entry;
}

class HttpTransport final : public Transport {
public:
    Result<Entry> stat(const Endpoint& endpoint, const std::string& path, Cancel& cancel) override {
        Progress progress;
        HttpRequest request = requestFor(endpoint, path);
        request.method = "HEAD";
        HttpResponse response = httpRequest(request, progress, cancel);
        if (response.status == Status::Ok && (response.code == 405 || response.code == 501)) {
            // A server that refuses HEAD: ask for the first byte instead.
            request.method = "GET";
            request.headers.push_back("Range: bytes=0-0");
            response = httpRequest(request, progress, cancel);
        }
        if (response.status != Status::Ok || statusOfHttpCode(response.code) != Status::Ok) return httpFailure<Entry>(response);
        return Result<Entry>::success(entryOf(response, path));
    }

    Result<Entry> fetch(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& into,
                        const std::string& knownEtag, Progress& progress, Cancel& cancel) override {
        HttpRequest request = requestFor(endpoint, path);
        request.into = into;
        if (!knownEtag.empty()) request.headers.push_back("If-None-Match: " + knownEtag);
        const HttpResponse response = httpRequest(request, progress, cancel);
        if (response.status != Status::Ok || statusOfHttpCode(response.code) != Status::Ok) return httpFailure<Entry>(response);
        return Result<Entry>::success(entryOf(response, path));
    }

private:
    static HttpRequest requestFor(const Endpoint& endpoint, const std::string& path) {
        HttpRequest request;
        // The query string goes as it was written; the path gets its escapes.
        const size_t query = path.find('?');
        request.url = endpoint.url + percentEncodePath(path.substr(0, query)) + (query == std::string::npos ? "" : path.substr(query));
        request.credential = endpoint.credential;
        // CDNs and download links redirect, to other hosts too; the credential stays behind.
        request.followRedirects = true;
        HttpUrl url;
        if (!endpoint.credential.empty() && parseHttpUrl(endpoint.url + "/", &url, nullptr)) {
            const Config config = defaultConfig();
            const auto host = config.httpHosts.find(url.host);
            request.credentialOverHttp = host != config.httpHosts.end() && host->second.insecure;
        }
        return request;
    }
};

// When the library loads: the transport of both schemes.
const struct RegisterTransport {
    RegisterTransport() {
        const std::shared_ptr<HttpTransport> transport = std::make_shared<HttpTransport>();
        registerTransport("http", transport);
        registerTransport("https", transport);
    }
} kRegisterTransport;

}  // namespace

PXR_NAMESPACE_OPEN_SCOPE

class UsdccHttpResolver final : public usdcc::assets::CachingResolver {
public:
    UsdccHttpResolver() : CachingResolver("http") {}

protected:
    bool _Locate(const std::string& assetPath, Location* location, std::string* why) const override {
        HttpUrl url;
        if (!parseHttpUrl(assetPath, &url, why)) return false;
        // A stage from elsewhere can name any address (docs/HTTP_RESOLVER.md §4).
        if (!hostAllowed(url.host)) {
            *why = "the host '" + url.host + "' isn't one this stage may contact (the \"http\" \"allow\" list, or USDCC_HTTP_ALLOW)";
            return false;
        }
        const Config config = defaultConfig();
        const auto host = config.httpHosts.find(url.host);
        Endpoint endpoint;
        endpoint.id = url.host;
        endpoint.scheme = url.scheme;
        endpoint.url = url.origin();
        endpoint.readOnly = true;
        if (host != config.httpHosts.end()) endpoint.credential = host->second.credential;
        // Else the address is the credential's name, as for WebDAV by address.
        if (endpoint.credential.empty()) endpoint.credential = credentialIdForAddress(url.host, url.port);
        Stack stack;
        stack.scheme = url.scheme;
        stack.name = url.host;
        stack.endpoints.push_back(std::move(endpoint));
        location->stack = std::make_shared<const Stack>(std::move(stack));
        location->path = url.path + url.query;
        location->resolved = url.text();
        location->display = location->resolved;
        return true;
    }

    std::string _CreateIdentifier(const std::string& assetPath, const ArResolvedPath& anchorAssetPath) const override {
        HttpUrl url;
        if (assetPath.find("://") != std::string::npos) return parseHttpUrl(assetPath, &url, nullptr) ? url.text() : assetPath;
        // A path on disk referenced from a layer on a server stays what it is.
        if (assetPath.empty() || !TfIsRelativePath(assetPath)) return assetPath;
        HttpUrl anchor;
        if (!parseHttpUrl(anchorAssetPath.GetPathString(), &anchor, nullptr)) return assetPath;
        // As a browser resolves it: against the layer's folder, without the layer's query string.
        const size_t query = assetPath.find('?');
        const std::string folder = anchor.path.substr(0, anchor.path.rfind('/') + 1);
        url = anchor;
        url.query = query == std::string::npos ? std::string() : assetPath.substr(query);
        if (!normalizePath(folder + assetPath.substr(0, query), &url.path)) return assetPath;
        return url.text();
    }

    // The same server whatever the context: the URL is the address.
    bool _IsContextDependentPath(const std::string&) const override { return false; }
};

AR_DEFINE_RESOLVER(UsdccHttpResolver, ArResolver);

PXR_NAMESPACE_CLOSE_SCOPE
