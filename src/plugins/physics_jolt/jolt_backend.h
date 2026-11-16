#pragma once

// Jolt Physics behind usdcc's backend interface (docs/PHYSICS.md §5.3). A static library,
// so the plugin and the tests share it.

#include "usdcc/usd/physics_backend.h"

#include <memory>

namespace usdcc::physics_jolt {

// Jolt's process-wide setup (its allocator, factory and types) happens on the first call.
std::shared_ptr<usdcc::usd::physics::PhysicsBackend> makeJoltBackend();

}  // namespace usdcc::physics_jolt
