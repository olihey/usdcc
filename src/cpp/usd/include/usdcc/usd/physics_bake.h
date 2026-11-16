#pragma once

// Keeping a simulation (docs/PHYSICS.md §4.5): the cache's poses become plain time samples
// on transform ops in the stage's edit target, which every reader understands.
//
// Each baked body gets `[xformOp:translate:sim, xformOp:orient:sim, xformOp:scale:sim]`
// (or one `xformOp:transform:sim` where its local transform has shear), sampled at every
// cached frame, relative to its parent as the stage composes it with the cache: a body
// under another body is baked against that body's simulated pose, so every frame looks
// as it did. No `!resetXformStack!` reaches the user's layer.
//
// Qt-free. The caller makes it one undo step and clears the cache afterwards.

#include "usdcc/usd/physics_desc.h"
#include "usdcc/usd/time_range.h"

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <functional>
#include <optional>
#include <vector>

namespace usdcc::usd::physics {

class PhysicsSession;

struct BakeOptions {
    // The prims whose bodies to bake: a body, or anything above bodies. Empty: all.
    std::vector<PXR_NS::SdfPath> prims;
    // physics:kinematicEnabled on each baked body, so a later run moves it as animated
    // instead of solving it again on top of its own animation.
    bool markKinematic = false;
};

struct BakeResult {
    std::vector<PXR_NS::SdfPath> baked;
    std::optional<TimeRange> frames;
    std::vector<PhysicsReport> reports;
};

// Makes the cache hold all of `range` (whole frames), as a bake needs: solves anew from its
// first frame when there's no world, the cache is stale or starts elsewhere, then on to its
// last. `progress(done, total)` follows each frame; returning false cancels, and then this
// returns false with what was solved kept.
bool cacheRange(PhysicsSession& session, const TimeRange& range, const std::function<bool(int, int)>& progress = {});

// The dynamic bodies of `session`'s scene that `options` chooses.
std::vector<PXR_NS::SdfPath> bodiesToBake(const PhysicsSession& session, const BakeOptions& options);

// Writes the cached frames of the chosen bodies into the stage's edit target. Reads
// everything before writing anything; reports a body whose ops a stronger layer than the
// edit target overrides, as the bake would then not show.
BakeResult bakeFromCache(const PXR_NS::UsdStageRefPtr& stage, const PhysicsSession& session, const BakeOptions& options);

}  // namespace usdcc::usd::physics
