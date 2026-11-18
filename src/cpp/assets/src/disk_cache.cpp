#include "usdcc/assets/disk_cache.h"

#include "usdcc/assets/config.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>
#include <pxr/base/tf/getenv.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

namespace usdcc::assets {

namespace {

namespace fs = std::filesystem;

// Longer than this and the cache file gets a hashed name: Windows' limit is 260
// characters for the whole path, and the cache's own folder comes first.
constexpr size_t kLongestKeptPath = 120;
// Another process's lock that nothing has touched for this long was left by a crash.
constexpr auto kStaleLock = std::chrono::seconds(30);
constexpr auto kLockHeartbeat = std::chrono::seconds(5);
constexpr uint64_t kTrimEvery = 64ull * 1024 * 1024;

bool keepable(const std::string& path) {
    if (path.size() > kLongestKeptPath) return false;
    for (const unsigned char c : path) {
        // Not valid in a Windows file name, or with a meaning of its own there or here.
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\\' ||
            c == '%' || c == '@') {
            return false;
        }
    }
    return true;
}

// Of the path, not of a query string after it.
std::string extensionOf(const std::string& path) {
    const std::string plain = path.substr(0, path.find('?'));
    const size_t slash = plain.rfind('/');
    const size_t dot = plain.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 == plain.size()) return {};
    const std::string extension = plain.substr(dot);
    return keepable(extension) && extension.size() <= 16 ? extension : std::string();
}

fs::path recordOf(const fs::path& file) {
    fs::path record = file;
    record += ".meta";
    return record;
}

bool isVersionFile(const fs::path& file) {
    const std::string name = toUtf8(file.filename());
    if (name.find('@') == std::string::npos) return false;
    for (const char* other : {".meta", ".part", ".lock"}) {
        const std::string suffix = other;
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) return false;
    }
    return true;
}

void writeRecord(const fs::path& file, const Endpoint& endpoint, const std::string& path, const Entry& entry) {
    PXR_NAMESPACE_USING_DIRECTIVE
    JsObject record;
    record["url"] = JsValue(endpoint.url + path);
    record["endpoint"] = JsValue(endpoint.id);
    record["etag"] = JsValue(entry.etag);
    record["modified"] = JsValue(entry.modified);
    record["size"] = JsValue(static_cast<uint64_t>(entry.size));
    std::ofstream output(recordOf(file), std::ios::binary | std::ios::trunc);
    output << JsWriteToString(JsValue(record));
}

std::optional<Entry> readRecord(const fs::path& file) {
    PXR_NAMESPACE_USING_DIRECTIVE
    std::ifstream input(recordOf(file), std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream text;
    text << input.rdbuf();
    const JsValue parsed = JsParseString(text.str());
    if (!parsed.IsObject()) return std::nullopt;
    const JsObject& object = parsed.GetJsObject();
    Entry entry;
    const auto etag = object.find("etag");
    if (etag != object.end() && etag->second.IsString()) entry.etag = etag->second.GetString();
    const auto modified = object.find("modified");
    if (modified != object.end() && (modified->second.IsReal() || modified->second.IsInt())) entry.modified = modified->second.GetReal();
    const auto size = object.find("size");
    if (size != object.end() && size->second.IsInt()) entry.size = size->second.GetUInt64();
    return entry;
}

// The record's time is the version's last use, which is what trim() goes by.
void touch(const fs::path& file) {
    std::error_code error;
    fs::last_write_time(file, fs::file_time_type::clock::now(), error);
}

fs::file_time_type lastUse(const fs::path& file) {
    std::error_code error;
    fs::file_time_type time = fs::last_write_time(recordOf(file), error);
    if (error) time = fs::last_write_time(file, error);
    return time;
}

// A folder beside the file a download goes to, held by the process downloading it:
// making a folder either works or says it's there, in one step, on every platform.
class DownloadLock {
public:
    explicit DownloadLock(const fs::path& file) : m_lock(file) { m_lock += ".lock"; }
    ~DownloadLock() { release(); }

    // False when the file appeared while waiting: someone else downloaded it.
    bool acquire(const fs::path& file) {
        std::error_code error;
        for (;;) {
            if (fs::create_directory(m_lock, error)) break;
            if (fs::is_regular_file(file, error)) return false;
            const fs::file_time_type touched = fs::last_write_time(m_lock, error);
            if (error) continue;  // gone meanwhile: try for it
            if (fs::file_time_type::clock::now() - touched > kStaleLock) {
                fs::remove(m_lock, error);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        m_held = true;
        m_heartbeat = std::thread([this]() {
            std::unique_lock<std::mutex> lock(m_mutex);
            while (!m_condition.wait_for(lock, kLockHeartbeat, [this]() { return !m_held; })) touch(m_lock);
        });
        return true;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_held) return;
            m_held = false;
        }
        m_condition.notify_all();
        if (m_heartbeat.joinable()) m_heartbeat.join();
        std::error_code error;
        fs::remove(m_lock, error);
    }

private:
    fs::path m_lock;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_held = false;
    std::thread m_heartbeat;
};

std::unique_ptr<DiskCache>& sharedCache() {
    static std::unique_ptr<DiskCache> instance;
    return instance;
}

std::mutex& sharedMutex() {
    static std::mutex mutex;
    return mutex;
}

fs::path defaultRoot() {
    const std::string named = PXR_NS::TfGetenv("USDCC_ASSET_CACHE");
    if (!named.empty()) return fromUtf8(named);
    return userDataFolder() / "asset-cache";
}

}  // namespace

