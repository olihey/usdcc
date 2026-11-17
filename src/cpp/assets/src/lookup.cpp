#include "usdcc/assets/lookup.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/disk_cache.h"

#include <pxr/base/tf/diagnostic.h>

#include <algorithm>
#include <atomic>

namespace usdcc::assets {

namespace {

std::atomic<int64_t>& lifetimeMs() {
    static std::atomic<int64_t> value{5000};
    return value;
}

std::string keyOf(const Endpoint& endpoint, const std::string& path) { return endpoint.cacheId() + "|" + path; }

struct DownMarks {
    std::mutex mutex;
    std::map<std::string, std::chrono::steady_clock::time_point> until;
};

DownMarks& downMarks() {
    static DownMarks instance;
    return instance;
}

Lookups::Listing listingOf(Transport& transport, const Endpoint& endpoint, const std::string& folder, Lookups& lookups) {
    const std::string key = keyOf(endpoint, folder);
    Lookups::Listing listing;
    if (lookups.listing(key, &listing)) return listing;
    const Result<std::vector<Entry>> listed = withMirrors(endpoint, [&transport, &folder](const Endpoint& at) {
        Cancel cancel;
        return transport.list(at, folder, cancel);
    });
    listing.status = listed.status;
    listing.message = listed.message;
    for (const Entry& entry : listed.value) listing.value[entry.name] = entry;
    lookups.setListing(key, listing);
    return listing;
}

// What `endpoint` says about the file at `path`: from its folder's listing, or, for a
// protocol that can't list, from a stat of the file itself.
Result<Entry> lookUp(Transport& transport, const Endpoint& endpoint, const Uri& uri, Lookups* scope) {
    Lookups& lookups = scope ? *scope : sharedLookups();

    const Lookups::Listing listing = listingOf(transport, endpoint, uri.folder(), lookups);
    if (listing.ok()) {
        // A listing has real names; the path may spell one with escapes.
        const auto found = listing.value.find(percentDecode(uri.fileName()));
        if (found == listing.value.end()) return Result<Entry>::failure(Status::Missing);
        return Result<Entry>::success(found->second);
    }
    if (listing.status != Status::NotSupported) return Result<Entry>::failure(listing.status, listing.message);

    const std::string entryKey = keyOf(endpoint, uri.path);
    Result<Entry> entry;
    if (!lookups.entry(entryKey, &entry)) {
        entry = withMirrors(endpoint, [&transport, &uri](const Endpoint& at) {
            Cancel cancel;
            return transport.stat(at, uri.path, cancel);
        });
        lookups.setEntry(entryKey, entry);
    }
    return entry;
}

}  // namespace

bool Lookups::fresh(std::chrono::steady_clock::time_point when) const {
    if (!m_expires) return true;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - when);
    return age.count() < lifetimeMs().load();
}

bool Lookups::listing(const std::string& key, Listing* listing) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_listings.find(key);
    if (found == m_listings.end() || !fresh(found->second.when)) return false;
    *listing = found->second.value;
    return true;
}

void Lookups::setListing(const std::string& key, const Listing& listing) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listings[key] = {listing, std::chrono::steady_clock::now()};
}

bool Lookups::entry(const std::string& key, Result<Entry>* entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found == m_entries.end() || !fresh(found->second.when)) return false;
    *entry = found->second.value;
    return true;
}

void Lookups::setEntry(const std::string& key, const Result<Entry>& entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries[key] = {entry, std::chrono::steady_clock::now()};
}

void Lookups::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listings.clear();
    m_entries.clear();
}

Lookups& sharedLookups() {
    static Lookups instance(true);
    return instance;
}

void setLookupLifetime(std::chrono::milliseconds lifetime) { lifetimeMs().store(lifetime.count()); }

void forgetLookups() { sharedLookups().clear(); }

bool isMarkedDown(const std::string& url) {
    DownMarks& marks = downMarks();
    std::lock_guard<std::mutex> lock(marks.mutex);
    const auto found = marks.until.find(url);
    return found != marks.until.end() && std::chrono::steady_clock::now() < found->second;
}

