#include "usdcc/usd/stage.h"

#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageCache.h>
#include <pxr/usd/usd/variantSets.h>
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

bool editTargetReaches(const PXR_NS::UsdPrim& prim) {
    if (!prim) return false;
    const PXR_NS::UsdEditTarget target = prim.GetStage()->GetEditTarget();
    return target.GetMapFunction().IsIdentity() || target.MapToSpecPath(prim.GetPath()).ContainsPrimVariantSelection();
}

void removeEmptyOvers(const PXR_NS::SdfLayerHandle& layer, PXR_NS::SdfPath specPath) {
    if (!layer) return;
    while (specPath.IsPrimPath() && !specPath.IsAbsoluteRootPath() && !specPath.IsPrimVariantSelectionPath()) {
        const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(specPath);
        if (!spec) return;
        if (spec->GetSpecifier() != PXR_NS::SdfSpecifierOver || !spec->GetTypeName().IsEmpty() || !spec->IsInert()) return;
        const PXR_NS::SdfPath parentPath = specPath.GetParentPath();
        const PXR_NS::SdfPrimSpecHandle parent = layer->GetPrimAtPath(parentPath);
        if (!parent) return;
        parent->RemoveNameChild(spec);
        specPath = parentPath;
    }
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
        // Someone else moved the target: the mode is over.
        if (m_owner.m_variantEdit && !m_owner.m_changingVariantTarget) m_owner.leaveVariantEdit(false);
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
    // The same layer with a variant target is still a change: it leaves the variant.
    const PXR_NS::UsdEditTarget target = m_usdStage->GetEditTarget();
    if (layer == target.GetLayer() && target.GetMapFunction().IsIdentity()) return true;
    m_usdStage->SetEditTarget(PXR_NS::UsdEditTarget(layer));  // sends StageEditTargetChanged
    return true;
}

bool Stage::isEditingVariantSet(const PXR_NS::SdfPath& prim, const std::string& set) const {
    return m_variantEdit && m_variantEdit->prim == prim && m_variantEdit->set == set;
}

bool Stage::enterVariantEdit(const PXR_NS::SdfPath& primPath, const std::string& set, const std::string& variant) {
    const PXR_NS::UsdPrim prim = m_usdStage->GetPrimAtPath(primPath);
    if (!prim || prim.IsInstanceProxy() || !prim.GetVariantSets().HasVariantSet(set)) return false;
    if (m_variantEdit && !isEditingVariantSet(primPath, set)) return false;
    const PXR_NS::UsdVariantSet variantSet = prim.GetVariantSet(set);
    if (!variantSet.HasAuthoredVariant(variant)) return false;
    if (m_variantEdit && m_variantEdit->variant == variant) return true;

    const PXR_NS::SdfLayerHandle session = m_usdStage->GetSessionLayer();
    if (!session) return false;
    const PXR_NS::SdfLayerHandle layer = m_variantEdit ? m_variantEditLayer : editTargetLayer();
    if (!m_variantEdit) {
        m_sessionSelectionBefore.reset();
        if (const PXR_NS::SdfPrimSpecHandle spec = session->GetPrimAtPath(primPath)) {
            const auto selections = spec->GetVariantSelections();
            if (const auto it = selections.find(set); it != selections.end()) m_sessionSelectionBefore = (*it).second;
        }
    }
    const PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(session, primPath);
    if (!spec) return false;
    spec->SetVariantSelection(set, variant);

    // GetVariantEditTarget() takes the selection as composed, which is why it's set first.
    m_variantEdit = VariantEdit{primPath, set, variant};
    m_variantEditLayer = layer;
    m_changingVariantTarget = true;
    // From the plain layer: under a variant target it would nest in that variant.
    if (!m_usdStage->GetEditTarget().GetMapFunction().IsIdentity()) m_usdStage->SetEditTarget(PXR_NS::UsdEditTarget(layer));
    m_usdStage->SetEditTarget(m_usdStage->GetPrimAtPath(primPath).GetVariantSet(set).GetVariantEditTarget(layer));
    m_changingVariantTarget = false;
    variantEditChanged();
    return true;
}

void Stage::exitVariantEdit() {
    if (m_variantEdit) leaveVariantEdit(true);
}

void Stage::leaveVariantEdit(bool resetTarget) {
    const VariantEdit edit = *m_variantEdit;
    const PXR_NS::SdfLayerHandle layer = m_variantEditLayer;
    m_variantEdit.reset();
    m_variantEditLayer = {};
    if (resetTarget && layer) {
        m_changingVariantTarget = true;
        m_usdStage->SetEditTarget(PXR_NS::UsdEditTarget(layer));
        m_changingVariantTarget = false;
    }
    const PXR_NS::SdfLayerHandle session = m_usdStage->GetSessionLayer();
    if (const PXR_NS::SdfPrimSpecHandle spec = session ? session->GetPrimAtPath(edit.prim) : PXR_NS::SdfPrimSpecHandle()) {
        spec->SetVariantSelection(edit.set, m_sessionSelectionBefore.value_or(std::string()));
        removeEmptyOvers(session, edit.prim);
    }
    m_sessionSelectionBefore.reset();
    variantEditChanged();
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
