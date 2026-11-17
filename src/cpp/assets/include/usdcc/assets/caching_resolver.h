#pragma once

// The resolver base class of every protocol (docs/REMOTE_ASSETS.md §5, §6). A
// protocol's own resolver derives from it, names its scheme, and registers a
// Transport for that scheme; everything else is here.
//
//     class WebDavResolver : public usdcc::assets::CachingResolver {
//     public:
//         WebDavResolver() : CachingResolver("webdav") {}
//     };
//     AR_DEFINE_RESOLVER(WebDavResolver, ArResolver);
//
// That is a named protocol: webdav://<name>/<path>, the name's stack coming from the
// context. A direct protocol (§5.1), whose URLs are real addresses, overrides
// _Locate(), _CreateIdentifier() and _IsContextDependentPath().

#include "usdcc/assets/context.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/uri.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/threadLocalScopedCache.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace usdcc::assets {

// The file on disk a resolved path on a server was downloaded to, downloading it first
// when it isn't: for what needs a real file (an app opening a texture, a renderer
// reading one itself). Nothing for a path that doesn't open (docs/REMOTE_ASSETS.md §10).
std::optional<std::filesystem::path> localFile(const std::string& resolvedPath);

class CachingResolver : public PXR_NS::ArResolver {
public:
    explicit CachingResolver(std::string scheme);
    ~CachingResolver() override;

    const std::string& scheme() const { return m_scheme; }

protected:
    // What an asset path means to the transports.
    struct Location {
        std::shared_ptr<const Stack> stack;  // where to look, top first
        std::string path;                    // relative to an endpoint's URL, starting with '/'
        std::string resolved;                // what Resolve() returns when the file is found
        std::string display;                 // the path as messages name it
    };

    // False, with `why`, for a path this protocol can't look for. The default is a named
    // protocol's: the stack its id names, or else the one the bound context (or the
    // default one) has for its name.
    virtual bool _Locate(const std::string& assetPath, Location* location, std::string* why) const;

    std::string _CreateIdentifier(const std::string& assetPath,
                                  const PXR_NS::ArResolvedPath& anchorAssetPath) const override;
    std::string _CreateIdentifierForNewAsset(const std::string& assetPath,
                                             const PXR_NS::ArResolvedPath& anchorAssetPath) const override;
    PXR_NS::ArResolvedPath _Resolve(const std::string& assetPath) const override;
    PXR_NS::ArResolvedPath _ResolveForNewAsset(const std::string& assetPath) const override;
    std::shared_ptr<PXR_NS::ArAsset> _OpenAsset(const PXR_NS::ArResolvedPath& resolvedPath) const override;
    std::shared_ptr<PXR_NS::ArWritableAsset> _OpenAssetForWrite(const PXR_NS::ArResolvedPath& resolvedPath,
                                                               WriteMode writeMode) const override;
    bool _CanWriteAssetToPath(const PXR_NS::ArResolvedPath& resolvedPath, std::string* whyNot) const override;
    PXR_NS::ArTimestamp _GetModificationTimestamp(const std::string& assetPath,
                                                  const PXR_NS::ArResolvedPath& resolvedPath) const override;
    PXR_NS::ArAssetInfo _GetAssetInfo(const std::string& assetPath,
                                      const PXR_NS::ArResolvedPath& resolvedPath) const override;
    bool _IsContextDependentPath(const std::string& assetPath) const override;

    PXR_NS::ArResolverContext _CreateDefaultContext() const override;
    PXR_NS::ArResolverContext _CreateDefaultContextForAsset(const std::string& assetPath) const override;
    // "test" or "env=test": that environment of the default configuration.
    PXR_NS::ArResolverContext _CreateContextFromString(const std::string& contextStr) const override;
    // Reads the configuration again and forgets what servers answered, then tells
    // OpenUSD that everything resolved with the context may have changed.
    void _RefreshContext(const PXR_NS::ArResolverContext& context) override;

    void _BeginCacheScope(PXR_NS::VtValue* cacheScopeData) override;
    void _EndCacheScope(PXR_NS::VtValue* cacheScopeData) override;

private:
    // Where the path is, with a warning when a server couldn't be asked. `location` is
    // filled whenever the path could be located at all.
    Found _Find(const std::string& assetPath, Location* location, bool warnIfUnlocated) const;
    Lookups* _Scope() const;

    std::string m_scheme;
    mutable PXR_NS::ArThreadLocalScopedCache<Lookups> m_scopes;
};

}  // namespace usdcc::assets