void markDown(const std::string& url) {
    DownMarks& marks = downMarks();
    std::lock_guard<std::mutex> lock(marks.mutex);
    marks.until[url] = std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

void forgetDownMarks() {
    DownMarks& marks = downMarks();
    std::lock_guard<std::mutex> lock(marks.mutex);
    marks.until.clear();
}

Found findInStack(const Stack& stack, const std::string& path, Lookups* scope) {
    Found found;
    if (isOffline()) {
        // No server is asked: whichever endpoint's version is in the cache, top first.
        for (const Endpoint& endpoint : stack.endpoints) {
            if (std::optional<std::pair<std::filesystem::path, Entry>> cached = DiskCache::shared().findAny(endpoint, path)) {
                found.status = Status::Ok;
                found.endpoint = endpoint;
                found.entry = cached->second;
                found.cachedFile = cached->first;
                return found;
            }
        }
        found.status = Status::Missing;
        found.message = "offline, and not in the cache";
        return found;
    }
    const std::shared_ptr<Transport> transport = transportFor(stack.scheme);
    if (!transport) {
        found.status = Status::Error;
        found.message = "no protocol plugin is loaded for " + stack.scheme + "://";
        return found;
    }
    Uri uri;
    uri.scheme = stack.scheme;
    uri.name = stack.name;
    uri.path = path;
    for (const Endpoint& endpoint : stack.endpoints) {
        const Result<Entry> entry = lookUp(*transport, endpoint, uri, scope);
        if (entry.ok()) {
            // A folder of that name doesn't hide a file further down.
            if (entry.value.isFolder) continue;
            found.status = Status::Ok;
            found.endpoint = endpoint;
            found.entry = entry.value;
            return found;
        }
        if (entry.status == Status::Missing) continue;
        // Not the next endpoint's file instead: that would hand out PROD's assets while TEST is down.
        if (endpoint.optional) continue;
        found.status = entry.status;
        found.message = entry.message;
        found.endpoint = endpoint;
        return found;
    }
    found.status = Status::Missing;
    return found;
}

Result<std::vector<ListedEntry>> listUri(const std::string& uri, const Context& context) {
    using Listed = Result<std::vector<ListedEntry>>;
    Uri named;
    if (parseUri(uri, &named) && named.stackId.empty()) {
        if (const std::shared_ptr<const Stack> stack = context.stack(named.scheme, named.name)) {
            return listFolder(*stack, named.path);
        }
    }
    const size_t colon = uri.find("://");
    const AddressParser parser = colon == std::string::npos ? AddressParser() : addressParserFor(uri.substr(0, colon));
    Endpoint endpoint;
    std::string path;
    std::string normalized;
    if (!parser || !parser(uri, &endpoint, &path, &normalized)) {
        return Listed::failure(Status::Missing, named.name.empty() ? "it isn't a server's path"
                                                                   : "the environment '" + context.environment() +
                                                                         "' has no server named '" + named.name + "'");
    }
    const size_t start = normalized.find("://") + 3;
    std::string host = normalized.substr(start, normalized.find('/', start) - start);
    if (host.rfind(':') != std::string::npos && host.find(']', host.rfind(':')) == std::string::npos) host.erase(host.rfind(':'));
    if (!hostAllowed(host)) return Listed::failure(Status::NotAllowed, "the host '" + host + "' isn't allowed");
    Stack stack;
    stack.scheme = endpoint.scheme;
    stack.name = endpoint.id;
    stack.endpoints.push_back(endpoint);
    return listFolder(stack, path);
}

Result<std::vector<ListedEntry>> listFolder(const Stack& stack, const std::string& folder, Lookups* scope) {
    using Listed = Result<std::vector<ListedEntry>>;
    const std::shared_ptr<Transport> transport = transportFor(stack.scheme);
    if (!transport) return Listed::failure(Status::Error, "no protocol plugin is loaded for " + stack.scheme + "://");
    if (isOffline()) return Listed::failure(Status::Unreachable, "offline");
    Lookups& lookups = scope ? *scope : sharedLookups();

    std::map<std::string, ListedEntry> merged;
    bool anywhere = false;
    for (const Endpoint& endpoint : stack.endpoints) {
        const Lookups::Listing listing = listingOf(*transport, endpoint, folder, lookups);
        if (listing.status == Status::Missing) continue;
        if (!listing.ok()) {
            if (endpoint.optional) continue;
            return Listed::failure(listing.status, listing.message.empty() ? endpoint.id : endpoint.id + ": " + listing.message);
        }
        anywhere = true;
        // emplace keeps what an upper endpoint already put there.
        for (const auto& [name, entry] : listing.value) merged.emplace(name, ListedEntry{entry, endpoint.id});
    }
    if (!anywhere) return Listed::failure(Status::Missing);
    std::vector<ListedEntry> entries;
    for (auto& [name, entry] : merged) entries.push_back(std::move(entry));
    return Listed::success(std::move(entries));
}

}  // namespace usdcc::assets
