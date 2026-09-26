#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <algorithm>

namespace usdcc::usd {

PXR_NS::UsdStageRefPtr StageManager::openStage(const std::string& identifier) {
    auto stage = PXR_NS::UsdStage::Open(identifier);
    if (!stage) {
        return nullptr;
    }

    if (std::find(m_stages.begin(), m_stages.end(), stage) == m_stages.end()) {
        PXR_NS::UsdUtilsStageCache::Get().Insert(stage);
        m_stages.push_back(stage);
        stageOpened(stage);
    }

    if (!m_currentStage) {
        setCurrentStage(stage);
    }

    return stage;
}

void StageManager::closeStage(const PXR_NS::UsdStageRefPtr& stage) {
    auto it = std::find(m_stages.begin(), m_stages.end(), stage);
    if (it == m_stages.end()) {
        return;
    }

    // Captured before Erase(): stageCacheId() looks the id up in the stage
    // cache, which is no longer possible once the stage is removed from it.
    const long cacheId = stageCacheId(stage);
    PXR_NS::UsdUtilsStageCache::Get().Erase(stage);
    m_stages.erase(it);
    m_selections.erase(cacheId);
    stageClosed(stage);

    if (m_currentStage == stage) {
        setCurrentStage(m_stages.empty() ? PXR_NS::UsdStageRefPtr() : m_stages.front());
    }
}

const std::vector<PXR_NS::UsdStageRefPtr>& StageManager::stages() const { return m_stages; }

PXR_NS::UsdStageRefPtr StageManager::currentStage() const { return m_currentStage; }

void StageManager::setCurrentStage(const PXR_NS::UsdStageRefPtr& stage) {
    if (m_currentStage == stage) {
        return;
    }
    m_currentStage = stage;
    currentStageChanged(stage);
}

long StageManager::stageCacheId(const PXR_NS::UsdStageRefPtr& stage) const {
    return PXR_NS::UsdUtilsStageCache::Get().GetId(stage).ToLongInt();
}

std::vector<long> StageManager::stageCacheIds() const {
    std::vector<long> ids;
    ids.reserve(m_stages.size());
    for (const auto& stage : m_stages) {
        ids.push_back(stageCacheId(stage));
    }
    return ids;
}

PXR_NS::UsdStageRefPtr StageManager::findByCacheId(long cacheId) const {
    auto stage = PXR_NS::UsdUtilsStageCache::Get().Find(PXR_NS::UsdStageCache::Id::FromLongInt(cacheId));
    if (std::find(m_stages.begin(), m_stages.end(), stage) == m_stages.end()) {
        return nullptr;
    }
    return stage;
}

std::vector<PXR_NS::SdfPath> StageManager::selectedPaths(const PXR_NS::UsdStageRefPtr& stage) const {
    if (!stage) {
        return {};
    }
    auto it = m_selections.find(stageCacheId(stage));
    return it != m_selections.end() ? it->second : std::vector<PXR_NS::SdfPath>{};
}

void StageManager::setSelectedPaths(const PXR_NS::UsdStageRefPtr& stage, std::vector<PXR_NS::SdfPath> paths) {
    if (!stage) {
        return;
    }
    m_selections[stageCacheId(stage)] = paths;
    selectionChanged(stage, paths);
}

}  // namespace usdcc::usd
