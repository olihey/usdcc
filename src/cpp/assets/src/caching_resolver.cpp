#include "usdcc/assets/caching_resolver.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/credentials.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/paths.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/filesystemAsset.h>
#include <pxr/usd/ar/filesystemWritableAsset.h>
#include <pxr/usd/ar/notice.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/timestamp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::assets {

namespace {

// A number that differs whenever the winning endpoint or the file's version does
// (docs/REMOTE_ASSETS.md §5): SdfLayer::Reload only asks whether it changed. The
// file's own time in whole seconds, with the endpoint and version in the fraction, so
// that a file appearing in TEST with PROD's time is still noticed.
double timestampOf(const Found& found) {
    const std::string identity = found.endpoint.cacheId() + "|" + found.entry.versionKey();
    const std::string hash = stableHash(identity, 6);
    const double fraction = static_cast<double>(std::stoul(hash, nullptr, 16)) / static_cast<double>(0x1000000);
    return std::floor(found.entry.modified) + fraction;
}

// One download, as the host's listener sees it, stopped by the listener or by
// cancelDownloads().
class Download : public Progress {
public:
    explicit Download(std::string url)
        : m_url(std::move(url)), m_listener(activityListener()), m_generation(downloadGeneration()) {
        if (m_listener) m_listener->started(m_url);
    }
    void finish(Status status) {
        if (m_listener) m_listener->finished(m_url, status);
    }
    void advanced(uint64_t done, uint64_t total) override {
        if (m_listener && !m_listener->advanced(m_url, done, total)) cancel.cancel();
        if (downloadGeneration() != m_generation) cancel.cancel();
    }

    Cancel cancel;

private:
    std::string m_url;
    std::shared_ptr<ActivityListener> m_listener;
    uint64_t m_generation;
};

// Which file each resolved path last opened as, for localFile(), and the version it was:
// what a write of it says it replaces (phase 6).
struct Opened {
    std::filesystem::path file;
    std::string endpoint;  // the cache id of the endpoint it came from
    std::string etag;      // its ETag there; may be empty
};

struct OpenedFiles {
    std::mutex mutex;
    std::map<std::string, Opened> files;
};

OpenedFiles& openedFiles() {
    static OpenedFiles instance;
    return instance;
}

void remember(const std::string& resolved, const std::filesystem::path& file, const Endpoint& endpoint, const Entry& entry) {
    OpenedFiles& all = openedFiles();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.files[resolved] = {file, endpoint.cacheId(), entry.etag};
}

std::optional<Opened> opened(const std::string& resolved) {
    OpenedFiles& all = openedFiles();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = all.files.find(resolved);
    if (found == all.files.end()) return std::nullopt;
    return found->second;
}

std::shared_ptr<ArAsset> openRemembered(const std::string& resolved, const std::filesystem::path& file, const Endpoint& endpoint,
                                        const Entry& entry) {
    remember(resolved, file, endpoint, entry);
    return ArFilesystemAsset::Open(ArResolvedPath(toUtf8(file)));
}

// A file of its own for each write, beside the cache, with the path's extension.
std::filesystem::path writeFileFor(const std::string& path) {
    static std::atomic<uint64_t> counter{0};
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    std::string extension;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash) && path.size() - dot <= 16) {
        extension = path.substr(dot);
    }
    const std::filesystem::path folder = DiskCache::shared().root() / "writes";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return folder / fromUtf8(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                             std::to_string(counter.fetch_add(1)) + extension);
}

}  // namespace

std::optional<std::filesystem::path> localFile(const std::string& resolvedPath) {
    if (!ArGetResolver().OpenAsset(ArResolvedPath(resolvedPath))) return std::nullopt;
    const std::optional<Opened> found = opened(resolvedPath);
    if (!found) return std::nullopt;
    return found->file;
}

