#pragma once

// The disk cache (docs/REMOTE_ASSETS.md §7). A file is stored per version, never
// overwritten:
//
//     <root>/<endpoint cache id>/chars/hero@3f9a1c2e.usd        the bytes
//     <root>/<endpoint cache id>/chars/hero@3f9a1c2e.usd.meta   ETag, time, size, where from
//
// Windows can't replace a file the .usdc reader has mapped, an open layer keeps the
// bytes it loaded until Reload, and TEST's and PROD's versions of one path sit side
// by side.

#include "usdcc/assets/result.h"
#include "usdcc/assets/transport.h"

#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace usdcc::assets {

class DiskCache {
public:
    // Downloads one version to the file it's given. The Entry it returns is recorded.
    using Download = std::function<Result<Entry>(const std::filesystem::path& into)>;

    explicit DiskCache(std::filesystem::path root);

    // USDCC_ASSET_CACHE, or else asset-cache in userDataFolder(). One for the process.
    static DiskCache& shared();
    // Moves the shared cache, for the app and for tests. Files already handed out stay where they are.
    static void setSharedRoot(const std::filesystem::path& root);

    const std::filesystem::path& root() const { return m_root; }

    // Where this version of `path` on `endpoint` is kept, there or not.
    std::filesystem::path fileFor(const Endpoint& endpoint, const std::string& path, const Entry& version) const;

    // The file, if this version is in the cache.
    std::optional<std::filesystem::path> find(const Endpoint& endpoint, const std::string& path, const Entry& version) const;

    // The file, downloaded first if it isn't there. However many threads ask for one
    // version at once, `download` runs once and they all get its result. Two processes
    // may both download; each writes a file of its own and the first to finish is kept.
    Result<std::filesystem::path> ensure(const Endpoint& endpoint, const std::string& path, const Entry& version,
                                         const Download& download);

private:
    std::filesystem::path m_root;
    std::mutex m_mutex;
    std::map<std::string, std::shared_future<Result<std::filesystem::path>>> m_inFlight;
};

}  // namespace usdcc::assets
