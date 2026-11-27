#pragma once

// The resolver context of every protocol (docs/REMOTE_ASSETS.md §4): an environment,
// and through it the stack of endpoints behind each name, with each server under its own
// id beside them. It holds credential ids,
// never secrets, as contexts are hashed, compared, printed and saved.

#include "usdcc/assets/config.h"
#include "usdcc/assets/transport.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/defineResolverContext.h>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::assets {

// The endpoints behind one name in one environment, top first. Its id is what a
// resolved path carries (§5), so that the path can be opened with no context bound.
struct Stack {
    std::string id;  // a short hash of the scheme, the name and the endpoints in order
    std::string scheme;
    std::string name;
    std::vector<Endpoint> endpoints;
};

// Every stack a context was made with, for the whole process. Null for an unknown id.
std::shared_ptr<const Stack> findStack(const std::string& id);

class Context {
public:
    Context() = default;
    // The stacks of `environment` in `config`, and each server by itself under its id
    // (ownName()) where the environment gives that name no stack: a server needs no
    // environment to be reached (docs/REMOTE_ASSETS.md §4.1). With no environment (an empty
    // one), the servers alone. An environment the configuration doesn't have gives a
    // context with no names.
    Context(const Config& config, std::string environment);

    const std::string& environment() const { return m_environment; }
    // Null when the environment has no such name for that scheme.
    std::shared_ptr<const Stack> stack(const std::string& scheme, const std::string& name) const;
    std::vector<std::string> names(const std::string& scheme) const;
    // Why stack() has none for `name`: "the environment 'prod' has no server named 'lib'",
    // or, with no environment, "there is no server named 'lib'".
    std::string whyNoName(const std::string& name) const;

    bool operator==(const Context& other) const;
    bool operator<(const Context& other) const;
    // Names, URLs and credential ids. Never a secret: there is none in here.
    std::string debugString() const;

private:
    friend size_t hash_value(const Context& context);

    std::string m_environment;
    // (scheme, name) -> stack id
    std::map<std::pair<std::string, std::string>, std::string> m_stacks;
};

size_t hash_value(const Context& context);
// Found by ArResolverContext::GetDebugString().
std::string ArGetDebugString(const Context& context);

// The context used when none is bound: the default configuration's chosen environment.
std::shared_ptr<const Context> defaultContext();
// The configuration defaultContext() and CreateContextFromString() use. Loaded from
// Config::defaultPath() on first use.
Config defaultConfig();
// Replaces it, for the app's settings page and for tests.
void setDefaultConfig(const Config& config);
// Goes back to the file at Config::defaultPath(), after setDefaultConfig().
void useDefaultConfigFile();
// Reads the file again, and forgets what servers answered. A configuration the host
// set with setDefaultConfig() is kept, as there may be no file behind it.
void reloadDefaultConfig();

}  // namespace usdcc::assets

PXR_NAMESPACE_OPEN_SCOPE
AR_DECLARE_RESOLVER_CONTEXT(usdcc::assets::Context);
PXR_NAMESPACE_CLOSE_SCOPE