// A write (phase 6): to a file of its own, and to the server on Close(). Close() puts the
// file to the stack's first endpoint that isn't read-only, only if that file is still the
// version this one was opened from; a conflict is an error, and nothing is written.
class CachingResolver::_WritableAsset final : public ArWritableAsset {
public:
    _WritableAsset(const CachingResolver* resolver, Location location, Endpoint target, std::string condition,
                   std::filesystem::path file, std::shared_ptr<ArWritableAsset> local)
        : m_resolver(resolver), m_location(std::move(location)), m_target(std::move(target)),
          m_condition(std::move(condition)), m_file(std::move(file)), m_local(std::move(local)) {}

    ~_WritableAsset() override {
        // As a file on disk is: one that isn't closed is when it goes.
        if (m_local) Close();
        removeFile();
    }

    size_t Write(const void* buffer, size_t count, size_t offset) override {
        return m_local ? m_local->Write(buffer, count, offset) : 0;
    }

    bool Close() override {
        if (!m_local) return m_closed;
        const bool written = m_local->Close();
        m_local.reset();
        if (!written) {
            TF_RUNTIME_ERROR("%s wasn't saved: its file couldn't be written on this machine", m_location.display.c_str());
            removeFile();
            return false;
        }
        m_closed = upload();
        removeFile();
        return m_closed;
    }

private:
    bool upload() {
        const std::shared_ptr<Transport> transport = transportFor(m_location.stack->scheme);
        if (!transport) {
            TF_RUNTIME_ERROR("%s wasn't saved: no protocol plugin is loaded for %s://", m_location.display.c_str(),
                             m_location.stack->scheme.c_str());
            return false;
        }
        const std::string& path = m_location.path;
        Download upload(m_target.url + path);
        const Result<Entry> put = withMirrors(m_target, [&](const Endpoint& at) {
            return transport->put(at, path, m_file, m_condition, upload, upload.cancel);
        });
        upload.finish(put.status);
        if (put.status == Status::Conflict) {
            TF_RUNTIME_ERROR("%s wasn't saved: the file changed on the server '%s' since it was opened%s (%s). Reload it, "
                             "and make the changes again.",
                             m_location.display.c_str(), m_target.id.c_str(),
                             m_condition.empty() ? " (a file is there now that this one wasn't opened from)" : "",
                             put.message.c_str());
            return false;
        }
        if (!put.ok()) {
            TF_RUNTIME_ERROR("%s wasn't saved: the server '%s' is %s%s%s", m_location.display.c_str(), m_target.id.c_str(),
                             statusName(put.status), put.message.empty() ? "" : ": ", put.message.c_str());
            return false;
        }
        // The new version, in the cache as if it had been downloaded: opening it again
        // costs nothing, and the next write says it replaces this one.
        const Result<std::filesystem::path> cached =
            DiskCache::shared().ensure(m_target, path, put.value, [this](const std::filesystem::path& into) {
                std::error_code error;
                std::filesystem::copy_file(m_file, into, std::filesystem::copy_options::overwrite_existing, error);
                if (error) return Result<Entry>::failure(Status::Error, error.message());
                return Result<Entry>::success(Entry());
            });
        if (cached.ok()) remember(m_location.resolved, cached.value, m_target, put.value);
        m_resolver->_Written(*m_location.stack, path);
        return true;
    }

    void removeFile() {
        std::error_code error;
        if (!m_file.empty()) std::filesystem::remove(m_file, error);
    }

    const CachingResolver* m_resolver;
    Location m_location;
    Endpoint m_target;
    std::string m_condition;
    std::filesystem::path m_file;
    std::shared_ptr<ArWritableAsset> m_local;
    bool m_closed = false;
};

CachingResolver::CachingResolver(std::string scheme) : m_scheme(std::move(scheme)) {}

CachingResolver::~CachingResolver() = default;

Lookups* CachingResolver::_Scope() const {
    const ArThreadLocalScopedCache<Lookups>::CachePtr cache = m_scopes.GetCurrentCache();
    // Kept alive by the scope on this thread's stack for as long as the caller runs.
    return cache.get();
}

