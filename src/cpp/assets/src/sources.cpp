#include "usdcc/assets/sources.h"

#include "usdcc/assets/credentials.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/uri.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

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

LoadedSource loadSource(const SourceEntry& entry, const std::filesystem::path& personalFile) {
    LoadedSource source;
    source.entry = entry;
    if (!entry.url.empty()) {
        source.problem = "a source from a URL isn't read yet";
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
