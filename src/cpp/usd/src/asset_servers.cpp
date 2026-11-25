#include "usdcc/usd/asset_servers.h"

#include "usdcc/assets/context.h"
#include "usdcc/assets/no_wait.h"
#include "usdcc/assets/paths.h"

#include <pxr/base/js/json.h>
#include "usdcc/usd/schema_info.h"

#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/defaultResolverContext.h>
#include <pxr/usd/ar/packageUtils.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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

bool hasAssetEnvironment(const std::string& environment) {
    return usdcc::assets::defaultConfig().environments.count(environment) > 0;
}

usdcc::core::Signal<>& assetConfigChanged() {
    static usdcc::core::Signal<> signal;
    return signal;
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

namespace {

struct Folders {
    std::mutex mutex;
    std::string file;
    bool loaded = false;
    std::map<std::string, std::string> environments;  // folder -> environment
};

Folders& folders() {
    static Folders instance;
    return instance;
}

// With the lock held.
void loadFolders(Folders& all) {
    if (all.loaded || all.file.empty()) return;
    all.loaded = true;
    std::ifstream input(usdcc::assets::fromUtf8(all.file), std::ios::binary);
    if (!input) return;
    std::ostringstream text;
    text << input.rdbuf();
    const PXR_NS::JsValue root = PXR_NS::JsParseString(text.str());
    if (!root.IsObject()) return;
    for (const auto& [folder, value] : root.GetJsObject()) {
        if (value.IsString()) all.environments[folder] = value.GetString();
    }
}

std::string normalFolder(std::string folder) {
    std::replace(folder.begin(), folder.end(), '\\', '/');
    while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
#if defined(_WIN32)
    std::transform(folder.begin(), folder.end(), folder.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return folder;
}

}  // namespace

void setAssetFolderFile(const std::string& file) {
    Folders& all = folders();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.file = file;
    all.loaded = false;
    all.environments.clear();
}

std::string folderOf(const std::string& identifier) {
    const std::string path = displayAssetPath(identifier);
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

namespace {

// With the lock held, both.
void saveFolders(Folders& all) {
    PXR_NS::JsObject object;
    for (const auto& [each, chosen] : all.environments) object[each] = PXR_NS::JsValue(chosen);
    const std::filesystem::path file = usdcc::assets::fromUtf8(all.file);
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    PXR_NS::JsWriter writer(output, PXR_NS::JsWriter::Style::Pretty);
    PXR_NS::JsWriteValue(&writer, PXR_NS::JsValue(object));
}

// A project's own choice wins over its parent's; one that isn't configured is passed over.
std::map<std::string, std::string>::iterator nearestFolder(Folders& all, const std::string& identifier) {
    loadFolders(all);
    std::string folder = normalFolder(folderOf(identifier));
    while (!folder.empty()) {
        const auto it = all.environments.find(folder);
        if (it != all.environments.end() && hasAssetEnvironment(it->second)) return it;
        const size_t slash = folder.find_last_of('/');
        if (slash == std::string::npos || slash == 0) break;
        folder.erase(slash);
    }
    return all.environments.end();
}

}  // namespace

void rememberFolderEnvironment(const std::string& folder, const std::string& environment) {
    Folders& all = folders();
    std::lock_guard<std::mutex> lock(all.mutex);
    if (all.file.empty() || folder.empty()) return;
    loadFolders(all);
    all.environments[normalFolder(folder)] = environment;
    saveFolders(all);
}

std::string rememberedEnvironment(const std::string& identifier) {
    Folders& all = folders();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = nearestFolder(all, identifier);
    return found == all.environments.end() ? std::string() : found->second;
}

bool forgetFolderEnvironment(const std::string& identifier) {
    Folders& all = folders();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = nearestFolder(all, identifier);
    if (found == all.environments.end()) return false;
    all.environments.erase(found);
    saveFolders(all);
    return true;
}

PXR_NS::ArResolverContext stageResolverContext(const std::string& identifier, const std::string& environment) {
    std::string chosen = environment.empty() && !identifier.empty() ? rememberedEnvironment(identifier) : environment;
    if (chosen.empty()) chosen = defaultAssetEnvironment();
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
    // USD anchors a relative path in a package (.usdz) inside the package: only the whole
    // path reaches a file outside it, as properties::layerFolder() has it on disk.
    if (PXR_NS::ArIsPackageRelativePath(anchor)) return {};
    const PXR_NS::SdfFileFormatConstPtr format = PXR_NS::SdfFileFormat::FindByExtension(anchor);
    if (format && format->IsPackage()) return {};
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

LocatedAsset locateAsset(const std::string& anchored) {
    const usdcc::assets::NoWait noWait;
    const std::size_t unanswered = usdcc::assets::NoWait::unanswered();
    LocatedAsset located;
    std::string raw;
    if (const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(anchored)) {
        raw = open->GetRealPath().empty() ? open->GetIdentifier() : open->GetRealPath();
    } else {
        raw = PXR_NS::ArGetResolver().Resolve(anchored).GetPathString();
        located.resolving = raw.empty() && usdcc::assets::NoWait::unanswered() != unanswered;
    }
    // A server's path as it's written, without the stack id only this process knows.
    located.path = displayAssetPath(raw);
    located.origin = assetOrigin(anchored, raw);
    return located;
}

std::string assetOrigin(const std::string& assetPath, const std::string& resolvedPath) {
    if (resolvedPath.empty() || isLocalAssetPath(resolvedPath)) return {};
    const PXR_NS::ArAssetInfo info = PXR_NS::ArGetResolver().GetAssetInfo(assetPath, PXR_NS::ArResolvedPath(resolvedPath));
    return info.resolverInfo.IsHolding<std::string>() ? info.resolverInfo.UncheckedGet<std::string>() : std::string();
}

}  // namespace usdcc::usd