bool CachingResolver::_Locate(const std::string& assetPath, Location* location, std::string* why) const {
    Uri uri;
    if (!parseUri(assetPath, &uri, why)) return false;
    location->path = uri.path;
    location->display = uri.logical();
    if (!uri.stackId.empty()) {
        location->stack = findStack(uri.stackId);
        if (!location->stack) {
            *why = "it names a server stack this process doesn't know";
            return false;
        }
    } else {
        const Context* bound = _GetCurrentContextObject<Context>();
        const std::shared_ptr<const Context> fallback = bound ? nullptr : defaultContext();
        const Context& context = bound ? *bound : *fallback;
        location->stack = context.stack(uri.scheme, uri.name);
        if (!location->stack) {
            *why = "the environment '" + context.environment() + "' has no server named '" + uri.name + "'";
            return false;
        }
    }
    location->resolved = uri.withStack(location->stack->id);
    return true;
}

Found CachingResolver::_Find(const std::string& assetPath, Location* location, bool warnIfUnlocated) const {
    Found found;
    std::string why;
    if (!_Locate(assetPath, location, &why)) {
        if (warnIfUnlocated) TF_WARN("%s isn't resolved: %s", assetPath.c_str(), why.c_str());
        return found;
    }
    found = findInStack(*location->stack, location->path, _Scope());
    if (found.status == Status::NotAllowed) {
        const std::string& credential = found.endpoint.credential;
        std::string why;
        if (credential.empty()) {
            why = "refused to answer without a login, and its endpoint names no credential";
        } else if (!credentialFor(credential)) {
            why = "wants a login, and none is saved under the credential '" + credential +
                  "' (Edit > Asset Server Login in usdcc, or USDCC_ASSETS_<ID>_USER and _PASSWORD)";
        } else {
            why = "refused the login saved under the credential '" + credential + "'";
        }
        TF_WARN("%s isn't resolved: the server '%s' %s (%s)", location->display.c_str(), found.endpoint.id.c_str(),
                why.c_str(), found.message.c_str());
    } else if (found.status != Status::Ok && found.status != Status::Missing) {
        TF_WARN("%s isn't resolved: the server '%s' is %s%s%s", location->display.c_str(), found.endpoint.id.c_str(),
                statusName(found.status), found.message.empty() ? "" : ": ", found.message.c_str());
    }
    return found;
}

std::string CachingResolver::_CreateIdentifier(const std::string& assetPath, const ArResolvedPath& anchorAssetPath) const {
    Uri uri;
    if (assetPath.find("://") != std::string::npos) {
        // One spelling of each asset. A stack id someone gave explicitly is kept.
        if (parseUri(assetPath, &uri)) return uri.withStack(uri.stackId);
        return assetPath;
    }
    // A path on disk referenced from a layer on a server stays what it is.
    if (assetPath.empty() || !TfIsRelativePath(assetPath)) return assetPath;
    Uri anchor;
    if (!parseUri(anchorAssetPath.GetPathString(), &anchor)) return assetPath;
    // Logical, though the anchor is a resolved path with a stack id (§3): the id would
    // otherwise be written into files, by Flatten for one.
    if (!anchorPath(anchor, assetPath, &uri)) {
        TF_WARN("%s, referenced from %s, goes above the server's root", assetPath.c_str(), anchor.logical().c_str());
        return assetPath;
    }
    return uri.logical();
}

std::string CachingResolver::_CreateIdentifierForNewAsset(const std::string& assetPath,
                                                          const ArResolvedPath& anchorAssetPath) const {
    return _CreateIdentifier(assetPath, anchorAssetPath);
}

ArResolvedPath CachingResolver::_Resolve(const std::string& assetPath) const {
    Location location;
    const Found found = _Find(assetPath, &location, true);
    if (found.status != Status::Ok) return ArResolvedPath();
    return ArResolvedPath(location.resolved);
}

