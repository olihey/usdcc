#pragma once

// Finding a file in a stack (docs/REMOTE_ASSETS.md §5, §7). Resolve asks for folder
// listings, not single files: one listing answers for every sibling. What servers
// answered is remembered, in a cache scope for as long as it lasts and otherwise for a
// few seconds.

#include "usdcc/assets/context.h"
#include "usdcc/assets/result.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"

#include <chrono>
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

// Where a path was found.
struct Found {
    Status status = Status::Missing;  // Ok, Missing, or why the search stopped
    std::string message;
    Endpoint endpoint;  // the winner; when the search stopped, the endpoint that stopped it
    Entry entry;
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

}  // namespace usdcc::assets
