#include "usdcc/usd/stage.h"

#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

namespace usdcc::usd {

Stage::Stage(PXR_NS::UsdStageRefPtr usdStage)
    : m_usdStage(std::move(usdStage)),
      m_cacheId(PXR_NS::UsdUtilsStageCache::Get().GetId(m_usdStage).ToLongInt()) {}

}  // namespace usdcc::usd
