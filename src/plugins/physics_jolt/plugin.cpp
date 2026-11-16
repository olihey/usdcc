// Jolt Physics, usdcc's physics backend (docs/PHYSICS.md §4.1): a bundled plugin, built
// and enabled in every build. All it does is register the backend; the timeline and the
// Physics menu drive it.
#include "jolt_backend.h"

#include "usdcc/plugin/plugin.h"

namespace {
std::string s_backendId;
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    auto backend = usdcc::physics_jolt::makeJoltBackend();
    s_backendId = backend->id();
    host.addPhysicsBackend(std::move(backend));
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host&) {
    usdcc::usd::physics::PhysicsBackends::instance().remove(s_backendId);
}
