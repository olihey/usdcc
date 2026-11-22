#include "usdcc/assets/transport.h"

#include "usdcc/assets/uri.h"

#include <pxr/base/js/value.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/resolver.h>

#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace usdcc::assets {

namespace {

struct Registry {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Transport>> transports;
    std::map<std::string, AddressParser> addressParsers;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

}  // namespace

const char* statusName(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::NotModified: return "not modified";
        case Status::Missing: return "missing";
        case Status::Unreachable: return "unreachable";
        case Status::NotAllowed: return "not allowed";
        case Status::NotSupported: return "not supported";
        case Status::Conflict: return "conflict";
        case Status::Cancelled: return "cancelled";
        case Status::Error: return "error";
    }
    return "error";
}

std::string Endpoint::cacheId() const { return stableHash(scheme + "|" + url + "|" + credential, 12); }

std::string Entry::versionKey() const {
    if (!etag.empty()) return stableHash("etag|" + etag, 8);
    std::ostringstream text;
    text.precision(17);
    text << "time|" << modified << "|" << size;
    return stableHash(text.str(), 8);
}

Transport::~Transport() = default;

Result<std::vector<Entry>> Transport::list(const Endpoint&, const std::string&, Cancel&) {
    return Result<std::vector<Entry>>::failure(Status::NotSupported, "this protocol can't list folders");
}

Result<EntryPage> Transport::listPage(const Endpoint& endpoint, const std::string& folder, const std::string& from,
                                     Cancel& cancel) {
    if (!from.empty()) return Result<EntryPage>::success({});
    Result<std::vector<Entry>> all = list(endpoint, folder, cancel);
    if (!all.ok()) return Result<EntryPage>::failure(all.status, all.message);
    EntryPage page;
    page.entries = std::move(all.value);
    return Result<EntryPage>::success(std::move(page));
}

Result<Entry> Transport::put(const Endpoint&, const std::string&, const std::filesystem::path&, const std::string&,
                             Progress&, Cancel&) {
    return Result<Entry>::failure(Status::NotSupported, "this protocol can't write");
}

void registerTransport(const std::string& scheme, std::shared_ptr<Transport> transport) {
    Registry& all = registry();
    std::lock_guard<std::mutex> lock(all.mutex);
    if (transport) {
        all.transports[scheme] = std::move(transport);
    } else {
        all.transports.erase(scheme);
    }
}

std::shared_ptr<Transport> transportFor(const std::string& scheme) {
    Registry& all = registry();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = all.transports.find(scheme);
    return found == all.transports.end() ? nullptr : found->second;
}

std::vector<std::string> transportSchemes() {
    Registry& all = registry();
    std::lock_guard<std::mutex> lock(all.mutex);
    std::vector<std::string> schemes;
    for (const auto& [scheme, transport] : all.transports) schemes.push_back(scheme);
    return schemes;
}

void registerAddressParser(const std::string& scheme, AddressParser parser) {
    Registry& all = registry();
    std::lock_guard<std::mutex> lock(all.mutex);
    if (parser) {
        all.addressParsers[scheme] = std::move(parser);
    } else {
        all.addressParsers.erase(scheme);
    }
}

AddressParser addressParserFor(const std::string& scheme) {
    Registry& all = registry();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = all.addressParsers.find(scheme);
    return found == all.addressParsers.end() ? AddressParser() : found->second;
}

void loadProtocolPlugins() {
    PXR_NAMESPACE_USING_DIRECTIVE
    std::set<TfType> types;
    PlugRegistry::GetAllDerivedTypes(TfType::Find<ArResolver>(), &types);
    for (const TfType& type : types) {
        const JsValue marked = PlugRegistry::GetInstance().GetDataFromPluginMetaData(type, "usdccAssetsProtocol");
        if (!marked.IsBool() || !marked.GetBool()) continue;
        if (const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginForType(type)) plugin->Load();
    }
}

}  // namespace usdcc::assets
