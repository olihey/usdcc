#pragma once

// Finding a file in a stack (docs/REMOTE_ASSETS.md §5, §7). Resolve asks for folder
// listings, not single files: one listing answers for every sibling. What servers
// answered is remembered, in a cache scope for as long as it lasts and otherwise for a
// few seconds.

#include "usdcc/assets/context.h"
#include "usdcc/assets/result.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace usdcc::assets {

// What servers answered. One for the process (answers expire), and one per cache scope
// (they don't: a composition sees one consistent state).
class Lookups {
public:
    using Listing = Result<std::map<std::string, Entry>>;

    // Not expiring is the default: that's what a cache scope makes.
    explicit Lookups(bool expires = false) : m_expires(expires) {}

    bool listing(const std::string& key, Listing* listing);
    void setListing(const std::string& key, const Listing& listing);
    bool entry(const std::string& key, Result<Entry>* entry);
    void setEntry(const std::string& key, const Result<Entry>& entry);
    void clear();
    // What any endpoint of `stack` said about `path` and its folder: after a write to it.
    void forget(const Stack& stack, const std::string& path);

private:
    template <class T>
    struct Timed {
        T value;
        std::chrono::steady_clock::time_point when;
    };
    bool fresh(std::chrono::steady_clock::time_point when) const;

    const bool m_expires;
    std::mutex m_mutex;
    std::map<std::string, Timed<Listing>> m_listings;
    std::map<std::string, Timed<Result<Entry>>> m_entries;
};

// The process's own, used outside a cache scope.
Lookups& sharedLookups();
// How long an answer is used outside a cache scope. Five seconds unless set.
void setLookupLifetime(std::chrono::milliseconds lifetime);
// Drops every remembered answer of the process.
void forgetLookups();

// ---- Mirrors (§4) ----------------------------------------------------------------------

// A URL that didn't answer is tried after an endpoint's other URLs for half a minute,
// so that not every request waits for its timeout before going to a mirror.
bool isMarkedDown(const std::string& url);
void markDown(const std::string& url);
void forgetDownMarks();

// `call` with the endpoint as it is, then with each mirror's URL in its place, until
// one is reachable. URLs marked down are tried last. The result is the first that
// isn't Unreachable, or the last one's.
template <class Call>
auto withMirrors(const Endpoint& endpoint, Call&& call) -> decltype(call(endpoint)) {
    std::vector<std::string> urls;
    urls.push_back(endpoint.url);
    urls.insert(urls.end(), endpoint.mirrors.begin(), endpoint.mirrors.end());
    std::stable_partition(urls.begin(), urls.end(), [](const std::string& url) { return !isMarkedDown(url); });
    decltype(call(endpoint)) result;
    for (const std::string& url : urls) {
        Endpoint at = endpoint;
        at.url = url;
        result = call(at);
        if (result.status != Status::Unreachable) return result;
        markDown(url);
    }
    return result;
}

// Where a path was found.
struct Found {
    Status status = Status::Missing;  // Ok, Missing, or why the search stopped
    std::string message;
    Endpoint endpoint;  // the winner; when the search stopped, the endpoint that stopped it
    Entry entry;
    // Offline: the cached version it resolved to.
    std::filesystem::path cachedFile;
};

// The first endpoint of `stack` that has `path` wins. A missing file moves on to the
// next endpoint; an unreachable endpoint ends the search unless it's optional (§5).
// `scope` is the current cache scope's answers, or null.
Found findInStack(const Stack& stack, const std::string& path, Lookups* scope);

struct ListedEntry {
    Entry entry;
    std::string origin;  // the id of the endpoint it came from
};

// The union of the folder on every endpoint, the upper one winning, sorted by name.
// Missing when no endpoint has the folder.
Result<std::vector<ListedEntry>> listFolder(const Stack& stack, const std::string& folder, Lookups* scope = nullptr);

// The folder `uri` names (webdav://studio/chars/, webdav://192.168.1.5:5000/single/), listed
// as listFolder() lists one: a name `context`'s environment has, else a server by address
// whose host is allowed (activity.h). Every page of it.
Result<std::vector<ListedEntry>> listUri(const std::string& uri, const Context& context);

// A page of a folder, and where the next starts: what the asset picker browses with, so a
// folder of thousands shows its first entries at once (docs/S3_RESOLVER.md §5).
struct ListedPage {
    std::vector<ListedEntry> entries;
    std::string next;  // `from` for the next page; empty when that was all
};
// One page from each endpoint of the stack that has more, merged as listFolder() merges.
// An entry an upper endpoint has on a later page than a lower one's may come twice.
Result<ListedPage> listFolderPage(const Stack& stack, const std::string& folder, const std::string& from = {});
Result<ListedPage> listUriPage(const std::string& uri, const Context& context, const std::string& from = {});

}  // namespace usdcc::assets
