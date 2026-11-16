#pragma once

// The simulation cache (docs/PHYSICS.md §4.2, §4.6): an anonymous sublayer of the session
// layer that holds the solver's poses as time samples. The user's layers are never
// touched; muting it shows the scene as authored; it is never saved.
//
// Each dynamic body gets, in this layer, `[!resetXformStack!, xformOp:translate:sim,
// xformOp:orient:sim, xformOp:scale:sim]`: its world pose, so neither its parents nor the
// order bodies are written in matter. A body whose world scale has shear gets one
// `xformOp:transform:sim` instead.
//
// Qt-free.

#include "usdcc/usd/physics_desc.h"
#include "usdcc/usd/time_range.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/common.h>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd::physics {

class SimCache {
public:
    // The tag in the layer's identifier ("anon:0x...:usdcc_simulation_cache").
    static constexpr const char* kTag = "usdcc_simulation_cache";
    static bool isCacheLayer(const PXR_NS::SdfLayerHandle& layer);
    static bool isCacheLayerIdentifier(const std::string& identifier);

    explicit SimCache(PXR_NS::UsdStageRefPtr stage);
    ~SimCache();

    // Null until a simulation has started.
    PXR_NS::SdfLayerHandle layer() const { return m_layer; }

    // Makes the layer (or puts it back in the session layer's sublayers) and the ops of
    // every dynamic body in `scene`, with nothing sampled yet.
    void prepare(const PhysicsSceneDesc& scene);
    // The dynamic bodies' poses (from PhysicsWorld::readPoses()) at `time`, in one change.
    void write(double time, const PhysicsSceneDesc& scene, const std::vector<Pose>& poses);
    // Empties it; it stays in the layer stack.
    void clear();
    // The frames it holds, first to last; nothing when empty.
    std::optional<TimeRange> range() const { return m_range; }

private:
    PXR_NS::UsdStageRefPtr m_stage;
    PXR_NS::SdfLayerRefPtr m_layer;
    std::optional<TimeRange> m_range;
};

}  // namespace usdcc::usd::physics