DiskCache::DiskCache(fs::path root) : m_root(std::move(root)) {}

DiskCache& DiskCache::shared() {
    std::lock_guard<std::mutex> lock(sharedMutex());
    if (!sharedCache()) sharedCache() = std::make_unique<DiskCache>(defaultRoot());
    return *sharedCache();
}

void DiskCache::setSharedRoot(const fs::path& root) {
    std::lock_guard<std::mutex> lock(sharedMutex());
    std::optional<uint64_t> limit;
    std::optional<uint64_t> configLimit;
    if (sharedCache()) {
        std::lock_guard<std::mutex> old(sharedCache()->m_mutex);
        limit = sharedCache()->m_limit;
        configLimit = sharedCache()->m_configLimit;
    }
    // Leaked on purpose when replaced: a thread may be in the old one's ensure().
    sharedCache().release();
    sharedCache() = std::make_unique<DiskCache>(root);
    sharedCache()->m_limit = limit;
    sharedCache()->m_configLimit = configLimit;
}

fs::path DiskCache::fileFor(const Endpoint& endpoint, const std::string& path, const Entry& version) const {
    const std::string extension = extensionOf(path);
    const std::string suffix = "@" + version.versionKey() + extension;
    fs::path file = m_root / endpoint.cacheId();
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

void DiskCache::handedOut(const fs::path& file) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handedOut[toUtf8(file)] = std::chrono::steady_clock::now();
}

std::optional<fs::path> DiskCache::find(const Endpoint& endpoint, const std::string& path, const Entry& version) {
    const fs::path file = fileFor(endpoint, path, version);
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return std::nullopt;
    touch(recordOf(file));
    handedOut(file);
    return file;
}

std::optional<std::pair<fs::path, Entry>> DiskCache::findAny(const Endpoint& endpoint, const std::string& path) {
    // Every version's name starts and ends the same; only the key between differs.
    const fs::path pattern = fileFor(endpoint, path, Entry());
    const std::string name = toUtf8(pattern.filename());
    const std::string prefix = name.substr(0, name.find('@') + 1);
    const std::string extension = extensionOf(path);

    std::optional<std::pair<fs::path, Entry>> newest;
    fs::file_time_type newestUse{};
    std::error_code error;
    for (const fs::directory_entry& each : fs::directory_iterator(pattern.parent_path(), error)) {
        const std::string other = toUtf8(each.path().filename());
        if (other.rfind(prefix, 0) != 0 || !isVersionFile(each.path())) continue;
        if (other.size() != prefix.size() + 8 + extension.size()) continue;
        const std::optional<Entry> entry = readRecord(each.path());
        if (!entry) continue;
        const fs::file_time_type used = lastUse(each.path());
        if (!newest || used > newestUse) {
            newest = std::make_pair(each.path(), *entry);
            newestUse = used;
        }
    }
    if (newest) {
        touch(recordOf(newest->first));
        handedOut(newest->first);
    }
    return newest;
}

Result<fs::path> DiskCache::ensure(const Endpoint& endpoint, const std::string& path, const Entry& version,
                                   const Download& download) {
    using FileResult = Result<fs::path>;
    if (const std::optional<fs::path> cached = find(endpoint, path, version)) return FileResult::success(*cached);
    const fs::path file = fileFor(endpoint, path, version);
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
    uint64_t added = 0;
    try {
        std::error_code error;
        fs::create_directories(file.parent_path(), error);
        DownloadLock lock(file);
        // Not acquired means the file is there: another process, or another thread
        // between find() and taking the slot, finished it.
        if (!lock.acquire(file) || fs::is_regular_file(file, error)) {
            result = FileResult::success(file);
        } else {
            static std::atomic<uint64_t> counter{0};
            fs::path part = file;
            part += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000) + "-" +
                    std::to_string(counter.fetch_add(1)) + ".part";
            const Result<Entry> downloaded = download(part);
            if (!downloaded.ok()) {
                fs::remove(part, error);
                result = FileResult::failure(downloaded.status, downloaded.message);
            } else {
                fs::rename(part, file, error);
                if (error) fs::remove(part, error);
                if (fs::is_regular_file(file, error)) {
                    writeRecord(file, endpoint, path, version);
                    added = fs::file_size(file, error);
                    result = FileResult::success(file);
                } else {
                    result = FileResult::failure(Status::Error, "can't write " + toUtf8(file));
                }
            }
        }
    } catch (const std::exception& failure) {
        result = FileResult::failure(Status::Error, failure.what());
    }
    bool trimNow = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (result.ok()) m_handedOut[key] = std::chrono::steady_clock::now();
        m_addedSinceTrim += added;
        if (m_addedSinceTrim >= std::min(kTrimEvery, effectiveLimit() / 8 + 1)) {
            m_addedSinceTrim = 0;
            trimNow = true;
        }
    }
    promise.set_value(result);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_inFlight.erase(key);
    }
    if (trimNow) trim();
    return result;
}

