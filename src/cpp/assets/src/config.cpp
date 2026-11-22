#include "usdcc/assets/config.h"

#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/getenv.h>

#include <algorithm>
#include <fstream>
#include <iterator>
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

// A value of a key the library doesn't know, as text: what Endpoint::options holds.
std::string optionText(const JsValue& value) {
    if (value.IsString()) return value.GetString();
    if (value.IsBool()) return value.GetBool() ? "true" : "false";
    if (value.IsInt()) return std::to_string(value.GetInt64());
    if (value.IsReal()) return std::to_string(value.GetReal());
    return std::string();
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
    config.source = json;
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
    if (const JsObject* s3 = objectOf(top, "s3")) {
        const auto allow = s3->find("allow");
        if (allow != s3->end() && allow->second.IsArray()) {
            for (const JsValue& pattern : allow->second.GetJsArray()) {
                if (pattern.IsString()) config.s3Allow.push_back(pattern.GetString());
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
            for (const auto& [key, value] : object) {
                static const char* kKnown[] = {"scheme", "url", "credential", "readOnly", "optional", "mirrors"};
                if (std::find(std::begin(kKnown), std::end(kKnown), key) != std::end(kKnown)) continue;
                const std::string text = optionText(value);
                if (!text.empty()) endpoint.options[key] = text;
            }
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

namespace {

// The object at `key` of `object`, or an empty one: where the keys a writer doesn't know live.
JsObject objectAt(const JsObject& object, const char* key) {
    const JsObject* found = objectOf(object, key);
    return found ? *found : JsObject();
}

// An endpoint's option as the file had it when its text is unchanged, else as text, a flag
// spelled "true" or "false" as a JSON bool.
JsValue optionValue(const JsObject* before, const std::string& key, const std::string& text) {
    if (before) {
        const auto found = before->find(key);
        if (found != before->end() && optionText(found->second) == text) return found->second;
    }
    if (text == "true") return JsValue(true);
    if (text == "false") return JsValue(false);
    return JsValue(text);
}

JsArray stringArray(const std::vector<std::string>& texts) {
    JsArray array;
    for (const std::string& text : texts) array.push_back(JsValue(text));
    return array;
}

// `key` set to `value`, or gone when `keep` is false.
void setOrErase(JsObject& object, const std::string& key, bool keep, JsValue value) {
    if (keep) {
        object[key] = std::move(value);
    } else {
        object.erase(key);
    }
}

}  // namespace

std::string Config::toJson() const {
    JsObject top;
    if (!source.empty()) {
        const JsValue before = JsParseString(source);
        if (before.IsObject()) top = before.GetJsObject();
    }

    setOrErase(top, "environment", !environment.empty(), JsValue(environment));
    setOrErase(top, "offline", offline || top.count("offline"), JsValue(offline));
    setOrErase(top, "cacheLimitGB", cacheLimitGB.has_value(), JsValue(cacheLimitGB.value_or(0.0)));

    JsObject http = objectAt(top, "http");
    {
        const JsObject hostsBefore = objectAt(http, "hosts");
        JsObject hosts;
        for (const auto& [host, entry] : httpHosts) {
            const auto found = hostsBefore.find(host);
            JsObject object = found != hostsBefore.end() && found->second.IsObject() ? found->second.GetJsObject()
                                                                                     : JsObject();
            setOrErase(object, "credential", !entry.credential.empty(), JsValue(entry.credential));
            setOrErase(object, "insecure", entry.insecure || object.count("insecure"), JsValue(entry.insecure));
            hosts[host] = JsValue(object);
        }
        setOrErase(http, "hosts", !hosts.empty(), JsValue(hosts));
        setOrErase(http, "allow", !httpAllow.empty(), JsValue(stringArray(httpAllow)));
    }
    setOrErase(top, "http", !http.empty(), JsValue(http));

    JsObject s3 = objectAt(top, "s3");
    setOrErase(s3, "allow", !s3Allow.empty(), JsValue(stringArray(s3Allow)));
    setOrErase(top, "s3", !s3.empty(), JsValue(s3));

    const JsObject endpointsBefore = objectAt(top, "endpoints");
    JsObject endpointsObject;
    for (const auto& [id, endpoint] : endpoints) {
        const auto found = endpointsBefore.find(id);
        const JsObject* before =
            found != endpointsBefore.end() && found->second.IsObject() ? &found->second.GetJsObject() : nullptr;
        JsObject object;
        // What the file had that isn't text (an option the library couldn't read as one) stays.
        if (before) {
            for (const auto& [key, value] : *before) {
                if (value.IsObject() || value.IsArray()) object[key] = value;
            }
        }
        object["scheme"] = JsValue(endpoint.scheme);
        object["url"] = JsValue(endpoint.url);
        setOrErase(object, "credential", !endpoint.credential.empty(), JsValue(endpoint.credential));
        setOrErase(object, "readOnly", endpoint.readOnly, JsValue(true));
        setOrErase(object, "optional", endpoint.optional, JsValue(true));
        setOrErase(object, "mirrors", !endpoint.mirrors.empty(), JsValue(stringArray(endpoint.mirrors)));
        for (const auto& [key, text] : endpoint.options) object[key] = optionValue(before, key, text);
        endpointsObject[id] = JsValue(object);
    }
    setOrErase(top, "endpoints", !endpointsObject.empty(), JsValue(endpointsObject));

    JsObject environmentsObject;
    for (const auto& [environment, schemes] : environments) {
        JsObject schemesObject;
        for (const auto& [scheme, names] : schemes) {
            JsObject namesObject;
            for (const auto& [name, ids] : names) namesObject[name] = JsValue(stringArray(ids));
            schemesObject[scheme] = JsValue(namesObject);
        }
        environmentsObject[environment] = JsValue(schemesObject);
    }
    setOrErase(top, "environments", !environmentsObject.empty(), JsValue(environmentsObject));

    std::ostringstream text;
    {
        JsWriter writer(text, JsWriter::Style::Pretty);
        JsWriteValue(&writer, JsValue(top));
    }
    return text.str() + "\n";
}

Result<bool> Config::save(const std::filesystem::path& file) const {
    std::error_code error;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), error);
    std::filesystem::path temporary = file;
    temporary += ".saving";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return Result<bool>::failure(Status::NotAllowed, "can't write " + toUtf8(temporary));
        output << toJson();
        if (!output) return Result<bool>::failure(Status::Error, "couldn't write " + toUtf8(temporary));
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return Result<bool>::failure(Status::NotAllowed, "can't replace " + toUtf8(file));
    }
    return Result<bool>::success(true);
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
