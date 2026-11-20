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
        if (!listed.ok()) throwFailure(uri, context, listed.status, listed.message);
        return entriesOf(listed.value);
    }

    AssetListPage listPage(const std::string& uri, const PXR_NS::ArResolverContext& context, usdcc::assets::Cancel& cancel,
                           const std::string& from) override {
        const usdcc::assets::Result<usdcc::assets::ListedPage> listed = usdcc::assets::listUriPage(uri, contextOf(context), from);
        if (cancel.cancelled()) return {};
        if (!listed.ok()) throwFailure(uri, context, listed.status, listed.message);
        AssetListPage page;
        page.entries = entriesOf(listed.value.entries);
        page.next = listed.value.next;
        return page;
    }

private:
    [[noreturn]] static void throwFailure(const std::string& uri, const PXR_NS::ArResolverContext& context,
                                          usdcc::assets::Status status, const std::string& why) {
        // Missing with no more to say: the picker's words for it.
        if (status == usdcc::assets::Status::Missing && why.empty()) throw std::runtime_error("This folder isn't there");
        std::string message = usdcc::assets::statusName(status);
        if (!why.empty()) message += ": " + why;
        if (status == usdcc::assets::Status::NotAllowed) {
            const std::string credential = refusedCredential(uri, contextOf(context), why);
            if (!credential.empty()) throw AssetLoginNeeded(message, credential, schemeOf(uri));
        }
        throw std::runtime_error(message);
    }

    static std::vector<AssetListEntry> entriesOf(const std::vector<usdcc::assets::ListedEntry>& listed) {
        // An origin only says something when a name has more than one server.
        std::set<std::string> origins;
        for (const usdcc::assets::ListedEntry& each : listed) origins.insert(each.origin);
        std::vector<AssetListEntry> entries;
        for (const usdcc::assets::ListedEntry& each : listed) {
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

    static std::string schemeOf(const std::string& uri) {
        const size_t colon = uri.find("://");
        return colon == std::string::npos ? std::string() : uri.substr(0, colon);
    }

    // The credential id of the server that refused listing `uri`: the stack's endpoint the
    // message names ("test: HTTP 401 ..."), or the endpoint an address means. Empty when
    // no login can be saved for it: none named, or AWS's own (docs/S3_RESOLVER.md §4.2).
    static std::string refusedCredential(const std::string& uri, const usdcc::assets::Context& context,
                                         const std::string& message) {
        usdcc::assets::Endpoint refused;
        usdcc::assets::Uri named;
        const std::shared_ptr<const usdcc::assets::Stack> stack =
            usdcc::assets::parseUri(uri, &named) && named.stackId.empty() ? context.stack(named.scheme, named.name) : nullptr;
        if (stack) {
            for (const usdcc::assets::Endpoint& endpoint : stack->endpoints) {
                if (message.rfind(endpoint.id + ": ", 0) == 0) refused = endpoint;
            }
        } else if (const usdcc::assets::AddressParser parser = usdcc::assets::addressParserFor(schemeOf(uri))) {
            std::string path;
            std::string normalized;
            if (!parser(uri, &refused, &path, &normalized)) return {};
        }
        const std::string& id = refused.credential;
        if (id.empty() || id == "aws" || id.rfind("aws:", 0) == 0) return {};
        return id;
    }

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
