// The resolver and transport for webdav:// (docs/WEBDAV_RESOLVER.md). A named
// protocol: webdav://<name>/<path>, with the name's stack of servers coming from the
// context, so the resolver adds nothing to CachingResolver but its scheme. The
// transport is PROPFIND for listings and GET for files.
#include "usdcc/assets/activity.h"
#include "usdcc/assets/caching_resolver.h"
#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/credentials.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include "xml_names.h"

#include <pxr/pxr.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/resolvedPath.h>

#include <pugixml.hpp>

#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace usdcc::assets;

// Only what resolve needs: a listing of a big folder is big enough without the rest.
const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
    "<D:resourcetype/><D:getetag/><D:getlastmodified/><D:getcontentlength/>"
    "</D:prop></D:propfind>";

using xml::childNamed;
using xml::localName;

// The path of an href, decoded and without a trailing slash: servers send hrefs
// absolute or relative, encoded their own way, and with a slash on folders.
std::string pathOfHref(std::string href) {
    const size_t scheme = href.find("://");
    if (scheme != std::string::npos) {
        const size_t slash = href.find('/', scheme + 3);
        href = slash == std::string::npos ? std::string("/") : href.substr(slash);
    }
    href = percentDecode(href);
    while (href.size() > 1 && href.back() == '/') href.pop_back();
    return href;
}

struct Response {
    std::string path;  // as pathOfHref() gives it
    Entry entry;
};

// The <response> elements of a 207 reply. False when it isn't one.
bool parseMultistatus(const std::string& xml, std::vector<Response>* responses) {
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) return false;
    const pugi::xml_node multistatus = childNamed(document, "multistatus");
    if (!multistatus) return false;
    for (const pugi::xml_node& node : multistatus.children()) {
        if (std::strcmp(localName(node), "response") != 0) continue;
        Response response;
        response.path = pathOfHref(childNamed(node, "href").text().get());
        response.entry.name = response.path.substr(response.path.rfind('/') + 1);
        for (const pugi::xml_node& propstat : node.children()) {
            if (std::strcmp(localName(propstat), "propstat") != 0) continue;
            // "HTTP/1.1 200 OK": the properties the server has. Others come under a 404.
            const std::string status = childNamed(propstat, "status").text().get();
            if (status.find(" 200") == std::string::npos) continue;
            const pugi::xml_node prop = childNamed(propstat, "prop");
            if (childNamed(childNamed(prop, "resourcetype"), "collection")) response.entry.isFolder = true;
            response.entry.etag = childNamed(prop, "getetag").text().get();
            response.entry.modified = parseHttpDate(childNamed(prop, "getlastmodified").text().get());
            const std::string length = childNamed(prop, "getcontentlength").text().get();
            if (!length.empty() && std::isdigit(static_cast<unsigned char>(length[0]))) response.entry.size = std::stoull(length);
        }
        responses->push_back(std::move(response));
    }
    return true;
}

class WebDavTransport final : public Transport {
public:
    Result<Entry> stat(const Endpoint& endpoint, const std::string& path, Cancel& cancel) override {
        std::vector<Response> responses;
        std::string message;
        const Status status = propfind(endpoint, path, "0", cancel, &responses, &message);
        if (status != Status::Ok) return Result<Entry>::failure(status, message);
        if (responses.empty()) return Result<Entry>::failure(Status::Missing);
        return Result<Entry>::success(responses.front().entry);
    }

    bool canList() const override { return true; }

    Result<std::vector<Entry>> list(const Endpoint& endpoint, const std::string& folder, Cancel& cancel) override {
        using Listed = Result<std::vector<Entry>>;
        std::vector<Response> responses;
        std::string message;
        const Status status = propfind(endpoint, folder, "1", cancel, &responses, &message);
        if (status != Status::Ok) return Listed::failure(status, message);
        // The folder itself is among them: the one whose path is the request's.
        const std::string self = pathOfHref(endpoint.url + folder);
        std::vector<Entry> entries;
        bool isFolder = false;
        for (Response& response : responses) {
            if (response.path == self) {
                isFolder = response.entry.isFolder;
                continue;
            }
            entries.push_back(std::move(response.entry));
        }
        // A file where a folder was asked for has no children, and isn't one.
        if (entries.empty() && !isFolder && !responses.empty()) return Listed::failure(Status::Missing);
        return Listed::success(std::move(entries));
    }

