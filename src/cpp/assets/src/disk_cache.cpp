#include "usdcc/assets/disk_cache.h"

#include "usdcc/assets/config.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>
#include <pxr/base/tf/getenv.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>

namespace usdcc::assets {

namespace {

// Longer than this and the cache file gets a hashed name: Windows' limit is 260
// characters for the whole path, and the cache's own folder comes first.
constexpr size_t kLongestKeptPath = 120;

bool keepable(const std::string& path) {
    if (path.size() > kLongestKeptPath) return false;
    for (const unsigned char c : path) {
        // Not valid in a Windows file name, or with a meaning of its own there.
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\\' ||
            c == '%' || c == '@') {
            return false;
        }
    }
    return true;
}

std::string extensionOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 == path.size()) return {};
    const std::string extension = path.substr(dot);
    return keepable(extension) && extension.size() <= 16 ? extension : std::string();
}

std::unique_ptr<DiskCache>& sharedCache() {
    static std::unique_ptr<DiskCache> instance;
    return instance;
}

std::mutex& sharedMutex() {
    static std::mutex mutex;
    return mutex;
}

std::filesystem::path defaultRoot() {
    const std::string named = PXR_NS::TfGetenv("USDCC_ASSET_CACHE");
    if (!named.empty()) return fromUtf8(named);
    return userDataFolder() / "asset-cache";
}

void writeRecord(const std::filesystem::path& file, const Endpoint& endpoint, const std::string& path, const Entry& entry) {
    PXR_NAMESPACE_USING_DIRECTIVE
    JsObject record;
    record["url"] = JsValue(endpoint.url + path);
    record["endpoint"] = JsValue(endpoint.id);
    record["etag"] = JsValue(entry.etag);
    record["modified"] = JsValue(entry.modified);
    record["size"] = JsValue(static_cast<uint64_t>(entry.size));
    std::filesystem::path recordFile = file;
    recordFile += ".meta";
    std::ofstream output(recordFile, std::ios::binary | std::ios::trunc);
    output << JsWriteToString(JsValue(record));
}

// The record's time is the version's last use, which is what eviction will go by.
void touchRecord(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::path record = file;
    record += ".meta";
    std::filesystem::last_write_time(record, std::filesystem::file_time_type::clock::now(), error);
}

}  // namespace

DiskCache::DiskCache(std::filesystem::path root) : m_root(std::move(root)) {}

DiskCache& DiskCache::shared() {
    std::lock_guard<std::mutex> lock(sharedMutex());
    if (!sharedCache()) sharedCache() = std::make_unique<DiskCache>(defaultRoot());
    return *sharedCache();
}

void DiskCache::setSharedRoot(const std::filesystem::path& root) {
    std::lock_guard<std::mutex> lock(sharedMutex());
    // Leaked on purpose when replaced: a thread may be in the old one's ensure().
    sharedCache().release();
    sharedCache() = std::make_unique<DiskCache>(root);
}

std::filesystem::path DiskCache::fileFor(const Endpoint& endpoint, const std::string& path, const Entry& version) const {
    const std::string extension = extensionOf(path);
    const std::string suffix = "@" + version.versionKey() + extension;
    std::filesystem::path file = m_root / endpoint.cacheId();
    if (keepable(path)) {
        // "/chars/hero.usd" -> chars/hero@3f9a1c2e.usd
        const std::string stem = path.substr(1, path.size() - 1 - extension.size());
        file /= fromUtf8(stem + suffix);
    } else {
        file /= "_";
        file /= stableHash(path, 16) + suffix;
    }
    return file;
}

std::optional<std::filesystem::path> DiskCache::find(const Endpoint& endpoint, const std::string& path,
                                                     const Entry& version) const {
    const std::filesystem::path file = fileFor(endpoint, path, version);
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return std::nullopt;
    touchRecord(file);
    return file;
}

Result<std::filesystem::path> DiskCache::ensure(const Endpoint& endpoint, const std::string& path, const Entry& version,
                                                const Download& download) {
    using FileResult = Result<std::filesystem::path>;
    if (const std::optional<std::filesystem::path> cached = find(endpoint, path, version)) {
        return FileResult::success(*cached);
    }
    const std::filesystem::path file = fileFor(endpoint, path, version);
    const std::string key = toUtf8(file);

    std::promise<FileResult> promise;
    std::shared_future<FileResult> running;
    bool mine = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_inFlight.find(key);
        if (found != m_inFlight.end()) {
            running = found->second;
        } else {
            running = promise.get_future().share();
            m_inFlight[key] = running;
            mine = true;
        }
    }
    if (!mine) return running.get();

    FileResult result;
    try {
        // Another thread may have finished it between find() and taking the slot.
        std::error_code error;
        if (std::filesystem::is_regular_file(file, error)) {
            result = FileResult::success(file);
        } else {
            std::filesystem::create_directories(file.parent_path(), error);
            static std::atomic<uint64_t> counter{0};
            std::filesystem::path part = file;
            part += "." + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()) % 100000) + "-" +
                    std::to_string(counter.fetch_add(1)) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) + ".part";
            const Result<Entry> downloaded = download(part);
            if (!downloaded.ok()) {
                std::filesystem::remove(part, error);
                result = FileResult::failure(downloaded.status, downloaded.message);
            } else {
                // Not over a file that's there: another process finished first, and its
                // file may already be mapped by a reader.
                std::filesystem::rename(part, file, error);
                if (error) std::filesystem::remove(part, error);
                if (std::filesystem::is_regular_file(file, error)) {
                    writeRecord(file, endpoint, path, version);
                    result = FileResult::success(file);
                } else {
                    result = FileResult::failure(Status::Error, "can't write " + toUtf8(file));
                }
            }
        }
    } catch (const std::exception& failure) {
        result = FileResult::failure(Status::Error, failure.what());
    }
    promise.set_value(result);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_inFlight.erase(key);
    }
    return result;
}

}  // namespace usdcc::assets
