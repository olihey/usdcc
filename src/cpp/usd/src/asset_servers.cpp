#include "usdcc/usd/asset_servers.h"

#include "usdcc/assets/context.h"
#include "usdcc/usd/schema_info.h"

#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/defaultResolverContext.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>

#include <algorithm>
#include <cctype>
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
    // A plugin's schema.usda sublayers others by plugin (@usdGeom/schema.usda@),
    // which usdGenSchema finds in the plugins' resources (docs/SCHEMAS.md §4).
    if (const std::vector<std::string> schemaPaths = schemaSourceSearchPaths(identifier); !schemaPaths.empty()) {
        contexts.emplace_back(PXR_NS::ArDefaultResolverContext(schemaPaths));
    }
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

std::string relativeAssetPath(const std::string& target, const std::string& anchor) {
    if (isLocalAssetPath(target) || isLocalAssetPath(anchor)) return {};
    const std::string to = displayAssetPath(target);
    const std::string from = displayAssetPath(anchor);
    const auto split = [](const std::string& uri, std::string* server, std::vector<std::string>* parts) {
        const size_t start = uri.find("://") + 3;
        const size_t slash = uri.find('/', start);
        *server = uri.substr(0, slash);
        std::string lowered = *server;
        for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        *server = lowered;
        parts->clear();
        if (slash == std::string::npos) return;
        std::string part;
        for (const char c : uri.substr(slash + 1)) {
            if (c == '/') {
                parts->push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        parts->push_back(part);  // the file, or "" after a trailing slash
    };
    std::string toServer;
    std::string fromServer;
    std::vector<std::string> toParts;
    std::vector<std::string> fromParts;
    split(to, &toServer, &toParts);
    split(from, &fromServer, &fromParts);
    if (toServer != fromServer || toParts.empty() || fromParts.empty()) return {};
    fromParts.pop_back();  // the anchor's folder
    size_t common = 0;
    while (common < fromParts.size() && common + 1 < toParts.size() && fromParts[common] == toParts[common]) ++common;
    std::string relative;
    for (size_t i = common; i < fromParts.size(); ++i) relative += "../";
    if (relative.empty()) relative = "./";
    for (size_t i = common; i < toParts.size(); ++i) relative += toParts[i] + (i + 1 < toParts.size() ? "/" : "");
    return relative;
}

std::string assetOrigin(const std::string& assetPath, const std::string& resolvedPath) {
    if (resolvedPath.empty() || isLocalAssetPath(resolvedPath)) return {};
    const PXR_NS::ArAssetInfo info = PXR_NS::ArGetResolver().GetAssetInfo(assetPath, PXR_NS::ArResolvedPath(resolvedPath));
    return info.resolverInfo.IsHolding<std::string>() ? info.resolverInfo.UncheckedGet<std::string>() : std::string();
}

}  // namespace usdcc::usd
