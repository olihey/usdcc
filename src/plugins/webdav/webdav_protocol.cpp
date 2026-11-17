// The resolver and transport for webdav:// (docs/WEBDAV_RESOLVER.md). A named
// protocol: webdav://<name>/<path>, with the name's stack of servers coming from the
// context, so the resolver adds nothing to CachingResolver but its scheme. The
// transport is PROPFIND for listings and GET for files.
#include "usdcc/assets/caching_resolver.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/defineResolver.h>

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

// Servers pick their own namespace prefix (D:, d:, lp1:, none), so elements are found
// by the name after it.
const char* localName(const pugi::xml_node& node) {
    const char* name = node.name();
    const char* colon = std::strchr(name, ':');
    return colon ? colon + 1 : name;
}

pugi::xml_node childNamed(const pugi::xml_node& parent, const char* name) {
    for (const pugi::xml_node& child : parent.children()) {
        if (std::strcmp(localName(child), name) == 0) return child;
    }
    return pugi::xml_node();
}

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

private:
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

// When the library loads.
const struct RegisterTransport {
    RegisterTransport() { registerTransport("webdav", std::make_shared<WebDavTransport>()); }
} kRegisterTransport;

}  // namespace

PXR_NAMESPACE_OPEN_SCOPE

class UsdccWebDavResolver final : public usdcc::assets::CachingResolver {
public:
    UsdccWebDavResolver() : CachingResolver("webdav") {}
};

AR_DEFINE_RESOLVER(UsdccWebDavResolver, ArResolver);

PXR_NAMESPACE_CLOSE_SCOPE