ArResolvedPath CachingResolver::_ResolveForNewAsset(const std::string& assetPath) const {
    // Where it would be, there or not: whether it can be written is asked separately.
    Location location;
    std::string why;
    if (!_Locate(assetPath, &location, &why)) {
        TF_WARN("%s can't be created: %s", assetPath.c_str(), why.c_str());
        return ArResolvedPath();
    }
    return ArResolvedPath(location.resolved);
}

bool CachingResolver::_WriteTarget(const Location& location, Endpoint* target, std::string* why) const {
    const Stack& stack = *location.stack;
    const auto writable = std::find_if(stack.endpoints.begin(), stack.endpoints.end(),
                                       [](const Endpoint& endpoint) { return !endpoint.readOnly; });
    if (writable == stack.endpoints.end()) {
        std::string ids;
        for (const Endpoint& endpoint : stack.endpoints) ids += (ids.empty() ? "" : ", ") + endpoint.id;
        *why = "every server " + location.display + " is on is read-only (" + ids + ")";
        return false;
    }
    const std::shared_ptr<Transport> transport = transportFor(stack.scheme);
    if (!transport) {
        *why = "no protocol plugin is loaded for " + stack.scheme + "://";
        return false;
    }
    if (!transport->canPut()) {
        *why = "the " + stack.scheme + ":// protocol can't write to a server";
        return false;
    }
    if (isOffline()) {
        *why = "offline: nothing is written to a server";
        return false;
    }
    *target = *writable;
    return true;
}

void CachingResolver::_Written(const Stack& stack, const std::string& path) const {
    sharedLookups().forget(stack, path);
    if (Lookups* scope = _Scope()) scope->forget(stack, path);
}

std::shared_ptr<ArAsset> CachingResolver::_OpenAsset(const ArResolvedPath& resolvedPath) const {
    Location location;
    const Found found = _Find(resolvedPath.GetPathString(), &location, true);
    if (found.status != Status::Ok) return nullptr;
    // Offline, it resolved to a version in the cache, and that is the one to open.
    if (!found.cachedFile.empty()) {
        return openRemembered(resolvedPath.GetPathString(), found.cachedFile, found.endpoint, found.entry);
    }
    const std::shared_ptr<Transport> transport = transportFor(location.stack->scheme);
    if (!transport) return nullptr;

    const Endpoint& endpoint = found.endpoint;
    const std::string& path = location.path;
    const Result<std::filesystem::path> file = DiskCache::shared().ensure(
        endpoint, path, found.entry, [&transport, &endpoint, &path](const std::filesystem::path& into) {
            Download download(endpoint.url + path);
            const Result<Entry> fetched = withMirrors(endpoint, [&](const Endpoint& at) {
                return transport->fetch(at, path, into, std::string(), download, download.cancel);
            });
            download.finish(fetched.status);
            return fetched;
        });
    if (!file.ok()) {
        TF_WARN("%s can't be opened: the server '%s' is %s%s%s", location.display.c_str(), endpoint.id.c_str(),
                statusName(file.status), file.message.empty() ? "" : ": ", file.message.c_str());
        return nullptr;
    }
    return openRemembered(resolvedPath.GetPathString(), file.value, endpoint, found.entry);
}

