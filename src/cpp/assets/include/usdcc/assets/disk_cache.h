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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

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
    std::optional<std::filesystem::path> find(const Endpoint& endpoint, const std::string& path, const Entry& version);

    // Any version of `path` that is in the cache, the one used last: what offline mode
    // resolves to. With the Entry its record holds.
    std::optional<std::pair<std::filesystem::path, Entry>> findAny(const Endpoint& endpoint, const std::string& path);

    // The file, downloaded first if it isn't there. However many threads ask for one
    // version at once, `download` runs once and they all get its result. Another
    // process downloading the same version is waited for (a lock beside the file).
    Result<std::filesystem::path> ensure(const Endpoint& endpoint, const std::string& path, const Entry& version,
                                         const Download& download);

    // Twenty gigabytes unless set. Checked after downloads, not on every one.
    void setLimit(uint64_t bytes);
    uint64_t limit() const;
    // Removes the versions used longest ago until the cache is within its limit. One
    // this process was handed within `grace` stays: its path may not be opened yet. An
    // open one is safe either way: Windows refuses to remove it, and elsewhere its
    // bytes stay until it's closed.
    void trim(std::chrono::seconds grace = std::chrono::seconds(60));
    // The bytes of every version in the cache.
    uint64_t size() const;

private:
    void handedOut(const std::filesystem::path& file);

    std::filesystem::path m_root;
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_future<Result<std::filesystem::path>>> m_inFlight;
    std::map<std::string, std::chrono::steady_clock::time_point> m_handedOut;
    uint64_t m_limit = 20ull * 1024 * 1024 * 1024;
    uint64_t m_addedSinceTrim = 0;
};

}  // namespace usdcc::assets
