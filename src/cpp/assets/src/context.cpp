#include "usdcc/assets/context.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/sources.h"
#include "usdcc/assets/uri.h"

#include <pxr/base/tf/diagnostic.h>

#include <functional>
#include <mutex>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::assets {

namespace {

struct StackRegistry {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const Stack>> stacks;
};

StackRegistry& stackRegistry() {
    static StackRegistry instance;
    return instance;
}

bool sameEndpoints(const Stack& a, const Stack& b) {
    if (a.scheme != b.scheme || a.name != b.name || a.endpoints.size() != b.endpoints.size()) return false;
    for (size_t i = 0; i < a.endpoints.size(); ++i) {
        const Endpoint& x = a.endpoints[i];
        const Endpoint& y = b.endpoints[i];
        if (x.url != y.url || x.credential != y.credential) return false;
    }
    return true;
}

std::string registerStack(Stack stack) {
    std::string text = stack.scheme + "|" + stack.name;
    for (const Endpoint& endpoint : stack.endpoints) text += "|" + endpoint.cacheId();
    stack.id = stableHash(text, 8);

    StackRegistry& all = stackRegistry();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = all.stacks.find(stack.id);
    if (found != all.stacks.end() && !sameEndpoints(*found->second, stack)) {
        TF_WARN("Two asset server stacks share the id %s (%s://%s and %s://%s); the second replaces the first",
                stack.id.c_str(), found->second->scheme.c_str(), found->second->name.c_str(), stack.scheme.c_str(),
                stack.name.c_str());
    }
    const std::string id = stack.id;
    // Replaced even when equal: read-only and optional may have changed, and they aren't in the id.
    all.stacks[id] = std::make_shared<const Stack>(std::move(stack));
    return id;
}

struct Defaults {
    std::mutex mutex;
    bool loaded = false;
    bool fromFile = true;
    Config config;
    std::shared_ptr<const Context> context;
};

Defaults& defaults() {
    static Defaults instance;
    return instance;
}

// With the lock held.
void loadDefaults(Defaults& all, const Config* replacement) {
    all.config = replacement ? *replacement : Config::loadDefault();
    all.fromFile = replacement == nullptr;
    all.context = std::make_shared<const Context>(all.config, all.config.chosenEnvironment());
    all.loaded = true;
    // Its switches, and nothing remembered from the configuration before it.
    setConfigOffline(all.config.offline);
    // Under the app's limit, when it sets one (DiskCache::setLimit).
    std::optional<uint64_t> limit;
    if (all.config.cacheLimitGB && *all.config.cacheLimitGB > 0) {
        limit = static_cast<uint64_t>(*all.config.cacheLimitGB * 1024.0 * 1024.0 * 1024.0);
    }
    DiskCache::shared().setConfigLimit(limit);
    forgetLookups();
    forgetDownMarks();
}

}  // namespace

std::shared_ptr<const Stack> findStack(const std::string& id) {
    StackRegistry& all = stackRegistry();
    std::lock_guard<std::mutex> lock(all.mutex);
    const auto found = all.stacks.find(id);
    return found == all.stacks.end() ? nullptr : found->second;
}

