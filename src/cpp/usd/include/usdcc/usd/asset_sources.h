#pragma once

// What the asset picker browses (docs/ASSET_PICKER.md §2). OpenUSD's ArResolver can't
// list a folder for any scheme, so browsing is an interface of its own: a source lists
// the folders of the schemes it serves. usdcc registers one for its own protocols that
// can list (webdav://, webdavs://); a plugin can add one for its own resolver. Qt-free.

#include "usdcc/assets/transport.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/resolverContext.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::usd {

// A place to start browsing a scheme from: "studio" at "webdav://studio/".
struct AssetRoot {
    std::string label;
    std::string uri;
};

struct AssetListEntry {
    std::string name;
    bool isFolder = false;
    uint64_t size = 0;
    double modified = 0;  // seconds since the epoch; 0 when not known
    std::string origin;   // the server it came from ("test"); empty when there is only one
};

// A page of a folder's entries, and where the next starts: empty when that was all.
struct AssetListPage {
    std::vector<AssetListEntry> entries;
    std::string next;
};

// What list() throws when a server refused for want of a login the user can save
// (Edit > Asset Servers > Logins): the picker offers to log in, then lists again.
class AssetLoginNeeded : public std::runtime_error {
public:
    AssetLoginNeeded(const std::string& message, std::string credential, std::string scheme)
        : std::runtime_error(message), m_credential(std::move(credential)), m_scheme(std::move(scheme)) {}
    // The credential id the refusing server's login is saved under.
    const std::string& credential() const { return m_credential; }
    const std::string& scheme() const { return m_scheme; }

private:
    std::string m_credential;
    std::string m_scheme;
};

class AssetSource {
public:
    virtual ~AssetSource();
    virtual std::string id() const = 0;
    // What Places calls it: "WebDAV".
    virtual std::string label() const = 0;
    // The URI schemes it lists ("webdav", "webdavs").
    virtual std::vector<std::string> schemes() const = 0;
    // Where browsing a scheme starts, in the stage's context. Quick: no server is asked.
    virtual std::vector<AssetRoot> roots(const std::string& scheme, const PXR_NS::ArResolverContext& context) const = 0;
    // The folder `uri`'s entries. On a worker thread; may take long, and should stop when
    // `cancel` is. Throws std::runtime_error with a message for the dialog, or
    // AssetLoginNeeded when a login would help.
    virtual std::vector<AssetListEntry> list(const std::string& uri, const PXR_NS::ArResolverContext& context,
                                             usdcc::assets::Cancel& cancel) = 0;
    // One page of them, from where `from` says (empty: the start), as list() throws: what the
    // picker asks, so a folder of thousands shows its first entries while the rest waits for
    // Show More. Unless overridden, all of list() as one page.
    virtual AssetListPage listPage(const std::string& uri, const PXR_NS::ArResolverContext& context,
                                   usdcc::assets::Cancel& cancel, const std::string& from) {
        AssetListPage page;
        if (from.empty()) page.entries = list(uri, context, cancel);
        return page;
    }
};

class AssetSourceRegistry {
public:
    // With usdcc's own source in it, for the protocols of remote assets that can list.
    static AssetSourceRegistry& instance();

    void add(std::shared_ptr<AssetSource> source);
    void remove(const std::string& id);
    std::vector<std::shared_ptr<AssetSource>> sources() const;
    // The source that lists `scheme`; null when none does.
    std::shared_ptr<AssetSource> forScheme(const std::string& scheme) const;
    // Every scheme a source lists, sorted.
    std::vector<std::string> browsableSchemes() const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<AssetSource>> m_sources;
};

// The URI schemes OpenUSD has a resolver for (ArGetRegisteredURISchemes), sorted, with the
// resolver type each comes from (its plugInfo.json name) for a tooltip.
std::vector<std::string> registeredUriSchemes();
std::string resolverTypeForScheme(const std::string& scheme);

}  // namespace usdcc::usd
