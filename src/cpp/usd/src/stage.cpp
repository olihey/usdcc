#include "usdcc/usd/stage.h"

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <algorithm>

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
        m_editTargetKey = PXR_NS::TfNotice::Register(PXR_NS::TfCreateWeakPtr(this), &ChangeListener::onEditTargetChanged,
                                                     PXR_NS::UsdStagePtr(owner.usdStage()));
    }
    ~ChangeListener() {
        PXR_NS::TfNotice::Revoke(m_key);
        PXR_NS::TfNotice::Revoke(m_editTargetKey);
    }

private:
    void onEditTargetChanged(const PXR_NS::UsdNotice::StageEditTargetChanged&, const PXR_NS::UsdStageWeakPtr&) {
        m_owner.editTargetChanged();
    }

    void onObjectsChanged(const PXR_NS::UsdNotice::ObjectsChanged& notice, const PXR_NS::UsdStageWeakPtr&) {
        StageChanges changes;
        for (const auto& path : notice.GetResyncedPaths()) {
            changes.resyncedPaths.push_back(path);
        }
        for (const auto& path : notice.GetChangedInfoOnlyPaths()) {
            changes.changedInfoOnlyPaths.push_back(path);
        }
        // Drop a tool target whose op is no longer in the stack, before
        // anyone reacts to the change.
        if (!m_owner.m_targetOp.IsEmpty() && changes.affectsPrim(m_owner.m_targetPrim)) {
            m_owner.validateTarget();
        }
        m_owner.changed(changes);
    }

    Stage& m_owner;
    PXR_NS::TfNotice::Key m_key;
    PXR_NS::TfNotice::Key m_editTargetKey;
};

Stage::Stage(PXR_NS::UsdStageRefPtr usdStage)
    : m_usdStage(std::move(usdStage)),
      m_cacheId(PXR_NS::UsdUtilsStageCache::Get().GetId(m_usdStage).ToLongInt()),
      m_changeListener(std::make_unique<ChangeListener>(*this)) {}

Stage::~Stage() = default;

PXR_NS::TfToken Stage::targetXformOpFor(const PXR_NS::SdfPath& primPath) const {
    return primPath == m_targetPrim ? m_targetOp : PXR_NS::TfToken();
}

void Stage::setTargetXformOp(const PXR_NS::SdfPath& primPath, const PXR_NS::TfToken& opName) {
    const PXR_NS::SdfPath prim = opName.IsEmpty() ? PXR_NS::SdfPath() : primPath;
    if (prim == m_targetPrim && opName == m_targetOp) return;
    m_targetPrim = prim;
    m_targetOp = opName;
    targetXformOpChanged();
}

PXR_NS::SdfLayerHandle Stage::editTargetLayer() const { return m_usdStage->GetEditTarget().GetLayer(); }

bool Stage::setEditTargetLayer(const PXR_NS::SdfLayerHandle& layer) {
    if (!layer || m_usdStage->IsLayerMuted(layer->GetIdentifier())) return false;
    if (!m_usdStage->HasLocalLayer(layer)) return false;  // not in the layer stack
    if (layer == editTargetLayer()) return true;
    m_usdStage->SetEditTarget(PXR_NS::UsdEditTarget(layer));  // sends StageEditTargetChanged
    return true;
}

void Stage::validateTarget() {
    const PXR_NS::UsdPrim prim = m_usdStage->GetPrimAtPath(m_targetPrim);
    PXR_NS::VtTokenArray order;
    const PXR_NS::UsdGeomXformable xformable(prim);
    if (xformable) xformable.GetXformOpOrderAttr().Get(&order);
    if (std::find(order.begin(), order.end(), m_targetOp) == order.end()) {
        clearTargetXformOp();
    }
}

}  // namespace usdcc::usd