uint64_t DiskCache::effectiveLimit() const {
    if (m_limit) return *m_limit;
    if (m_configLimit) return *m_configLimit;
    return kDefaultLimit;
}

void DiskCache::limitChanged(uint64_t before) {
    // A smaller cache: the next download trims, whatever it adds.
    if (effectiveLimit() < before) m_addedSinceTrim = kTrimEvery;
}

void DiskCache::setLimit(std::optional<uint64_t> bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t before = effectiveLimit();
    m_limit = bytes;
    limitChanged(before);
}

void DiskCache::setConfigLimit(std::optional<uint64_t> bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t before = effectiveLimit();
    m_configLimit = bytes;
    limitChanged(before);
}

uint64_t DiskCache::limit() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return effectiveLimit();
}

uint64_t DiskCache::size() const {
    uint64_t total = 0;
    std::error_code error;
    for (const fs::directory_entry& each : fs::recursive_directory_iterator(m_root, error)) {
        if (each.is_regular_file(error) && isVersionFile(each.path())) total += each.file_size(error);
    }
    return total;
}

DiskCache::Cleared DiskCache::clear(std::chrono::seconds grace) {
    std::set<std::string> kept;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [file, when] : m_handedOut) {
            if (now - when < grace) kept.insert(file);
        }
        m_addedSinceTrim = 0;
    }
    Cleared cleared;
    std::vector<fs::path> versions;
    std::vector<fs::path> records;
    std::vector<fs::path> parts;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(m_root, error); !error && it != fs::recursive_directory_iterator();
         it.increment(error)) {
        const fs::path& file = it->path();
        const std::string name = toUtf8(file.filename());
        const auto endsWith = [&name](std::string_view suffix) {
            return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (it->is_directory(error)) {
            // A download's lock: its owner removes it.
            if (endsWith(".lock")) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(error)) continue;
        if (isVersionFile(file)) {
            versions.push_back(file);
        } else if (endsWith(".meta")) {
            records.push_back(file);
        } else if (endsWith(".part")) {
            parts.push_back(file);
        }
    }
    for (const fs::path& version : versions) {
        const uint64_t size = fs::file_size(version, error);
        // Open in some process: Windows refuses, and it stays with its record.
        if (kept.count(toUtf8(version)) || !fs::remove(version, error) || error) {
            ++cleared.kept;
            continue;
        }
        ++cleared.files;
        cleared.bytes += size;
    }
    // Records whose version is gone, now or before.
    for (const fs::path& record : records) {
        fs::path version = record;
        version.replace_extension();
        if (!fs::exists(version, error)) fs::remove(record, error);
    }
    // A download still running writes to its part; one untouched for a while was cut off.
    const auto stale = fs::file_time_type::clock::now() - kStaleLock;
    for (const fs::path& part : parts) {
        const fs::file_time_type written = fs::last_write_time(part, error);
        if (!error && written < stale) fs::remove(part, error);
    }
    return cleared;
}

void DiskCache::trim(std::chrono::seconds grace) {
    struct Version {
        fs::path file;
        uint64_t size;
        fs::file_time_type used;
    };
    std::vector<Version> versions;
    uint64_t total = 0;
    std::error_code error;
    for (const fs::directory_entry& each : fs::recursive_directory_iterator(m_root, error)) {
        if (!each.is_regular_file(error) || !isVersionFile(each.path())) continue;
        const uint64_t size = each.file_size(error);
        versions.push_back({each.path(), size, lastUse(each.path())});
        total += size;
    }
    uint64_t limit = 0;
    std::set<std::string> kept;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        limit = effectiveLimit();
        m_addedSinceTrim = 0;
        const auto now = std::chrono::steady_clock::now();
        for (auto it = m_handedOut.begin(); it != m_handedOut.end();) {
            if (now - it->second < grace) {
                kept.insert(it->first);
                ++it;
            } else {
                it = m_handedOut.erase(it);
            }
        }
    }
    if (total <= limit) return;
    std::sort(versions.begin(), versions.end(), [](const Version& a, const Version& b) { return a.used < b.used; });
    for (const Version& version : versions) {
        if (total <= limit) break;
        if (kept.count(toUtf8(version.file))) continue;
        // Another process may have it open: on Windows that refuses, and it stays.
        if (!fs::remove(version.file, error) || error) continue;
        fs::remove(recordOf(version.file), error);
        total -= std::min(total, version.size);
    }
}

}  // namespace usdcc::assets
