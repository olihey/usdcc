#pragma once

// The stages being shared and their live layers (docs/SHARED_STAGE.md §2), for the
// rules sharing brings to code that knows nothing else of it: the move tools ask
// whether a prim may be renamed or reparented.

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QString>

namespace usdcc::usd::share {

USDCC_USD_UI_API void registerLiveLayer(const PXR_NS::UsdStageRefPtr& stage, const PXR_NS::SdfLayerHandle& live);
USDCC_USD_UI_API void unregisterLiveLayer(const PXR_NS::UsdStageRefPtr& stage);
// The stage's live layer while it is shared; null otherwise.
USDCC_USD_UI_API PXR_NS::SdfLayerHandle liveLayerOf(const PXR_NS::UsdStageRefPtr& stage);

// Whether `moves` may be made on `stage`: on a shared stage only prims made in its
// live layer move, since moving one from a layer below would rewrite that layer.
// False with why in `why`.
USDCC_USD_UI_API bool movesAllowed(const PXR_NS::UsdStageRefPtr& stage, const PrimMoves& moves, QString* why);

}  // namespace usdcc::usd::share
