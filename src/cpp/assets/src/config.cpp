#include "usdcc/assets/config.h"

#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/getenv.h>

#include <fstream>
#include <sstream>

namespace usdcc::assets {

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

std::string textOf(const JsObject& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->second.IsString() ? found->second.GetString() : std::string();
}

bool flagOf(const JsObject& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->second.IsBool() && found->second.GetBool();
}

const JsObject* objectOf(const JsObject& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->second.IsObject() ? &found->second.GetJsObject() : nullptr;
}

}  // namespace

Result<Config> Config::parse(const std::string& json) {
    JsParseError error;
    const JsValue root = JsParseString(json, &error);
    if (!root.IsObject()) {
        return Result<Config>::failure(
            Status::Error, error.reason.empty() ? "it isn't a JSON object"
                                                : "line " + std::to_string(error.line) + ": " + error.reason);
    }
    const JsObject& top = root.GetJsObject();
    std::vector<std::string> problems;
    Config config;
    config.environment = textOf(top, "environment");

    config.offline = flagOf(top, "offline");
    const auto limit = top.find("cacheLimitGB");
    if (limit != top.end() && (limit->second.IsReal() || limit->second.IsInt())) config.cacheLimitGB = limit->second.GetReal();
    if (const JsObject* http = objectOf(top, "http")) {
        if (const JsObject* hosts = objectOf(*http, "hosts")) {
            for (const auto& [host, value] : *hosts) {
                if (!value.IsObject()) continue;
                HttpHost entry;
                entry.credential = textOf(value.GetJsObject(), "credential");
                entry.insecure = flagOf(value.GetJsObject(), "insecure");
                config.httpHosts[host] = entry;
            }
        }
        const auto allow = http->find("allow");
        if (allow != http->end() && allow->second.IsArray()) {
            for (const JsValue& pattern : allow->second.GetJsArray()) {
                if (pattern.IsString()) config.httpAllow.push_back(pattern.GetString());
            }
        }
    }

    if (const JsObject* endpoints = objectOf(top, "endpoints")) {
        for (const auto& [id, value] : *endpoints) {
            if (!value.IsObject()) {
                problems.push_back("endpoint '" + id + "' isn't an object");
                continue;
            }
            const JsObject& object = value.GetJsObject();
            Endpoint endpoint;
            endpoint.id = id;
            endpoint.scheme = textOf(object, "scheme");
            endpoint.url = textOf(object, "url");
            endpoint.credential = textOf(object, "credential");
            endpoint.readOnly = flagOf(object, "readOnly");
            endpoint.optional = flagOf(object, "optional");
            const auto mirrors = object.find("mirrors");
            if (mirrors != object.end() && mirrors->second.IsArray()) {
                for (const JsValue& mirror : mirrors->second.GetJsArray()) {
                    if (mirror.IsString()) endpoint.mirrors.push_back(mirror.GetString());
                }
            }
            // One spelling of a URL, so that the same server is the same cache folder.
            while (endpoint.url.size() > 1 && endpoint.url.back() == '/') endpoint.url.pop_back();
            if (endpoint.scheme.empty()) problems.push_back("endpoint '" + id + "' has no \"scheme\"");
            if (endpoint.url.empty()) problems.push_back("endpoint '" + id + "' has no \"url\"");
            config.endpoints[id] = std::move(endpoint);
        }
    }

    if (const JsObject* environments = objectOf(top, "environments")) {
        for (const auto& [environment, schemes] : *environments) {
            if (!schemes.IsObject()) {
                problems.push_back("environment '" + environment + "' isn't an object");
                continue;
            }
            auto& ofEnvironment = config.environments[environment];
            for (const auto& [scheme, names] : schemes.GetJsObject()) {
                if (!names.IsObject()) {
                    problems.push_back("environment '" + environment + "': '" + scheme + "' isn't an object");
                    continue;
                }
                for (const auto& [name, stack] : names.GetJsObject()) {
                    const std::string where = "environment '" + environment + "', " + scheme + "://" + name;
                    if (!isValidName(name)) {
                        problems.push_back(where + ": a name is lowercase letters, digits, '-', '_' and '.'");
                        continue;
                    }
                    if (!stack.IsArray() || stack.GetJsArray().empty()) {
                        problems.push_back(where + ": its stack is a list of endpoint ids, top first");
                        continue;
                    }
                    std::vector<std::string> ids;
                    for (const JsValue& id : stack.GetJsArray()) {
                        const std::string endpoint = id.IsString() ? id.GetString() : std::string();
                        const auto found = config.endpoints.find(endpoint);
                        if (found == config.endpoints.end()) {
                            problems.push_back(where + ": there is no endpoint '" + endpoint + "'");
                        } else if (found->second.scheme != scheme) {
                            problems.push_back(where + ": endpoint '" + endpoint + "' is " + found->second.scheme +
                                               ", and a stack holds one protocol");
                        } else {
                            ids.push_back(endpoint);
                        }
                    }
                    ofEnvironment[scheme][name] = std::move(ids);
                }
            }
        }
    }

    if (!config.environment.empty() && !config.environments.count(config.environment)) {
        problems.push_back("there is no environment '" + config.environment + "'");
    }
    if (!problems.empty()) {
        std::string message;
        for (const std::string& problem : problems) message += (message.empty() ? "" : "\n") + problem;
        return Result<Config>::failure(Status::Error, message);
    }
    return Result<Config>::success(std::move(config));
}

Result<Config> Config::load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return Result<Config>::failure(Status::Missing, "can't read " + toUtf8(file));
    std::ostringstream text;
    text << input.rdbuf();
    return parse(text.str());
}

std::filesystem::path Config::defaultPath() {
    const std::string named = TfGetenv("USDCC_ASSETS_CONFIG");
    if (!named.empty()) return fromUtf8(named);
    return userDataFolder() / "assets.json";
}

Config Config::loadDefault() {
    const std::filesystem::path file = defaultPath();
    std::error_code error;
    if (!std::filesystem::exists(file, error)) return Config();
    Result<Config> loaded = load(file);
    if (!loaded.ok()) {
        TF_WARN("The asset servers in %s are ignored:\n%s", toUtf8(file).c_str(), loaded.message.c_str());
        return Config();
    }
    return std::move(loaded.value);
}

std::string Config::chosenEnvironment() const {
    const std::string named = TfGetenv("USDCC_ASSET_ENV");
    return named.empty() ? environment : named;
}

std::filesystem::path userDataFolder() {
#if defined(_WIN32)
    const std::string local = TfGetenv("LOCALAPPDATA");
    // Not %LOCALAPPDATA%\usdcc: the installer puts the app there (docs/UPDATES.md).
    return fromUtf8(local.empty() ? std::string(".") : local) / "usdcc_config";
#elif defined(__APPLE__)
    return fromUtf8(TfGetenv("HOME", ".")) / "Library" / "Application Support" / "usdcc";
#else
    const std::string data = TfGetenv("XDG_DATA_HOME");
    if (!data.empty()) return fromUtf8(data) / "usdcc";
    return fromUtf8(TfGetenv("HOME", ".")) / ".local" / "share" / "usdcc";
#endif
}

}  // namespace usdcc::assets