    Result<Entry> fetch(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& into,
                        const std::string& knownEtag, Progress& progress, Cancel& cancel) override {
        HttpRequest request = requestFor(endpoint, path);
        request.into = into;
        if (!knownEtag.empty()) request.headers.push_back("If-None-Match: " + knownEtag);
        const HttpResponse response = httpRequest(request, progress, cancel);
        if (response.status != Status::Ok || statusOfHttpCode(response.code) != Status::Ok) return httpFailure<Entry>(response);
        Entry entry;
        entry.name = percentDecode(path.substr(path.rfind('/') + 1));
        entry.etag = response.header("etag");
        entry.modified = parseHttpDate(response.header("last-modified"));
        const std::string length = response.header("content-length");
        if (!length.empty() && std::isdigit(static_cast<unsigned char>(length[0]))) entry.size = std::stoull(length);
        return Result<Entry>::success(std::move(entry));
    }

    bool canPut() const override { return true; }

    // MKCOL for each folder it's in that isn't there, then a PUT only if the file is the
    // version `knownEtag` names, or isn't there at all (docs/WEBDAV_RESOLVER.md §5).
    Result<Entry> put(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& from,
                      const std::string& knownEtag, Progress& progress, Cancel& cancel) override {
        std::string condition;
        if (knownEtag.empty()) {
            condition = "If-None-Match: *";
        } else if (knownEtag == "*" || knownEtag.rfind("W/", 0) == 0) {
            // A weak ETag never matches in If-Match (RFC 9110 §13.1.1): it's compared here,
            // weakly, then the file is replaced whichever version is there by then.
            if (knownEtag != "*") {
                const Result<Entry> now = stat(endpoint, path, cancel);
                if (now.status == Status::Missing) return Result<Entry>::failure(Status::Conflict, "the file is gone");
                if (!now.ok()) return now;
                if (weakly(now.value.etag) != weakly(knownEtag)) {
                    return Result<Entry>::failure(Status::Conflict, "its ETag is " + now.value.etag + ", not " + knownEtag);
                }
            }
            condition = "If-Match: *";
        } else {
            condition = "If-Match: " + knownEtag;
        }

        const Result<bool> folders = makeFolders(endpoint, path, cancel);
        if (!folders.ok()) return Result<Entry>::failure(folders.status, folders.message);
        HttpRequest request = requestFor(endpoint, path);
        request.method = "PUT";
        request.uploadFrom = from;
        request.headers.push_back(condition);
        const HttpResponse response = httpRequest(request, progress, cancel);
        if (response.status != Status::Ok) return httpFailure<Entry>(response);
        const Status status = statusOfHttpCode(response.code);
        if (status == Status::Conflict) {
            return Result<Entry>::failure(Status::Conflict, "HTTP " + std::to_string(response.code) + ", for " + condition);
        }
        if (status != Status::Ok) return httpFailure<Entry>(response);
        Entry entry;
        entry.name = percentDecode(path.substr(path.rfind('/') + 1));
        entry.etag = response.header("etag");
        entry.modified = parseHttpDate(response.header("last-modified"));
        std::error_code error;
        entry.size = std::filesystem::file_size(from, error);
        // Not every server says what the new version is: it's asked.
        if (entry.etag.empty() || entry.modified == 0) {
            const Result<Entry> now = stat(endpoint, path, cancel);
            if (now.ok()) entry = now.value;
        }
        return Result<Entry>::success(std::move(entry));
    }

private:
    static std::string weakly(const std::string& etag) { return etag.rfind("W/", 0) == 0 ? etag.substr(2) : etag; }

