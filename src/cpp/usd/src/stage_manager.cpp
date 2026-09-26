#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <algorithm>

namespace usdcc::usd {

StageRefPtr StageManager::openStage(const std::string& identifier) {
    auto usdStage = PXR_NS::UsdStage::Open(identifier);
    if (!usdStage) {
        return nullptr;
    }

    StageRefPtr stage;
    for (const auto& existing : m_stages) {
        if (existing->usdStage() == usdStage) {
            stage = existing;
            break;
        }
    }

    if (!stage) {
        PXR_NS::UsdUtilsStageCache::Get().Insert(usdStage);
        stage = std::make_shared<Stage>(usdStage);
        m_stages.push_back(stage);
        stageOpened(stage);
    }

    if (!m_currentStage) {
        setCurrentStage(stage);
    }

    return stage;
}

void StageManager::closeStage(const StageRefPtr& stage) {
    auto it = std::find(m_stages.begin(), m_stages.end(), stage);
    if (it == m_stages.end()) {
        return;
    }

    PXR_NS::UsdUtilsStageCache::Get().Erase(stage->usdStage());
    m_stages.erase(it);
    stageClosed(stage);

    if (m_currentStage == stage) {
        setCurrentStage(m_stages.empty() ? StageRefPtr() : m_stages.front());
    }
}

const std::vector<StageRefPtr>& StageManager::stages() const { return m_stages; }

StageRefPtr StageManager::currentStage() const { return m_currentStage; }

void StageManager::setCurrentStage(const StageRefPtr& stage) {
    if (m_currentStage == stage) {
        return;
    }
    m_currentStage = stage;
    currentStageChanged(stage);
}

std::vector<long> StageManager::stageCacheIds() const {
    std::vector<long> ids;
    ids.reserve(m_stages.size());
    for (const auto& stage : m_stages) {
        ids.push_back(stage->cacheId());
    }
    return ids;
}

StageRefPtr StageManager::findByCacheId(long cacheId) const {
    for (const auto& stage : m_stages) {
        if (stage->cacheId() == cacheId) {
            return stage;
        }
    }
    return nullptr;
}

std::vector<PXR_NS::SdfPath> StageManager::selectedPaths(const StageRefPtr& stage) const {
    return stage ? stage->selectedPaths() : std::vector<PXR_NS::SdfPath>{};
}

void StageManager::setSelectedPaths(const StageRefPtr& stage, std::vector<PXR_NS::SdfPath> paths) {
    if (!stage) {
        return;
    }
    stage->setSelectedPaths(paths);
    selectionChanged(stage, std::move(paths));
}

}  // namespace usdcc::usd
