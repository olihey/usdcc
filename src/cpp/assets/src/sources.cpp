#include "usdcc/assets/sources.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/credentials.h"
#include "usdcc/assets/http_client.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>
#include <pxr/base/tf/getenv.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

namespace usdcc::assets {

std::vector<std::string> MergeProblems::all() const {
    std::vector<std::string> both = personal;
    both.insert(both.end(), sources.begin(), sources.end());
    return both;
}

std::string qualifiedId(const std::string& alias, const std::string& id) {
    return alias.empty() ? id : alias + "/" + id;
}

std::string qualifiedCredential(const std::string& alias, const std::string& id) {
    if (id.empty() || isAwsCredential(id)) return id;
    return qualifiedId(alias, id);
}

std::string aliasOf(const std::string& id) {
    const size_t slash = id.find('/');
    return slash == std::string::npos ? std::string() : id.substr(0, slash);
}

namespace {

PXR_NAMESPACE_USING_DIRECTIVE

constexpr uint64_t kMaxSourceBytes = 8 * 1024 * 1024;
constexpr std::chrono::milliseconds kLoadTimeout{5000};

std::atomic<bool> s_fetchAtLoad{true};
std::atomic<bool> s_allowLoopback{false};

// How the last try in this process went, by copy file.
struct Tries {
    std::mutex mutex;
    std::map<std::string, SourceRefresh> last;
};

Tries& tries() {
    static Tries instance;
    return instance;
}

double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// One file per alias and URL: a server may answer each login differently.
std::filesystem::path copyFile(const SourceEntry& entry) {
    const std::string named = TfGetenv("USDCC_ASSET_SOURCES");
    const std::filesystem::path folder = named.empty() ? userDataFolder() / "asset-sources" : fromUtf8(named);
    return folder / (stableHash(entry.alias + "|" + entry.url, 16) + ".json");
}

// What's kept: the text as it was served, and what the server said of it, in one file so that
// they can't disagree.
struct Copy {
    std::string text;
    std::string etag;
    double checked = 0;
    double changed = 0;
};

std::string textAt(const JsObject& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->second.IsString() ? found->second.GetString() : std::string();
}

double numberAt(const JsObject& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return 0;
    return found->second.IsReal() || found->second.IsInt() ? found->second.GetReal() : 0;
}

std::optional<Copy> readCopy(const SourceEntry& entry) {
    std::ifstream input(copyFile(entry), std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream text;
    text << input.rdbuf();
    const JsValue root = JsParseString(text.str());
    if (!root.IsObject()) return std::nullopt;
    const JsObject& object = root.GetJsObject();
    // Not another's, should two hashes meet.
    if (textAt(object, "url") != entry.url || textAt(object, "alias") != entry.alias) return std::nullopt;
    return Copy{textAt(object, "text"), textAt(object, "etag"), numberAt(object, "checked"), numberAt(object, "changed")};
}

bool writeCopy(const SourceEntry& entry, const Copy& copy) {
    const std::filesystem::path file = copyFile(entry);
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // A name of its own: usdcc and a tool may both be refreshing.
    std::filesystem::path temporary = file;
    temporary += "." + stableHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                      std::to_string(reinterpret_cast<uintptr_t>(&copy)), 8) + ".saving";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        JsObject object;
        object["alias"] = JsValue(entry.alias);
        object["url"] = JsValue(entry.url);
        object["etag"] = JsValue(copy.etag);
        object["checked"] = JsValue(copy.checked);
        object["changed"] = JsValue(copy.changed);
        object["text"] = JsValue(copy.text);
        JsWriter writer(output, JsWriter::Style::Pretty);
        JsWriteValue(&writer, JsValue(object));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (!error) return true;
    std::filesystem::remove(temporary, error);
    return false;
}

// "https://host:port", in lowercase.
std::string originOf(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    std::string origin = url.substr(0, std::min(url.find_first_of("/?#", scheme + 3), url.size()));
    std::transform(origin.begin(), origin.end(), origin.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return origin;
}

SourceRefresh fetch(const SourceEntry& entry, std::chrono::milliseconds timeout, bool anonymous) {
    SourceRefresh result;
    if (result.problem = badSourceUrl(entry.url); !result.problem.empty()) return result;
    std::optional<Copy> copy = readCopy(entry);
    // One this version can't read is fetched whole.
    if (copy && !Config::parse(copy->text).ok()) copy.reset();

    std::string url = entry.url;
    HttpResponse response;
    for (int hop = 0;; ++hop) {
        HttpRequest request;
        request.url = url;
        request.headers.push_back("Accept: application/json");
        if (copy && !copy->etag.empty()) request.headers.push_back("If-None-Match: " + copy->etag);
        if (!anonymous) request.credential = sourceCredentialId(entry.alias);
        // Only loopback gets here over http, and only in tests.
        request.credentialOverHttp = s_allowLoopback.load();
        request.connectTimeout = std::min<std::chrono::milliseconds>(timeout, std::chrono::milliseconds(5000));
        request.totalTimeout = timeout;
        request.maxBytes = kMaxSourceBytes;
        Progress progress;
        Cancel cancel;
        response = httpRequest(request, progress, cancel);
        if (response.status != Status::Ok || response.code < 301 || response.code > 308 || response.code == 304) break;
        std::string to = response.header("location");
        if (!to.empty() && to[0] == '/' && to.compare(0, 2, "//") != 0) to = originOf(url) + to;
        if (hop >= 5 || originOf(to) != originOf(url)) {
            result.problem = "it redirects to another host, or too often";
            return result;
        }
        url = to;
    }
    if (response.status != Status::Ok) {
        result.problem = response.message.empty() ? std::string("it can't be reached") : response.message;
        return result;
    }
    if (response.code == 304 && copy) {
        copy->checked = now();
        writeCopy(entry, *copy);
        result.what = Refreshed::Unchanged;
        return result;
    }
    if (response.code != 200) {
        result.wantsLogin = statusOfHttpCode(response.code) == Status::NotAllowed;
        result.problem = result.wantsLogin
                             ? "it wants a login, saved as '" + sourceCredentialId(entry.alias) + "' (HTTP " +
                                   std::to_string(response.code) + ")"
                             : "HTTP " + std::to_string(response.code);
        return result;
    }
    const Result<Config> parsed = response.body.size() > kMaxSourceBytes
                                      ? Result<Config>::failure(Status::Error, "it is too large")
                                      : Config::parse(response.body);
    if (!parsed.ok()) {
        result.problem = "what it serves isn't a configuration: " + parsed.message;
        return result;
    }
    // Any JSON object parses, a server's error message too: one with nothing in it isn't taken.
    const Config& config = parsed.value;
    if (config.endpoints.empty() && config.environments.empty() && config.httpHosts.empty() && config.httpAllow.empty() &&
        config.s3Allow.empty()) {
        result.problem = "what it serves has no servers, environments or web hosts";
        return result;
    }
    Copy fresh;
    fresh.text = response.body;
    fresh.etag = response.header("etag");
    fresh.checked = now();
    const bool same = copy && copy->text == fresh.text;
    fresh.changed = same ? copy->changed : fresh.checked;
    if (!writeCopy(entry, fresh)) {
        result.problem = "its copy can't be written to " + toUtf8(copyFile(entry));
        return result;
    }
    result.what = same ? Refreshed::Unchanged : Refreshed::Changed;
    return result;
}

// From its copy: opening a stage never waits for a server.
void loadFetched(LoadedSource& source) {
    const SourceEntry& entry = source.entry;
    if (source.problem = badSourceUrl(entry.url); !source.problem.empty()) return;
    std::optional<Copy> copy = readCopy(entry);
    if (!copy && s_fetchAtLoad.load() && !isOffline()) {
        refreshSource(entry, kLoadTimeout);
        copy = readCopy(entry);
    }
    {
        Tries& all = tries();
        std::lock_guard<std::mutex> lock(all.mutex);
        const auto last = all.last.find(toUtf8(copyFile(entry)));
        if (last != all.last.end()) {
            source.fetchProblem = last->second.problem;
            source.wantsLogin = last->second.wantsLogin;
        }
    }
    if (!copy) {
        source.problem = source.fetchProblem.empty() ? std::string("it hasn't been fetched yet") : source.fetchProblem;
        return;
    }
    source.checked = copy->checked;
    source.changed = copy->changed;
    Result<Config> parsed = Config::parse(copy->text);
    if (!parsed.ok()) {
        source.problem = parsed.message;
        return;
    }
    source.config = std::move(parsed.value);
}

}  // namespace

std::string sourceCredentialId(const std::string& alias) { return alias + "/@source"; }

std::string badSourceUrl(const std::string& url) {
    const std::string origin = originOf(url);
    const size_t scheme = origin.find("://");
    if (scheme == std::string::npos || origin.size() <= scheme + 3) return "its URL isn't one";
    if (url.find_first_of(" \t\r\n") != std::string::npos) return "its URL has spaces";
    if (origin.find('@') != std::string::npos) return "its URL has a login in it; save the login instead";
    if (origin.compare(0, scheme, "https") == 0) return std::string();
    const std::string host = origin.substr(scheme + 3);
    if (s_allowLoopback.load() && origin.compare(0, scheme, "http") == 0 &&
        (host == "127.0.0.1" || host.rfind("127.0.0.1:", 0) == 0)) {
        return std::string();
    }
    return "its URL has to be https://";
}

SourceRefresh refreshSource(const SourceEntry& entry, std::chrono::milliseconds timeout, bool anonymous) {
    const SourceRefresh result = fetch(entry, timeout, anonymous);
    Tries& all = tries();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.last[toUtf8(copyFile(entry))] = result;
    return result;
}

void forgetSourceCopy(const SourceEntry& entry) {
    if (entry.url.empty()) return;
    const std::filesystem::path file = copyFile(entry);
    std::error_code error;
    std::filesystem::remove(file, error);
    Tries& all = tries();
    std::lock_guard<std::mutex> lock(all.mutex);
    all.last.erase(toUtf8(file));
}

void setFetchSourcesAtLoad(bool fetch) { s_fetchAtLoad.store(fetch); }

void allowLoopbackSources(bool allow) { s_allowLoopback.store(allow); }

LoadedSource loadSource(const SourceEntry& entry, const std::filesystem::path& personalFile) {
    LoadedSource source;
    source.entry = entry;
    if (!entry.url.empty()) {
        loadFetched(source);
        return source;
    }
    std::filesystem::path file = fromUtf8(entry.path);
    if (file.is_relative() && !personalFile.empty()) file = personalFile.parent_path() / file;
    source.file = file;
    Result<Config> loaded = Config::load(file);
    if (!loaded.ok()) {
        source.problem = loaded.message;
        return source;
    }
    source.config = std::move(loaded.value);
    return source;
}

std::vector<LoadedSource> loadSources(const Config& personal, const std::filesystem::path& personalFile) {
    std::vector<LoadedSource> sources;
    for (const SourceEntry& entry : personal.sources) sources.push_back(loadSource(entry, personalFile));
    return sources;
}

Config mergeSources(const Config& personal, const std::vector<LoadedSource>& sources, MergeProblems* problems) {
    MergeProblems ignored;
    MergeProblems& said = problems ? *problems : ignored;
    Config merged = personal;
    merged.sources.clear();
    merged.source.clear();
    merged.extends.clear();
    // Mixed environments are made below, from the sources.
    for (const auto& [environment, extended] : personal.extends) merged.environments.erase(environment);
    for (auto it = merged.environments.begin(); it != merged.environments.end();) {
        it = personal.isMixed(it->first) ? merged.environments.erase(it) : std::next(it);
    }
    std::map<std::string, std::string> hostFrom;  // a web host, and whose it is
    for (const auto& [host, entry] : personal.httpHosts) hostFrom[host] = "your configuration";

    for (const LoadedSource& source : sources) {
        const std::string& alias = source.entry.alias;
        // Also of one that can't be fetched: for want of that login, maybe.
        if (!source.entry.url.empty()) merged.sourceLogins[sourceCredentialId(alias)] = alias;
        if (!source.problem.empty()) {
            said.sources.push_back("source '" + alias + "' isn't used: " + source.problem);
            continue;
        }
        const Config& theirs = source.config;
        if (!theirs.environment.empty()) merged.sourceDefaults.push_back(qualifiedId(alias, theirs.environment));
        if (!theirs.sources.empty() || !theirs.extends.empty()) {
            said.sources.push_back("source '" + alias + "': its own sources and mixed environments are left out");
        }
        for (const auto& [id, endpoint] : theirs.endpoints) {
            Endpoint qualified = endpoint;
            qualified.id = qualifiedId(alias, id);
            qualified.credential = qualifiedCredential(alias, endpoint.credential);
            merged.endpoints[qualified.id] = std::move(qualified);
        }
        for (const auto& [environment, schemes] : theirs.environments) {
            if (theirs.isMixed(environment)) continue;
            auto& ofEnvironment = merged.environments[qualifiedId(alias, environment)];
            for (const auto& [scheme, names] : schemes) {
                for (const auto& [name, ids] : names) {
                    std::vector<std::string> stack;
                    for (const std::string& id : ids) stack.push_back(qualifiedId(alias, id));
                    ofEnvironment[scheme][name] = std::move(stack);
                }
            }
        }
        for (const auto& [host, entry] : theirs.httpHosts) {
            const auto taken = hostFrom.find(host);
            if (taken != hostFrom.end()) {
                said.sources.push_back("web host " + host + " is in " + taken->second + " and in source '" + alias +
                                       "': the first one's is used");
                continue;
            }
            HttpHost qualified = entry;
            qualified.credential = qualifiedCredential(alias, entry.credential);
            merged.httpHosts[host] = qualified;
            hostFrom[host] = "source '" + alias + "'";
        }
        // A bare * would turn the check off for every stage.
        for (const std::string& pattern : theirs.httpAllow) {
            if (pattern != "*") merged.httpAllow.push_back(pattern);
        }
        for (const std::string& pattern : theirs.s3Allow) {
            if (pattern != "*") merged.s3Allow.push_back(pattern);
        }
    }

    // Mixed environments: what they extend, then their own names, which replace those.
    for (const auto& [environment, schemes] : personal.environments) {
        if (!personal.isMixed(environment)) continue;
        const std::string where = "environment '" + environment + "'";
        auto& made = merged.environments[environment];
        // scheme://name -> (the stack, where from), and the names two give differently.
        std::map<std::pair<std::string, std::string>, std::pair<std::vector<std::string>, std::string>> taken;
        std::set<std::pair<std::string, std::string>> clashing;
        const auto extended = personal.extends.find(environment);
        if (extended != personal.extends.end()) {
            for (const std::string& other : extended->second) {
                const auto found = merged.environments.find(other);
                if (aliasOf(other).empty() || found == merged.environments.end()) {
                    said.personal.push_back(where + " extends '" + other +
                                            "', which no source has (only a source's environments can be extended)");
                    continue;
                }
                for (const auto& [scheme, names] : found->second) {
                    for (const auto& [name, stack] : names) {
                        const auto key = std::make_pair(scheme, name);
                        const auto before = taken.find(key);
                        if (before == taken.end()) {
                            taken[key] = {stack, other};
                        } else if (before->second.first != stack) {
                            clashing.insert(key);
                        }
                    }
                }
            }
        }
        for (const auto& [scheme, names] : schemes) {
            for (const auto& [name, ids] : names) {
                std::vector<std::string> stack;
                for (const std::string& id : ids) {
                    const auto endpoint = merged.endpoints.find(id);
                    if (endpoint == merged.endpoints.end()) {
                        said.personal.push_back(where + ", " + scheme + "://" + name + ": there is no endpoint '" + id + "'");
                    } else if (endpoint->second.scheme != scheme) {
                        said.personal.push_back(where + ", " + scheme + "://" + name + ": endpoint '" + id + "' is " +
                                                endpoint->second.scheme + ", and a stack holds one protocol");
                    } else {
                        stack.push_back(id);
                    }
                }
                const auto key = std::make_pair(scheme, name);
                clashing.erase(key);
                taken[key] = {std::move(stack), environment};
            }
        }
        for (const auto& key : clashing) {
            said.personal.push_back(where + ": " + key.first + "://" + key.second +
                                    " is in two environments it extends, differently; give it a stack of its own");
            taken.erase(key);
        }
        for (auto& [key, value] : taken) {
            if (!value.first.empty()) made[key.first][key.second] = std::move(value.first);
        }
    }

    if (!merged.environment.empty() && !merged.environments.count(merged.environment)) {
        said.personal.push_back("the default environment '" + merged.environment + "' isn't there");
    }

    // Two credential ids with one spelling of their variables: neither gets them.
    std::map<std::string, std::set<std::string>> spellings;
    for (const auto& [id, endpoint] : merged.endpoints) {
        if (!endpoint.credential.empty()) spellings[credentialVariablePrefix(endpoint.credential)].insert(endpoint.credential);
    }
    for (const auto& [host, entry] : merged.httpHosts) {
        if (!entry.credential.empty()) spellings[credentialVariablePrefix(entry.credential)].insert(entry.credential);
    }
    for (const auto& [id, alias] : merged.sourceLogins) spellings[credentialVariablePrefix(id)].insert(id);
    std::set<std::string> ambiguous;
    for (const auto& [prefix, ids] : spellings) {
        if (ids.size() < 2) continue;
        ambiguous.insert(prefix);
        std::string names;
        for (const std::string& id : ids) names += (names.empty() ? "" : " and ") + id;
        said.sources.push_back("the credentials " + names + " would both read " + prefix +
                               "_USER and the like, so neither does");
    }
    setAmbiguousCredentialVariables(ambiguous);
    return merged;
}

std::vector<std::string> environmentsWithName(const Config& config, const std::string& scheme, const std::string& name) {
    std::vector<std::string> found;
    for (const auto& [environment, schemes] : config.environments) {
        const auto ofScheme = schemes.find(scheme);
        if (ofScheme != schemes.end() && ofScheme->second.count(name)) found.push_back(environment);
    }
    return found;
}

std::vector<std::string> offeredEnvironments(const Config& config, const std::string& scheme, const std::string& name) {
    const std::vector<std::string> all = environmentsWithName(config, scheme, name);
    std::vector<std::string> offered;
    for (const std::string& environment : all) {
        const std::string alias = aliasOf(environment);
        const auto isDefault = [&](const std::string& other) {
            return std::find(config.sourceDefaults.begin(), config.sourceDefaults.end(), other) != config.sourceDefaults.end();
        };
        // A source's default stands for that source, when it has the name.
        const bool sourceDefaultHasIt = !alias.empty() && std::any_of(all.begin(), all.end(), [&](const std::string& other) {
            return aliasOf(other) == alias && isDefault(other);
        });
        if (!sourceDefaultHasIt || isDefault(environment)) offered.push_back(environment);
    }
    return offered;
}

std::string suggestedAlias(const Config& source, const std::filesystem::path& file) {
    if (!source.name.empty() && isValidName(source.name)) return source.name;
    std::string made;
    for (const char c : toUtf8(file.stem())) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            made += static_cast<char>(std::tolower(u));
        } else if (!made.empty() && made.back() != '-') {
            made += '-';
        }
    }
    while (!made.empty() && made.back() == '-') made.pop_back();
    return made.empty() ? std::string("source") : made;
}

}  // namespace usdcc::assets
