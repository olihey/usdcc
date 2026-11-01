#include "usdcc/usd/stage.h"

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdUtils/stageCache.h>

namespace usdcc::usd {

bool StageChanges::affectsPrim(const PXR_NS::SdfPath& primPath) const {
    for (const auto& path : changedInfoOnlyPaths) {
        if (path.GetPrimPath() == primPath) {
            return true;
        }
    }
    for (const auto& path : resyncedPaths) {
        if (primPath.HasPrefix(path) || path.GetPrimPath() == primPath) {
            return true;
        }
    }
    return false;
}

bool StageChanges::resyncsPrim(const PXR_NS::SdfPath& primPath) const {
    for (const auto& path : resyncedPaths) {
        if (primPath.HasPrefix(path) || (path.IsPropertyPath() && path.GetPrimPath() == primPath)) {
            return true;
        }
    }
    return false;
}

// TfNotice needs a TfWeakBase-derived listener; this keeps that (and the
// notice headers) out of stage.h.
class Stage::ChangeListener : public PXR_NS::TfWeakBase {
public:
    ChangeListener(Stage& owner) : m_owner(owner) {
        m_key = PXR_NS::TfNotice::Register(PXR_NS::TfCreateWeakPtr(this), &ChangeListener::onObjectsChanged,
                                           PXR_NS::UsdStagePtr(owner.usdStage()));
    }
    ~ChangeListener() { PXR_NS::TfNotice::Revoke(m_key); }

private:
    void onObjectsChanged(const PXR_NS::UsdNotice::ObjectsChanged& notice, const PXR_NS::UsdStageWeakPtr&) {
        StageChanges changes;
        for (const auto& path : notice.GetResyncedPaths()) {
            changes.resyncedPaths.push_back(path);
        }
        for (const auto& path : notice.GetChangedInfoOnlyPaths()) {
            changes.changedInfoOnlyPaths.push_back(path);
        }
        m_owner.changed(changes);
    }

    Stage& m_owner;
    PXR_NS::TfNotice::Key m_key;
};

Stage::Stage(PXR_NS::UsdStageRefPtr usdStage)
    : m_usdStage(std::move(usdStage)),
      m_cacheId(PXR_NS::UsdUtilsStageCache::Get().GetId(m_usdStage).ToLongInt()),
      m_changeListener(std::make_unique<ChangeListener>(*this)) {}

Stage::~Stage() = default;

}  // namespace usdcc::usd
