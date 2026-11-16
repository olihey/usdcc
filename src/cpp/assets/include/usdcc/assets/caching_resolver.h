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

#include "usdcc/assets/context.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/uri.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/threadLocalScopedCache.h>

#include <memory>
#include <string>

namespace usdcc::assets {

class CachingResolver : public PXR_NS::ArResolver {
public:
    explicit CachingResolver(std::string scheme);
    ~CachingResolver() override;

    const std::string& scheme() const { return m_scheme; }

protected:
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

    void _BeginCacheScope(PXR_NS::VtValue* cacheScopeData) override;
    void _EndCacheScope(PXR_NS::VtValue* cacheScopeData) override;

private:
    // The stack a path is looked for in: the one its id names, or else the one the
    // bound context (or the default one) has for its name. Null with a warning when
    // there is none.
    std::shared_ptr<const Stack> _StackFor(const Uri& uri) const;
    Found _Find(const Uri& uri, std::shared_ptr<const Stack>* stack = nullptr) const;
    Lookups* _Scope() const;

    std::string m_scheme;
    mutable PXR_NS::ArThreadLocalScopedCache<Lookups> m_scopes;
};

}  // namespace usdcc::assets
