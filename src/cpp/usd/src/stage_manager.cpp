#include "usdcc/usd/stage_manager.h"

#include "usdcc/core/log.h"

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <algorithm>

namespace usdcc::usd {

StageRefPtr StageManager::openStage(const std::string& identifier) {
    return adoptStage(loadUsdStage(identifier));
}

PXR_NS::UsdStageRefPtr StageManager::loadUsdStage(const std::string& identifier, std::vector<std::string>* errors,
                                                  PXR_NS::UsdStage::InitialLoadSet load) {
    PXR_NS::UsdStageRefPtr stage;
    std::string firstError;
    {
        // Only to read the errors: the mark isn't cleared, so they still
        // reach the log (TfDiagnosticBridge) when it goes out of scope.
        PXR_NS::TfErrorMark mark;
        stage = PXR_NS::UsdStage::Open(identifier, load);
        for (auto it = mark.GetBegin(); it != PXR_NS::TfDiagnosticMgr::GetInstance().GetErrorEnd(); ++it) {
            if (firstError.empty()) firstError = it->GetCommentary();
            if (errors) errors->push_back(it->GetCommentary());
        }
    }
    if (!stage) {
        if (firstError.empty()) {
            USDCC_LOG_ERROR("usdcc.stage", "Could not open stage {}", identifier);
        } else {
            USDCC_LOG_ERROR("usdcc.stage", "Could not open stage {}: {}", identifier, firstError);
        }
    }
    return stage;
}

StageRefPtr StageManager::adoptStage(const PXR_NS::UsdStageRefPtr& usdStage) {
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
        stage = registerStage(usdStage);
    } else if (!m_currentStage) {
        setCurrentStage(stage);
    }

    return stage;
}

StageRefPtr StageManager::createStage() { return registerStage(PXR_NS::UsdStage::CreateInMemory()); }

StageRefPtr StageManager::createStageFile(const std::string& path, std::string* error) {
    const auto fail = [&](const std::string& why) -> StageRefPtr {
        USDCC_LOG_ERROR("usdcc.stage", "Could not create stage {}: {}", path, why);
        if (error) *error = why;
        return nullptr;
    };
    // USD refuses this too, but in terms of layer identifiers.
    if (PXR_NS::SdfLayer::Find(path)) return fail("That file is open already. Close it first.");
    PXR_NS::UsdStageRefPtr stage;
    std::string firstError;
    {
        // Only to read the errors: the mark isn't cleared, so they still reach the
        // log (TfDiagnosticBridge) when it goes out of scope.
        PXR_NS::TfErrorMark mark;
        stage = PXR_NS::UsdStage::CreateNew(path);
        for (auto it = mark.GetBegin(); it != PXR_NS::TfDiagnosticMgr::GetInstance().GetErrorEnd(); ++it) {
            if (firstError.empty()) firstError = it->GetCommentary();
        }
    }
    if (!stage) return fail(firstError.empty() ? "USD couldn't create it." : firstError);
    return registerStage(stage);
}

StageRefPtr StageManager::registerStage(const PXR_NS::UsdStageRefPtr& usdStage) {
    PXR_NS::UsdUtilsStageCache::Get().Insert(usdStage);
    auto stage = std::make_shared<Stage>(usdStage);
    m_stages.push_back(stage);
    // The selection follows a prim that changes path. By the stage's raw pointer, as
    // the stage owns the signal and shouldn't be kept alive by its own callback.
    m_primsMovedConnections[stage.get()] = stage->primsMoved.connect([this, raw = stage.get()](const PrimMoves& moves) {
        const auto it = std::find_if(m_stages.begin(), m_stages.end(), [raw](const StageRefPtr& s) { return s.get() == raw; });
        if (it == m_stages.end()) return;
        const StageRefPtr moved = *it;
        std::vector<PXR_NS::SdfPath> paths = selectedPaths(moved);
        bool changed = false;
        for (PXR_NS::SdfPath& path : paths) {
            const PXR_NS::SdfPath after = pathAfterMoves(path, moves);
            changed = changed || after != path;
            path = after;
        }
        if (changed) setSelectedPaths(moved, std::move(paths));
    });
    USDCC_LOG_INFO("usdcc.stage", "Opened stage {}", usdStage->GetRootLayer()->GetIdentifier());
    stageOpened(stage);

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
    m_primsMovedConnections.erase(stage.get());
    m_stages.erase(it);
    USDCC_LOG_INFO("usdcc.stage", "Closed stage {}", stage->usdStage()->GetRootLayer()->GetIdentifier());
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
    // A tool target belongs to one prim: selecting another prim drops it.
    if (paths.empty() || paths.front() != stage->targetPrim()) {
        stage->clearTargetXformOp();
    }
    selectionChanged(stage, std::move(paths));
}

}  // namespace usdcc::usd
