#pragma once

#include <pxr/base/gf/range3d.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/timeCode.h>

#include <optional>
#include <vector>

namespace usdcc::usd {

// The world-space box around what the viewport draws of `paths`' prims (the geometry of
// `purposes`, visible prims only), at `time`; around the whole stage when `paths` is
// empty. No purposes: default and proxy, what a viewport shows unless told otherwise
// (viewportPurposes()). What Fit to View frames (HydraViewportWindow::frameBox()). Prims
// with no bounds (a scope, a material, an invisible mesh, a missing path) add nothing;
// nullopt when none of them has any. Qt-free.
std::optional<PXR_NS::GfRange3d> sceneBounds(const PXR_NS::UsdStagePtr& stage,
                                             const std::vector<PXR_NS::SdfPath>& paths,
                                             PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default(),
                                             const PXR_NS::TfTokenVector& purposes = {});

}  // namespace usdcc::usd
