#pragma once

// What a protocol implements (docs/REMOTE_ASSETS.md §6): four calls, from which
// CachingResolver makes a whole ArResolver. Only stat() and fetch() are required.
// Every call may come from any thread.

#include "usdcc/assets/result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace usdcc::assets {

// One real server location (§4).
struct Endpoint {
    std::string id;                    // its key in the configuration: "prod"
    std::string scheme;                // "webdav"
    std::string url;                   // https://dav.studio.local/assets
    std::vector<std::string> mirrors;  // other URLs with the same content
    std::string credential;            // a credential id, never a secret; empty for anonymous
    bool readOnly = false;
    bool optional = false;             // a stack carries on when this one is unreachable

    // Names its folder in the disk cache. From the scheme, URL and credential id,
    // so it's the same in every process.
    std::string cacheId() const;
};

struct Entry {
    std::string name;
    bool isFolder = false;
    uint64_t size = 0;
    double modified = 0;  // seconds since the epoch; 0 when the server doesn't say
    std::string etag;     // or the nearest thing the server has; may be empty

    // Tells two versions of one file apart: from the ETag, or else the time and size.
    std::string versionKey() const;
};

class Cancel {
public:
    void cancel() { m_cancelled.store(true); }
    bool cancelled() const { return m_cancelled.load(); }

private:
    std::atomic<bool> m_cancelled{false};
};

class Progress {
public:
    virtual ~Progress() = default;
    // `total` is 0 when the server didn't say.
    virtual void advanced(uint64_t /*done*/, uint64_t /*total*/) {}
};

class Transport {
public:
    virtual ~Transport();

    // `path` starts with '/' and is relative to the endpoint's URL.
    virtual Result<Entry> stat(const Endpoint& endpoint, const std::string& path, Cancel& cancel) = 0;

    // The files and folders directly in `folder`. "Not supported" unless overridden.
    virtual Result<std::vector<Entry>> list(const Endpoint& endpoint, const std::string& folder, Cancel& cancel);
    // Whether list() does: what makes a protocol's folders browsable in the asset picker.
    virtual bool canList() const { return false; }

    // Writes the file to `into`. With `knownEtag`, NotModified is a result and nothing is written.
    virtual Result<Entry> fetch(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& into,
                                const std::string& knownEtag, Progress& progress, Cancel& cancel) = 0;

    // Phase 6. An empty `knownEtag` means "only if it doesn't exist yet". "Not supported" unless overridden.
    virtual Result<Entry> put(const Endpoint& endpoint, const std::string& path, const std::filesystem::path& from,
                              const std::string& knownEtag, Progress& progress, Cancel& cancel);
};

// One transport per scheme, for the whole process. A protocol's library registers its
// own when it loads; registering a scheme again replaces the transport.
void registerTransport(const std::string& scheme, std::shared_ptr<Transport> transport);
std::shared_ptr<Transport> transportFor(const std::string& scheme);
// The schemes a transport is registered for, sorted.
std::vector<std::string> transportSchemes();

// For a protocol that takes a server's address in place of a name
// (webdav://192.168.1.5:5000/a.usd, docs/WEBDAV_RESOLVER.md §2.1): the single
// endpoint an address means, the path on it, and the URI normalized. False when
// `uri` isn't an address of this protocol. Registered with the transport.
using AddressParser =
    std::function<bool(const std::string& uri, Endpoint* endpoint, std::string* path, std::string* normalized)>;
void registerAddressParser(const std::string& scheme, AddressParser parser);
AddressParser addressParserFor(const std::string& scheme);

// Loads the plugin of every resolver whose plugInfo.json entry says
// "usdccAssetsProtocol": true, so that its transport is registered before anything
// was resolved (USD loads a resolver's library only when it first needs it).
void loadProtocolPlugins();

}  // namespace usdcc::assets
