#include "usdcc/usd/scene_bounds.h"

#include <pxr/base/gf/bbox3d.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <cmath>

namespace usdcc::usd {

namespace {
bool isFinite(const PXR_NS::GfVec3d& v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
}  // namespace

std::optional<PXR_NS::GfRange3d> sceneBounds(const PXR_NS::UsdStagePtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
                                             PXR_NS::UsdTimeCode time, const PXR_NS::TfTokenVector& purposes) {
    if (!stage) return std::nullopt;
    // The purposes the viewport draws (by default UsdImagingGLRenderParams shows proxies,
    // not render or guide geometry). Invisible prims have no bounds in the cache.
    const PXR_NS::TfTokenVector drawn =
        purposes.empty() ? PXR_NS::TfTokenVector{PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->proxy} : purposes;
    PXR_NS::UsdGeomBBoxCache cache(time, drawn, /*useExtentsHint=*/true);

    std::vector<PXR_NS::UsdPrim> prims;
    if (paths.empty()) {
        prims.push_back(stage->GetPseudoRoot());
    } else {
        for (const PXR_NS::SdfPath& path : paths) {
            if (const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path.GetPrimPath())) prims.push_back(prim);
        }
    }

    PXR_NS::GfRange3d bounds;  // empty
    for (const PXR_NS::UsdPrim& prim : prims) {
        // The cache leaves out invisible descendants, not an invisible prim asked for itself.
        if (const PXR_NS::UsdGeomImageable imageable{prim};
            imageable && imageable.ComputeVisibility(time) == PXR_NS::UsdGeomTokens->invisible) {
            continue;
        }
        const PXR_NS::GfRange3d range = cache.ComputeWorldBound(prim).ComputeAlignedRange();
        if (range.IsEmpty() || !isFinite(range.GetMin()) || !isFinite(range.GetMax())) continue;
        bounds.UnionWith(range);
    }
    if (bounds.IsEmpty()) return std::nullopt;
    return bounds;
}

}  // namespace usdcc::usd