    // Each folder above `path` that isn't there, top first. A 405 is a folder that is.
    static Result<bool> makeFolders(const Endpoint& endpoint, const std::string& path, Cancel& cancel) {
        std::vector<std::string> missing;
        for (std::string folder = path.substr(0, path.rfind('/')); !folder.empty(); folder = folder.substr(0, folder.rfind('/'))) {
            std::vector<Response> responses;
            std::string message;
            const Status status = propfind(endpoint, folder, "0", cancel, &responses, &message);
            if (status == Status::Ok) break;
            if (status != Status::Missing) return Result<bool>::failure(status, "PROPFIND " + folder + ": " + message);
            missing.push_back(folder);
        }
        for (auto folder = missing.rbegin(); folder != missing.rend(); ++folder) {
            Progress quiet;
            HttpRequest request = requestFor(endpoint, *folder + "/");
            request.method = "MKCOL";
            const HttpResponse response = httpRequest(request, quiet, cancel);
            if (response.status != Status::Ok) return Result<bool>::failure(response.status, response.message);
            if (response.code == 405) continue;
            const Status status = statusOfHttpCode(response.code);
            if (status != Status::Ok) {
                // A 409 here is a folder above that isn't there, not a version that changed.
                return Result<bool>::failure(status == Status::Conflict ? Status::Error : status,
                                             "MKCOL " + *folder + ": HTTP " + std::to_string(response.code));
            }
        }
        return Result<bool>::success(true);
    }
    static HttpRequest requestFor(const Endpoint& endpoint, const std::string& path) {
        HttpRequest request;
        request.url = endpoint.url + percentEncodePath(path);
        request.credential = endpoint.credential;
        // Allowed, with a warning in the log (docs/WEBDAV_RESOLVER.md §7).
        request.credentialOverHttp = true;
        return request;
    }

    static Status propfind(const Endpoint& endpoint, const std::string& path, const char* depth, Cancel& cancel,
                           std::vector<Response>* responses, std::string* message) {
        Progress progress;
        HttpRequest request = requestFor(endpoint, path);
        request.method = "PROPFIND";
        request.headers = {std::string("Depth: ") + depth, "Content-Type: application/xml; charset=utf-8"};
        request.body = kPropfindBody;
        const HttpResponse response = httpRequest(request, progress, cancel);
        if (response.status != Status::Ok) {
            *message = response.message;
            return response.status;
        }
        if (response.code != 207) {
            const Status status = statusOfHttpCode(response.code);
            *message = "HTTP " + std::to_string(response.code);
            // A 2xx that isn't a multistatus isn't a WebDAV server answering.
            return status == Status::Ok ? Status::Error : status;
        }
        if (!parseMultistatus(response.body, responses)) {
            *message = "its reply to PROPFIND isn't a WebDAV multistatus";
            return Status::Error;
        }
        return Status::Ok;
    }
};


// A server by its address (docs/WEBDAV_RESOLVER.md §2.1): webdav://192.168.1.5:5000/a.usd is
// http://192.168.1.5:5000/a.usd, webdavs:// the same over https.
struct DirectUrl {
    std::string scheme;  // "webdav" or "webdavs"
    std::string host;    // lowercase, without the port
    std::string port;    // empty for the scheme's own
    std::string path;    // normalized

    std::string authority() const { return port.empty() ? host : host + ":" + port; }
    std::string text() const { return scheme + "://" + authority() + path; }
    std::string httpUrl() const { return (scheme == "webdavs" ? "https://" : "http://") + authority(); }
};

bool parseDirect(const std::string& text, DirectUrl* url) {
    const size_t colon = text.find("://");
    if (colon == std::string::npos) return false;
    DirectUrl parsed;
    for (const char c : text.substr(0, colon)) parsed.scheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (parsed.scheme != "webdav" && parsed.scheme != "webdavs") return false;
    if (text.find_first_of("?#") != std::string::npos) return false;
    const size_t start = colon + 3;
    const size_t slash = std::min(text.find('/', start), text.size());
    std::string authority = text.substr(start, slash - start);
    if (authority.empty() || authority.find_first_of("@~") != std::string::npos) return false;
    const size_t portColon = authority.rfind(':');
    if (portColon != std::string::npos && authority.find(']', portColon) == std::string::npos) {
        parsed.port = authority.substr(portColon + 1);
        authority.erase(portColon);
        if (parsed.port.empty() || parsed.port.find_first_not_of("0123456789") != std::string::npos) return false;
    }
    for (const char c : authority) parsed.host += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (parsed.host.empty()) return false;
    if ((parsed.scheme == "webdav" && parsed.port == "80") || (parsed.scheme == "webdavs" && parsed.port == "443")) {
        parsed.port.clear();
    }
    if (!normalizePath(text.substr(slash), &parsed.path)) return false;
    *url = std::move(parsed);
    return true;
}

// A name in the configuration is a word ("studio"); an address has a port, a dot, is
// localhost, or is an IPv6 address in brackets.
bool looksLikeAddress(const DirectUrl& url) {
    return !url.port.empty() || url.host.find('.') != std::string::npos || url.host == "localhost" ||
           (!url.host.empty() && url.host.front() == '[');
}

