#include "usdcc/usd/asset_sources.h"

#include "usdcc/assets/context.h"
#include "usdcc/assets/lookup.h"

#include <pxr/base/js/value.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/resolver.h>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace usdcc::usd {

namespace {

// usdcc's own protocols that can list (docs/REMOTE_ASSETS.md §6): the names of the
// stage's environment as roots, and their folders through the transports.
class RemoteAssetsSource final : public AssetSource {
public:
    std::string id() const override { return "usdcc.remote-assets"; }
    std::string label() const override { return "Servers"; }

    std::vector<std::string> schemes() const override {
        // A protocol's library registers its transport when it loads, which OpenUSD only
        // does once something resolves through it: so load them first.
        usdcc::assets::loadProtocolPlugins();
        std::vector<std::string> schemes;
        for (const std::string& scheme : usdcc::assets::transportSchemes()) {
            const std::shared_ptr<usdcc::assets::Transport> transport = usdcc::assets::transportFor(scheme);
            if (transport && transport->canList()) schemes.push_back(scheme);
        }
        return schemes;
    }

    std::vector<AssetRoot> roots(const std::string& scheme, const PXR_NS::ArResolverContext& context) const override {
        std::vector<AssetRoot> roots;
        const usdcc::assets::Context assets = contextOf(context);
        for (const std::string& name : assets.names(scheme)) roots.push_back({name, scheme + "://" + name + "/"});
        return roots;
    }

    std::vector<AssetListEntry> list(const std::string& uri, const PXR_NS::ArResolverContext& context,
                                     usdcc::assets::Cancel& cancel) override {
        const usdcc::assets::Result<std::vector<usdcc::assets::ListedEntry>> listed =
            usdcc::assets::listUri(uri, contextOf(context));
        if (cancel.cancelled()) return {};
        if (!listed.ok()) {
            std::string message = usdcc::assets::statusName(listed.status);
            if (!listed.message.empty()) message += ": " + listed.message;
            throw std::runtime_error(message);
        }
        // An origin only says something when a name has more than one server.
        std::set<std::string> origins;
        for (const usdcc::assets::ListedEntry& each : listed.value) origins.insert(each.origin);
        std::vector<AssetListEntry> entries;
        for (const usdcc::assets::ListedEntry& each : listed.value) {
            AssetListEntry entry;
            entry.name = each.entry.name;
            entry.isFolder = each.entry.isFolder;
            entry.size = each.entry.size;
            entry.modified = each.entry.modified;
            if (origins.size() > 1) entry.origin = each.origin;
            entries.push_back(std::move(entry));
        }
        return entries;
    }

private:
    // The stage's environment, or else the default one.
    static usdcc::assets::Context contextOf(const PXR_NS::ArResolverContext& context) {
        if (const usdcc::assets::Context* own = context.Get<usdcc::assets::Context>()) return *own;
        return *usdcc::assets::defaultContext();
    }
};

}  // namespace

AssetSource::~AssetSource() = default;

AssetSourceRegistry& AssetSourceRegistry::instance() {
    static AssetSourceRegistry* registry = []() {
        auto* made = new AssetSourceRegistry();
        made->add(std::make_shared<RemoteAssetsSource>());
        return made;
    }();
    return *registry;
}

void AssetSourceRegistry::add(std::shared_ptr<AssetSource> source) {
    if (!source) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string id = source->id();
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(),
                                   [&id](const std::shared_ptr<AssetSource>& each) { return each->id() == id; }),
                    m_sources.end());
    m_sources.push_back(std::move(source));
}

void AssetSourceRegistry::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(),
                                   [&id](const std::shared_ptr<AssetSource>& each) { return each->id() == id; }),
                    m_sources.end());
}

std::vector<std::shared_ptr<AssetSource>> AssetSourceRegistry::sources() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sources;
}

std::shared_ptr<AssetSource> AssetSourceRegistry::forScheme(const std::string& scheme) const {
    for (const std::shared_ptr<AssetSource>& source : sources()) {
        const std::vector<std::string> schemes = source->schemes();
        if (std::find(schemes.begin(), schemes.end(), scheme) != schemes.end()) return source;
    }
    return nullptr;
}

std::vector<std::string> AssetSourceRegistry::browsableSchemes() const {
    std::set<std::string> schemes;
    for (const std::shared_ptr<AssetSource>& source : sources()) {
        for (const std::string& scheme : source->schemes()) schemes.insert(scheme);
    }
    return {schemes.begin(), schemes.end()};
}

std::vector<std::string> registeredUriSchemes() {
    std::vector<std::string> schemes = PXR_NS::ArGetRegisteredURISchemes();
    std::sort(schemes.begin(), schemes.end());
    return schemes;
}

std::string resolverTypeForScheme(const std::string& scheme) {
    PXR_NAMESPACE_USING_DIRECTIVE
    std::set<TfType> types;
    PlugRegistry::GetAllDerivedTypes(TfType::Find<ArResolver>(), &types);
    for (const TfType& type : types) {
        const JsValue schemes = PlugRegistry::GetInstance().GetDataFromPluginMetaData(type, "uriSchemes");
        if (!schemes.IsArray()) continue;
        for (const JsValue& each : schemes.GetJsArray()) {
            if (each.IsString() && each.GetString() == scheme) return type.GetTypeName();
        }
    }
    return {};
}

}  // namespace usdcc::usd
