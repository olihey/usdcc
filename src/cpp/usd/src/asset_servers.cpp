#include "usdcc/usd/asset_servers.h"

#include "usdcc/assets/context.h"

#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>

#include <algorithm>
#include <mutex>

namespace usdcc::usd {

namespace {

std::mutex& preferredMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string& preferred() {
    static std::string environment;
    return environment;
}

}  // namespace

std::vector<std::string> assetEnvironments() {
    std::vector<std::string> names;
    for (const auto& [name, schemes] : usdcc::assets::defaultConfig().environments) names.push_back(name);
    return names;
}

std::string defaultAssetEnvironment() {
    {
        std::lock_guard<std::mutex> lock(preferredMutex());
        if (!preferred().empty()) return preferred();
    }
    return usdcc::assets::defaultContext()->environment();
}

void setPreferredAssetEnvironment(const std::string& environment) {
    std::lock_guard<std::mutex> lock(preferredMutex());
    preferred() = environment;
}

PXR_NS::ArResolverContext stageResolverContext(const std::string& identifier, const std::string& environment) {
    const std::string chosen = environment.empty() ? defaultAssetEnvironment() : environment;
    // Ours first: a context holds one object per type, and the first given wins over the
    // default one each remote protocol adds.
    std::vector<PXR_NS::ArResolverContext> contexts;
    contexts.emplace_back(usdcc::assets::Context(usdcc::assets::defaultConfig(), chosen));
    contexts.push_back(PXR_NS::ArGetResolver().CreateDefaultContextForAsset(identifier));
    return PXR_NS::ArResolverContext(contexts);
}

std::string stageAssetEnvironment(const PXR_NS::UsdStageRefPtr& stage) {
    if (!stage) return {};
    const usdcc::assets::Context* context = stage->GetPathResolverContext().Get<usdcc::assets::Context>();
    return context ? context->environment() : std::string();
}

bool isLocalAssetPath(const std::string& path) {
    const size_t scheme = path.find("://");
    if (scheme == std::string::npos) return true;
    // "C://x" isn't written, but a drive letter is no scheme.
    if (scheme <= 1) return true;
    // Inside a package, the package decides: "a.usdz[...]" on disk is local.
    const size_t bracket = path.find('[');
    return bracket != std::string::npos && bracket < scheme;
}

std::string displayAssetPath(const std::string& resolvedPath) {
    const size_t scheme = resolvedPath.find("://");
    if (scheme == std::string::npos) return resolvedPath;
    const size_t authority = scheme + 3;
    const size_t slash = resolvedPath.find('/', authority);
    const size_t tilde = resolvedPath.find('~', authority);
    if (tilde == std::string::npos || (slash != std::string::npos && tilde > slash)) return resolvedPath;
    return resolvedPath.substr(0, tilde) + (slash == std::string::npos ? std::string() : resolvedPath.substr(slash));
}

std::string assetOrigin(const std::string& assetPath, const std::string& resolvedPath) {
    if (resolvedPath.empty() || isLocalAssetPath(resolvedPath)) return {};
    const PXR_NS::ArAssetInfo info = PXR_NS::ArGetResolver().GetAssetInfo(assetPath, PXR_NS::ArResolvedPath(resolvedPath));
    return info.resolverInfo.IsHolding<std::string>() ? info.resolverInfo.UncheckedGet<std::string>() : std::string();
}

}  // namespace usdcc::usd
