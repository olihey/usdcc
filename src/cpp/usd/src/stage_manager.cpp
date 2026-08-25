#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <algorithm>

namespace usdcc::usd {

StageManager::StageManager(QObject* parent) : QObject(parent) {}

PXR_NS::UsdStageRefPtr StageManager::openStage(const QString& identifier) {
    auto stage = PXR_NS::UsdStage::Open(identifier.toStdString());
    if (!stage) {
        return nullptr;
    }

    if (std::find(m_stages.begin(), m_stages.end(), stage) == m_stages.end()) {
        PXR_NS::UsdUtilsStageCache::Get().Insert(stage);
        m_stages.push_back(stage);
        emit stageOpened(stage);
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

    PXR_NS::UsdUtilsStageCache::Get().Erase(stage);
    m_stages.erase(it);
    emit stageClosed(stage);

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
    emit currentStageChanged(stage);
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

}  // namespace usdcc::usd