// What an address means (docs/WEBDAV_RESOLVER.md §2.1): one endpoint, its credential
// named after the address unless the configuration names one. False for anything else.
bool addressEndpoint(const std::string& uri, Endpoint* endpoint, std::string* path, std::string* normalized) {
    DirectUrl url;
    if (!parseDirect(uri, &url) || !looksLikeAddress(url)) return false;
    Endpoint made;
    made.id = url.authority();
    made.scheme = "webdav";
    made.url = url.httpUrl();
    made.readOnly = true;
    const Config config = defaultConfig();
    const auto host = config.httpHosts.find(url.host);
    if (host != config.httpHosts.end()) made.credential = host->second.credential;
    // Else the address is the credential's name: what Edit > Asset Servers > Logins is given.
    if (made.credential.empty()) made.credential = credentialIdForAddress(url.host, url.port);
    *endpoint = std::move(made);
    *path = url.path;
    *normalized = url.text();
    return true;
}

// When the library loads: the transport of both schemes, and what an address means in them.
const struct RegisterTransport {
    RegisterTransport() {
        const std::shared_ptr<WebDavTransport> transport = std::make_shared<WebDavTransport>();
        registerTransport("webdav", transport);
        registerTransport("webdavs", transport);
        registerAddressParser("webdav", addressEndpoint);
        registerAddressParser("webdavs", addressEndpoint);
    }
} kRegisterTransport;

}  // namespace

PXR_NAMESPACE_OPEN_SCOPE

class UsdccWebDavResolver final : public usdcc::assets::CachingResolver {
public:
    UsdccWebDavResolver() : CachingResolver("webdav") {}

protected:
    // A name the environment knows, as every named protocol; else, when it's an address,
    // that server directly, with no stack and no context.
    bool _Locate(const std::string& assetPath, Location* location, std::string* why) const override {
        if (CachingResolver::_Locate(assetPath, location, why)) return true;
        usdcc::assets::Endpoint endpoint;
        std::string path;
        std::string normalized;
        if (!addressEndpoint(assetPath, &endpoint, &path, &normalized)) return false;
        DirectUrl url;
        parseDirect(normalized, &url);
        const std::string& hostOnly = url.host;
        // A stage can name any address, as with plain HTTP (docs/HTTP_RESOLVER.md §4).
        if (!usdcc::assets::hostAllowed(hostOnly)) {
            *why = "the host '" + hostOnly +
                   "' isn't one this stage may contact (Settings > Assets > Allowed hosts, the configuration's \"http\" "
                   "\"allow\" list, or USDCC_HTTP_ALLOW)";
            return false;
        }
        usdcc::assets::Stack stack;
        stack.scheme = "webdav";
        stack.name = endpoint.id;
        stack.endpoints.push_back(std::move(endpoint));
        location->path = path;
        location->resolved = normalized;
        location->display = normalized;
        location->stack = std::make_shared<const usdcc::assets::Stack>(std::move(stack));
        return true;
    }

    std::string _CreateIdentifier(const std::string& assetPath, const ArResolvedPath& anchorAssetPath) const override {
        usdcc::assets::Uri named;
        DirectUrl url;
        if (assetPath.find("://") != std::string::npos) {
            if (usdcc::assets::parseUri(assetPath, &named)) return CachingResolver::_CreateIdentifier(assetPath, anchorAssetPath);
            return parseDirect(assetPath, &url) ? url.text() : assetPath;
        }
        if (assetPath.empty() || !TfIsRelativePath(assetPath)) return assetPath;
        const std::string anchor = anchorAssetPath.GetPathString();
        if (usdcc::assets::parseUri(anchor, &named)) return CachingResolver::_CreateIdentifier(assetPath, anchorAssetPath);
        // Against an address: its folder, on the same server.
        if (!parseDirect(anchor, &url)) return assetPath;
        const std::string folder = url.path.substr(0, url.path.rfind('/') + 1);
        if (!normalizePath(folder + assetPath, &url.path)) return assetPath;
        return url.text();
    }
};

AR_DEFINE_RESOLVER(UsdccWebDavResolver, ArResolver);

PXR_NAMESPACE_CLOSE_SCOPE
