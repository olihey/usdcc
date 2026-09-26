#pragma once

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <memory>
#include <vector>

namespace usdcc::usd {

// Wraps a UsdStageRefPtr together with usdcc-specific, per-stage state that
// doesn't belong on USD's own Stage object — currently just the prim
// selection (see docs/PLAN.md milestone M4/§6 item 21), but the point of
// this class is to have a single place to grow that: e.g. a per-stage
// render setting would live here too, rather than in another StageManager-
// side map keyed by cache id.
//
// Always held as a StageRefPtr (shared_ptr<Stage>), never a bare Stage*:
// StageManager owns the canonical list, but — exactly like a
// UsdStageRefPtr itself — anyone holding a copy (e.g. a ViewPanel's "which
// stage am I showing" member) keeps both this wrapper and the underlying
// UsdStage alive even after StageManager::closeStage() removes it from the
// manager's own list. See StageManager::closeStage()'s comment.
class Stage {
public:
    // `usdStage` must already be registered in UsdUtilsStageCache::Get()
    // (StageManager::openStage() does this before constructing a Stage) —
    // cacheId() is looked up once here, at construction, rather than on
    // every call.
    explicit Stage(PXR_NS::UsdStageRefPtr usdStage);

    const PXR_NS::UsdStageRefPtr& usdStage() const { return m_usdStage; }

    // The UsdUtilsStageCache::Get() id this stage was registered under when
    // opened (computed once, at construction) — see
    // StageManager::stageCacheIds() for why this crosses into Python
    // instead of the stage itself.
    long cacheId() const { return m_cacheId; }

    const std::vector<PXR_NS::SdfPath>& selectedPaths() const { return m_selectedPaths; }
    void setSelectedPaths(std::vector<PXR_NS::SdfPath> paths) { m_selectedPaths = std::move(paths); }

private:
    PXR_NS::UsdStageRefPtr m_usdStage;
    long m_cacheId;
    std::vector<PXR_NS::SdfPath> m_selectedPaths;
};

using StageRefPtr = std::shared_ptr<Stage>;

}  // namespace usdcc::usd
