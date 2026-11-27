#include "usdcc/assets/lookup.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/no_wait.h"

#include <pxr/base/tf/diagnostic.h>

#include <algorithm>
#include <sstream>
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
    // Never destroyed, as sharedLookups(): a worker may be asking as the process ends (no_wait.h).
    static DownMarks& instance = *new DownMarks;
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

void Lookups::forget(const Stack& stack, const std::string& path) {
    // Its folder, and the folders above: a write may have made them.
    std::vector<std::string> folders;
    Uri uri;
    uri.path = path;
    while (true) {
        const std::string folder = uri.folder();
        folders.push_back(folder);
        if (folder == "/" || folder.empty()) break;
        uri.path = folder;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Endpoint& endpoint : stack.endpoints) {
        for (const std::string& folder : folders) m_listings.erase(keyOf(endpoint, folder));
        m_entries.erase(keyOf(endpoint, path));
    }
}

Lookups& sharedLookups() {
    static Lookups& instance = *new Lookups(true);
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

namespace {

Found search(const Stack& stack, const std::string& path, Lookups* scope);

}  // namespace

Found findInStack(const Stack& stack, const std::string& path, Lookups* scope) {
    // Offline asks no server either.
    if (isOffline()) return search(stack, path, scope);
    if (NoWait::active()) return answerWithoutWaiting(stack, path);
    Found found = search(stack, path, scope);
    rememberAnswer(stack, path, found);
    return found;
}

namespace {

Found search(const Stack& stack, const std::string& path, Lookups* scope) {
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

// The stack and folder `uri` names for browsing: a name of the context's environment, or
// a server by its address whose host (or bucket) is allowed.
Result<bool> browsedStack(const std::string& uri, const Context& context, Stack* found, std::string* folder) {
    using Located = Result<bool>;
    Uri named;
    if (parseUri(uri, &named) && named.stackId.empty()) {
        if (const std::shared_ptr<const Stack> stack = context.stack(named.scheme, named.name)) {
            *found = *stack;
            *folder = named.path;
            return Located::success(true);
        }
    }
    const size_t colon = uri.find("://");
    const AddressParser parser = colon == std::string::npos ? AddressParser() : addressParserFor(uri.substr(0, colon));
    Endpoint endpoint;
    std::string path;
    std::string normalized;
    if (!parser || !parser(uri, &endpoint, &path, &normalized)) {
        return Located::failure(Status::Missing,
                                named.name.empty() ? "it isn't a server's path" : context.whyNoName(named.name));
    }
    const size_t start = normalized.find("://") + 3;
    std::string host = normalized.substr(start, normalized.find('/', start) - start);
    if (host.rfind(':') != std::string::npos && host.find(']', host.rfind(':')) == std::string::npos) host.erase(host.rfind(':'));
    // An S3 bucket named by itself has an allow list of its own (docs/S3_RESOLVER.md §3.2);
    // an S3 server by its address is a host as any other.
    const bool bucket = endpoint.scheme == "s3" && endpoint.url.rfind("s3://", 0) == 0;
    if (bucket ? !bucketAllowed(host) : !hostAllowed(host)) {
        return Located::failure(Status::NotAllowed, (bucket ? "the bucket '" : "the host '") + host + "' isn't allowed");
    }
    found->scheme = endpoint.scheme;
    found->name = endpoint.id;
    found->endpoints = {endpoint};
    *folder = path;
    return Located::success(true);
}

// Every page of a folder, as one listing.
Result<std::vector<ListedEntry>> allPages(const Stack& stack, const std::string& folder) {
    using Listed = Result<std::vector<ListedEntry>>;
    std::map<std::string, ListedEntry> merged;
    std::string from;
    for (int page = 0; page < 100000; ++page) {
        Result<ListedPage> listed = listFolderPage(stack, folder, from);
        if (!listed.ok()) return Listed::failure(listed.status, listed.message);
        const size_t before = merged.size();
        for (ListedEntry& entry : listed.value.entries) merged.emplace(entry.entry.name, std::move(entry));
        // A page with nothing new ends it: a server that doesn't move on (rclone, S3_RESOLVER.md §8.1).
        if (page > 0 && merged.size() == before) break;
        from = listed.value.next;
        if (from.empty()) break;
    }
    std::vector<ListedEntry> entries;
    for (auto& [name, entry] : merged) entries.push_back(std::move(entry));
    return Listed::success(std::move(entries));
}

}  // namespace

Result<std::vector<ListedEntry>> listUri(const std::string& uri, const Context& context) {
    Stack stack;
    std::string folder;
    const Result<bool> located = browsedStack(uri, context, &stack, &folder);
    if (!located.ok()) return Result<std::vector<ListedEntry>>::failure(located.status, located.message);
    return allPages(stack, folder);
}

Found findUri(const std::string& uri, const Context& context) {
    Stack stack;
    std::string path;
    const Result<bool> located = browsedStack(uri, context, &stack, &path);
    if (located.ok()) return findInStack(stack, path, nullptr);
    Found found;
    // Not "missing": no server was asked.
    found.status = located.status == Status::Missing ? Status::Error : located.status;
    found.message = located.message;
    return found;
}

Result<ListedPage> listUriPage(const std::string& uri, const Context& context, const std::string& from) {
    Stack stack;
    std::string folder;
    const Result<bool> located = browsedStack(uri, context, &stack, &folder);
    if (!located.ok()) return Result<ListedPage>::failure(located.status, located.message);
    return listFolderPage(stack, folder, from);
}

Result<std::vector<ListedEntry>> listFolder(const Stack& stack, const std::string& folder, Lookups*) {
    return allPages(stack, folder);
}

Result<ListedPage> listFolderPage(const Stack& stack, const std::string& folder, const std::string& from) {
    using Paged = Result<ListedPage>;
    const std::shared_ptr<Transport> transport = transportFor(stack.scheme);
    if (!transport) return Paged::failure(Status::Error, "no protocol plugin is loaded for " + stack.scheme + "://");
    if (isOffline()) return Paged::failure(Status::Unreachable, "offline");

    // `from` is, per endpoint that has more, "<its index>=<where its next page starts>", one
    // a line. Empty: every endpoint from its start.
    const bool start = from.empty();
    std::map<size_t, std::string> resume;
    std::istringstream lines(from);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equals = line.find('=');
        if (equals != std::string::npos) resume[static_cast<size_t>(std::stoul(line.substr(0, equals)))] = line.substr(equals + 1);
    }
    std::map<std::string, ListedEntry> merged;
    std::string next;
    bool anywhere = false;
    for (size_t i = 0; i < stack.endpoints.size(); ++i) {
        const Endpoint& endpoint = stack.endpoints[i];
        if (!start && !resume.count(i)) continue;
        const std::string token = start ? std::string() : resume[i];
        const Result<EntryPage> page = withMirrors(endpoint, [&transport, &folder, &token](const Endpoint& at) {
            Cancel cancel;
            return transport->listPage(at, folder, token, cancel);
        });
        if (page.status == Status::Missing) continue;
        if (!page.ok()) {
            if (endpoint.optional) continue;
            return Paged::failure(page.status, page.message.empty() ? endpoint.id : endpoint.id + ": " + page.message);
        }
        anywhere = true;
        // emplace keeps what an upper endpoint already put there.
        for (const Entry& entry : page.value.entries) merged.emplace(entry.name, ListedEntry{entry, endpoint.id});
        if (!page.value.next.empty()) next += std::to_string(i) + "=" + page.value.next + "\n";
    }
    // A later page that's empty is the end of one; only a first one says the folder isn't there.
    if (!anywhere && start) return Paged::failure(Status::Missing);
    ListedPage listed;
    for (auto& [name, entry] : merged) listed.entries.push_back(std::move(entry));
    listed.next = std::move(next);
    return Paged::success(std::move(listed));
}

}  // namespace usdcc::assets
