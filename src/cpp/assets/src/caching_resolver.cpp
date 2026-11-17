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
#include <pxr/usd/ar/notice.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/timestamp.h>

#include <cmath>
#include <cstdint>
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

// Which file each resolved path last opened as, for localFile().
struct OpenedFiles {
    std::mutex mutex;
    std::map<std::string, std::filesystem::path> files;
};

OpenedFiles& openedFiles() {
    static OpenedFiles instance;
    return instance;
}

std::shared_ptr<ArAsset> openRemembered(const std::string& resolved, const std::filesystem::path& file) {
    {
        OpenedFiles& all = openedFiles();
        std::lock_guard<std::mutex> lock(all.mutex);
        all.files[resolved] = file;
    }
    return ArFilesystemAsset::Open(ArResolvedPath(toUtf8(file)));
}

}  // namespace

std::optional<std::filesystem::path> localFile(const std::string& resolvedPath) {
    if (!ArGetResolver().OpenAsset(ArResolvedPath(resolvedPath))) return std::nullopt;
    OpenedFiles& all = openedFiles();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = all.files.find(resolvedPath);
    if (found == all.files.end()) return std::nullopt;
    return found->second;
}

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

ArResolvedPath CachingResolver::_ResolveForNewAsset(const std::string&) const {
    // Writing is a later phase.
    return ArResolvedPath();
}

std::shared_ptr<ArAsset> CachingResolver::_OpenAsset(const ArResolvedPath& resolvedPath) const {
    Location location;
    const Found found = _Find(resolvedPath.GetPathString(), &location, true);
    if (found.status != Status::Ok) return nullptr;
    // Offline, it resolved to a version in the cache, and that is the one to open.
    if (!found.cachedFile.empty()) return openRemembered(resolvedPath.GetPathString(), found.cachedFile);
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
    return openRemembered(resolvedPath.GetPathString(), file.value);
}

std::shared_ptr<ArWritableAsset> CachingResolver::_OpenAssetForWrite(const ArResolvedPath&, WriteMode) const {
    return nullptr;
}

bool CachingResolver::_CanWriteAssetToPath(const ArResolvedPath&, std::string* whyNot) const {
    if (whyNot) *whyNot = "Assets on a server are read-only in this version.";
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
