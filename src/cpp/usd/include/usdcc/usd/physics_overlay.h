#pragma once

// The viewport's collider overlay (docs/PHYSICS.md §5.1): the stage's colliders as the
// simulation sees them (a convex hull where a mesh is simulated as one, a box for a
// bounding cube) and its joints' frames, as world-space lines the viewport draws over the
// render, the way it draws a gizmo. Dynamic bodies green, kinematic blue, static orange.

#include "usdcc/tools/tool.h"

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/timeCode.h>

#include <optional>

namespace usdcc::usd {

// The lines for `stage` at `time`, with the simulation cache's poses when it has some.
usdcc::tools::GizmoGeometry buildPhysicsOverlay(const PXR_NS::UsdStageRefPtr& stage, PXR_NS::UsdTimeCode time);

// The same, kept until the stage changes or another time is asked for: a viewport redraws
// far more often than either.
class PhysicsOverlay : public PXR_NS::TfWeakBase {
public:
    PhysicsOverlay();
    ~PhysicsOverlay();
    const usdcc::tools::GizmoGeometry& geometry(const PXR_NS::UsdStageRefPtr& stage, PXR_NS::UsdTimeCode time);

private:
    void onChanged(const PXR_NS::UsdNotice::ObjectsChanged& notice, const PXR_NS::UsdStageWeakPtr& sender);

    PXR_NS::UsdStageRefPtr m_stage;
    std::optional<double> m_time;
    bool m_dirty = true;
    PXR_NS::TfNotice::Key m_key;
    usdcc::tools::GizmoGeometry m_geometry;
};

}  // namespace usdcc::usd