std::shared_ptr<ArWritableAsset> CachingResolver::_OpenAssetForWrite(const ArResolvedPath& resolvedPath,
                                                                    WriteMode writeMode) const {
    const std::string& resolved = resolvedPath.GetPathString();
    Location location;
    std::string why;
    Endpoint target;
    if (!_Locate(resolved, &location, &why) || !_WriteTarget(location, &target, &why)) {
        TF_RUNTIME_ERROR("%s can't be written: %s", resolved.c_str(), why.c_str());
        return nullptr;
    }
    // What the server should still have: the version it was opened at, when it's the
    // endpoint being written to. Otherwise nothing: a file there now is someone else's.
    std::optional<Opened> was = opened(location.resolved);
    std::string condition;
    if (was && was->endpoint == target.cacheId()) condition = was->etag.empty() ? std::string("*") : was->etag;

    const std::filesystem::path file = writeFileFor(location.path);
    if (writeMode == WriteMode::Update) {
        // The file as it is, to change in place (a .usdc saved over itself).
        if (!was) {
            if (localFile(resolved)) was = opened(location.resolved);
        }
        std::error_code error;
        if (was) std::filesystem::copy_file(was->file, file, std::filesystem::copy_options::overwrite_existing, error);
        if (!was || error) std::ofstream(file, std::ios::binary | std::ios::trunc);
    }
    std::shared_ptr<ArWritableAsset> local = ArFilesystemWritableAsset::Create(ArResolvedPath(toUtf8(file)), writeMode);
    if (!local) {
        TF_RUNTIME_ERROR("%s can't be written: no file for it on this machine", location.display.c_str());
        return nullptr;
    }
    return std::make_shared<_WritableAsset>(this, std::move(location), std::move(target), std::move(condition), file,
                                            std::move(local));
}

bool CachingResolver::_CanWriteAssetToPath(const ArResolvedPath& resolvedPath, std::string* whyNot) const {
    Location location;
    std::string why;
    Endpoint target;
    if (_Locate(resolvedPath.GetPathString(), &location, &why) && _WriteTarget(location, &target, &why)) return true;
    if (whyNot) *whyNot = why;
    return false;
}

ArTimestamp CachingResolver::_GetModificationTimestamp(const std::string&, const ArResolvedPath& resolvedPath) const {
    Location location;
    const Found found = _Find(resolvedPath.GetPathString(), &location, false);
    if (found.status != Status::Ok) return ArTimestamp();
    return ArTimestamp(timestampOf(found));
}

ArAssetInfo CachingResolver::_GetAssetInfo(const std::string&, const ArResolvedPath& resolvedPath) const {
    ArAssetInfo info;
    Location location;
    const Found found = _Find(resolvedPath.GetPathString(), &location, false);
    if (found.status != Status::Ok) return info;
    info.version = found.entry.etag;
    // The path that names it (webdav://studio/a.usdz), not the server's own URL: Sdf anchors
    // the relative paths inside a package (a .usdz's textures) against a layer's repository
    // path, so it has to be one this resolver resolves again. The server's URL would have
    // sent them to plain HTTP.
    info.repoPath = location.display;
    // Which endpoint delivered it, for usdcc to show (§5).
    info.resolverInfo = VtValue(found.endpoint.id);
    return info;
}

bool CachingResolver::_IsContextDependentPath(const std::string& assetPath) const {
    Uri uri;
    return parseUri(assetPath, &uri) && uri.stackId.empty();
}

ArResolverContext CachingResolver::_CreateDefaultContext() const { return ArResolverContext(*defaultContext()); }

ArResolverContext CachingResolver::_CreateDefaultContextForAsset(const std::string&) const {
    return _CreateDefaultContext();
}

ArResolverContext CachingResolver::_CreateContextFromString(const std::string& contextStr) const {
    std::string environment = TfStringTrim(contextStr);
    if (TfStringStartsWith(environment, "env=")) environment = environment.substr(4);
    if (environment.empty()) return _CreateDefaultContext();
    return ArResolverContext(Context(defaultConfig(), environment));
}

void CachingResolver::_RefreshContext(const ArResolverContext& context) {
    const Context* refreshed = context.Get<Context>();
    if (!refreshed) return;
    // Also forgets what servers answered and which were down.
    reloadDefaultConfig();
    // Every context holding this one: their stages recompose what resolves differently.
    ArNotice::ResolverChanged(*refreshed).Send();
}

void CachingResolver::_BeginCacheScope(VtValue* cacheScopeData) { m_scopes.BeginCacheScope(cacheScopeData); }

void CachingResolver::_EndCacheScope(VtValue* cacheScopeData) { m_scopes.EndCacheScope(cacheScopeData); }

}  // namespace usdcc::assets
