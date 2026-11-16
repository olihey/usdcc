#pragma once

// Check Physics (docs/PHYSICS.md §4.8, §5.4): what OpenUSD's own physics validators say
// about a stage, and what a simulation of it would leave out or change, without running one.
//
// Qt-free.

#include "usdcc/usd/physics_desc.h"

#include <pxr/usd/usd/common.h>

#include <string>
#include <vector>

namespace usdcc::usd::physics {

struct CheckFinding {
    enum class Source { Validator, Simulation };
    Source source = Source::Simulation;
    bool error = false;  // a validator's error, not a warning
    PXR_NS::SdfPath prim;
    std::string message;
};

// The usdPhysicsValidators' findings (rigid bodies, colliders, articulations, joints), then
// the parsing's and the current backend's reports for a world built at `frame`. Builds a
// world, never steps it, and leaves any simulation cache alone.
std::vector<CheckFinding> checkPhysics(const PXR_NS::UsdStageRefPtr& stage, double frame);

}  // namespace usdcc::usd::physics