Context::Context(const Config& config, std::string environment) : m_environment(std::move(environment)) {
    const auto ofEnvironment = config.environments.find(m_environment);
    // One that isn't configured has no names: a job naming an environment this machine lacks
    // finds nothing, rather than other files. No environment (an empty one) is the servers alone.
    if (ofEnvironment == config.environments.end() && !m_environment.empty()) return;
    // The names the environment gives, whether or not their servers are there.
    std::set<std::pair<std::string, std::string>> given;
    if (ofEnvironment != config.environments.end()) {
        for (const auto& [scheme, names] : ofEnvironment->second) {
            for (const auto& [name, ids] : names) {
                given.insert({scheme, name});
                Stack stack;
                stack.scheme = scheme;
                stack.name = name;
                for (const std::string& id : ids) {
                    const auto endpoint = config.endpoints.find(id);
                    if (endpoint != config.endpoints.end()) stack.endpoints.push_back(endpoint->second);
                }
                if (!stack.endpoints.empty()) m_stacks[{scheme, name}] = registerStack(std::move(stack));
            }
        }
    }
    // Each server is a name by itself too, its id (docs/REMOTE_ASSETS.md §4.1), where the
    // environment gives that name no stack of its own.
    for (const auto& [id, endpoint] : config.endpoints) {
        const std::string name = ownName(id, m_environment);
        if (name.empty() || given.count({endpoint.scheme, name})) continue;
        Stack stack;
        stack.scheme = endpoint.scheme;
        stack.name = name;
        stack.endpoints.push_back(endpoint);
        m_stacks[{endpoint.scheme, name}] = registerStack(std::move(stack));
    }
}

std::shared_ptr<const Stack> Context::stack(const std::string& scheme, const std::string& name) const {
    const auto found = m_stacks.find({scheme, name});
    return found == m_stacks.end() ? nullptr : findStack(found->second);
}

std::vector<std::string> Context::names(const std::string& scheme) const {
    std::vector<std::string> names;
    for (const auto& [key, id] : m_stacks) {
        if (key.first == scheme) names.push_back(key.second);
    }
    return names;
}

std::string Context::whyNoName(const std::string& name) const {
    if (m_environment.empty()) return "there is no server named '" + name + "'";
    return "the environment '" + m_environment + "' has no server named '" + name + "'";
}

bool Context::operator==(const Context& other) const {
    return m_environment == other.m_environment && m_stacks == other.m_stacks;
}

bool Context::operator<(const Context& other) const {
    if (m_environment != other.m_environment) return m_environment < other.m_environment;
    return m_stacks < other.m_stacks;
}

std::string Context::debugString() const {
    std::string text = "environment '" + m_environment + "'";
    for (const auto& [key, id] : m_stacks) {
        text += "\n  " + key.first + "://" + key.second + " (" + id + "):";
        if (const std::shared_ptr<const Stack> stack = findStack(id)) {
            for (const Endpoint& endpoint : stack->endpoints) {
                text += " " + endpoint.id + "=" + endpoint.url;
                if (!endpoint.credential.empty()) text += " [credential " + endpoint.credential + "]";
            }
        }
    }
    return text;
}

size_t hash_value(const Context& context) {
    size_t hash = std::hash<std::string>()(context.m_environment);
    for (const auto& [key, id] : context.m_stacks) {
        hash = hash * 31 + std::hash<std::string>()(key.first + "|" + key.second + "|" + id);
    }
    return hash;
}

std::string ArGetDebugString(const Context& context) { return context.debugString(); }

std::shared_ptr<const Context> defaultContext() {
    Defaults& all = defaults();
    std::lock_guard<std::mutex> lock(all.mutex);
    if (!all.loaded) loadDefaults(all, nullptr);
    return all.context;
}

Config defaultConfig() {
    Defaults& all = defaults();
    std::lock_guard<std::mutex> lock(all.mutex);
    if (!all.loaded) loadDefaults(all, nullptr);
    return all.config;
}

void setDefaultConfig(const Config& config) {
    Defaults& all = defaults();
    std::lock_guard<std::mutex> lock(all.mutex);
    loadDefaults(all, &config);
}

void useDefaultConfigFile() {
    Defaults& all = defaults();
    std::lock_guard<std::mutex> lock(all.mutex);
    loadDefaults(all, nullptr);
}

void reloadDefaultConfig() {
    Defaults& all = defaults();
    std::lock_guard<std::mutex> lock(all.mutex);
    // One the host set itself stays: there may be no file behind it.
    if (all.loaded && !all.fromFile) {
        const Config kept = all.config;
        loadDefaults(all, &kept);
    } else {
        loadDefaults(all, nullptr);
    }
}

}  // namespace usdcc::assets
