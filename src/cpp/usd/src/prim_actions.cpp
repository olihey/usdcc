#include "usdcc/usd/prim_actions.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/applied_schemas.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/xform_stack.h"

#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/inherits.h>
#include <pxr/usd/usd/namespaceEditor.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/specializes.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageLoadRules.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QCoreApplication>
#include <QStringList>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace usdcc::usd::prim_actions {

namespace {
QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::prim_actions", text); }

QString qs(const std::string& text) { return QString::fromStdString(text); }

// "Deactivate Chair", or "Deactivate 3 Prims".
QString describe(const QString& verb, const std::vector<PXR_NS::SdfPath>& paths) {
    if (paths.size() == 1) return QStringLiteral("%1 %2").arg(verb, qs(paths.front().GetName()));
    return tr("%1 %2 Prims").arg(verb).arg(paths.size());
}

// The prims at `paths` that can be edited.
std::vector<PXR_NS::UsdPrim> editablePrims(const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths) {
    std::vector<PXR_NS::UsdPrim> prims;
    for (const PXR_NS::SdfPath& path : paths) {
        const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
        if (prim && isEditable(prim)) prims.push_back(prim);
    }
    return prims;
}

// Load and unload change the stage's load rules, not a layer: this undo step
// puts the rules back as they were.
class LoadRulesCommand : public QUndoCommand {
public:
    LoadRulesCommand(PXR_NS::UsdStageRefPtr stage, const QString& description,
                     std::function<void(const PXR_NS::UsdStageRefPtr&)> change)
        : QUndoCommand(description),
          m_stage(std::move(stage)),
          m_change(std::move(change)),
          m_before(m_stage->GetLoadRules()) {}

    void redo() override {
        if (m_change) {
            m_change(m_stage);  // the first time: the load or unload itself
            m_change = nullptr;
            m_after = m_stage->GetLoadRules();
        } else {
            m_stage->SetLoadRules(m_after);
        }
    }

    void undo() override { m_stage->SetLoadRules(m_before); }

private:
    PXR_NS::UsdStageRefPtr m_stage;
    std::function<void(const PXR_NS::UsdStageRefPtr&)> m_change;
    PXR_NS::UsdStageLoadRules m_before;
    PXR_NS::UsdStageLoadRules m_after;
};

// The entry of a list op that `matches`, in whichever part of it it is;
// false when there's none.
template <typename Proxy, typename Item, typename Matches>
bool findItem(const Proxy& list, Matches matches, Item* found) {
    auto search = [&](const auto& part) {
        for (const auto& item : part) {
            if (matches(item)) {
                *found = item;
                return true;
            }
        }
        return false;
    };
    return search(list.GetExplicitItems()) || search(list.GetPrependedItems()) || search(list.GetAddedItems()) ||
           search(list.GetAppendedItems());
}

// A reference's or payload's fields as `arc` has them.
template <typename Item>
bool sameAsset(const Item& item, const ArcInfo& arc) {
    return item.GetAssetPath() == arc.assetPath && item.GetPrimPath() == arc.primPath &&
           item.GetLayerOffset() == arc.layerOffset;
}

template <typename Item>
Item withFields(Item item, const ArcInfo& changed) {
    item.SetAssetPath(changed.assetPath);
    item.SetPrimPath(changed.primPath);
    item.SetLayerOffset(changed.layerOffset);
    return item;
}

// Replaces (`changed`) or removes (null) the entry `arc` describes in the
// edit target's prim spec.
bool editArcEntry(const PXR_NS::UsdStageRefPtr& usdStage, const PXR_NS::SdfPath& path, const ArcInfo& arc,
                  const ArcInfo* changed) {
    const PXR_NS::SdfPrimSpecHandle spec = usdStage->GetEditTarget().GetPrimSpecForScenePath(path);
    if (!spec) return false;
    switch (arc.type) {
        case ArcInfo::Type::Reference: {
            PXR_NS::SdfReferencesProxy list = spec->GetReferenceList();
            PXR_NS::SdfReference item;
            if (!findItem(list, [&](const PXR_NS::SdfReference& r) { return sameAsset(r, arc); }, &item)) return false;
            if (!changed) return list.RemoveItemEdits(item), true;
            list.ReplaceItemEdits(item, withFields(item, *changed));
            return true;
        }
        case ArcInfo::Type::Payload: {
            PXR_NS::SdfPayloadsProxy list = spec->GetPayloadList();
            PXR_NS::SdfPayload item;
            if (!findItem(list, [&](const PXR_NS::SdfPayload& p) { return sameAsset(p, arc); }, &item)) return false;
            if (!changed) return list.RemoveItemEdits(item), true;
            list.ReplaceItemEdits(item, withFields(item, *changed));
            return true;
        }
        case ArcInfo::Type::Inherit:
        case ArcInfo::Type::Specialize: {
            PXR_NS::SdfPathEditorProxy list =
                arc.type == ArcInfo::Type::Inherit ? spec->GetInheritPathList() : spec->GetSpecializesList();
            PXR_NS::SdfPath item;
            if (!findItem(list, [&](const PXR_NS::SdfPath& p) { return p == arc.primPath; }, &item)) return false;
            if (!changed) return list.RemoveItemEdits(item), true;
            if (!changed->primPath.IsAbsolutePath() || !changed->primPath.IsPrimPath()) return false;
            list.ReplaceItemEdits(item, changed->primPath);
            return true;
        }
        case ArcInfo::Type::Variant:
            return false;
    }
    return false;
}

// A selection on the prim's plain path, in the edit target's layer, whatever the
// target maps to: under a variant target USD would write it inside the variant,
// where it can't change what's selected. Empty clears it.
bool writeSelection(const PXR_NS::UsdStageRefPtr& usdStage, const PXR_NS::SdfPath& path, const std::string& set,
                    const std::string& variant) {
    const PXR_NS::UsdEditTarget target = usdStage->GetEditTarget();
    if (target.GetMapFunction().IsIdentity()) {
        PXR_NS::UsdVariantSet variantSet = usdStage->GetPrimAtPath(path).GetVariantSet(set);
        return variant.empty() ? variantSet.ClearVariantSelection() : variantSet.SetVariantSelection(variant);
    }
    const PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(target.GetLayer(), path);
    if (!spec) return false;
    spec->SetVariantSelection(set, variant);
    return true;
}

// Every layer a namespace edit can touch: this stage's and the other open stages'.
std::vector<PXR_NS::SdfLayerHandle> layersToRecord(const StageRefPtr& stage, const std::vector<StageRefPtr>& openStages) {
    std::vector<PXR_NS::SdfLayerHandle> layers;
    std::set<std::string> seen;
    auto add = [&](const PXR_NS::UsdStageRefPtr& usdStage) {
        for (const PXR_NS::SdfLayerHandle& layer : usdStage->GetUsedLayers(/*includeClipLayers=*/false)) {
            if (seen.insert(layer->GetIdentifier()).second) layers.push_back(layer);
        }
    };
    add(stage->usdStage());
    for (const StageRefPtr& other : openStages) add(other->usdStage());
    return layers;
}

// The names in a layer's child order lists that a moved prim leaves stale, which USD
// doesn't update: a rename keeps its place, and one moved away goes.
void tidyChildOrder(const PXR_NS::UsdStageRefPtr& usdStage, const PXR_NS::SdfPath& from, const PXR_NS::SdfPath& to) {
    for (const PXR_NS::SdfLayerHandle& layer : usdStage->GetLayerStack(/*includeSessionLayers=*/true)) {
        const PXR_NS::SdfPrimSpecHandle parent = layer->GetPrimAtPath(from.GetParentPath());
        if (!parent || !layer->PermissionToEdit() || !parent->HasNameChildrenOrder()) continue;
        const auto current = parent->GetNameChildrenOrder();
        std::vector<PXR_NS::TfToken> order(current.begin(), current.end());
        const auto it = std::find(order.begin(), order.end(), from.GetNameToken());
        if (it == order.end()) continue;
        if (to.GetParentPath() == from.GetParentPath()) {
            *it = to.GetNameToken();
        } else {
            order.erase(it);
        }
        if (order.empty()) {
            layer->EraseField(parent->GetPath(), PXR_NS::SdfFieldKeys->PrimOrder);
        } else {
            parent->SetNameChildrenOrder(order);
        }
    }
}

// In the edit target: the whole list, so it's plain what it means. Works over a
// weaker layer's children or a reference's, with an `over` if need be.
bool writeChildOrder(const PXR_NS::UsdStageRefPtr& usdStage, const PXR_NS::SdfPath& parent, const std::vector<PXR_NS::TfToken>& order) {
    const PXR_NS::UsdEditTarget target = usdStage->GetEditTarget();
    // Not OverridePrim(), which authors nothing where the prim exists, in a weaker layer say.
    const PXR_NS::SdfPrimSpecHandle spec = parent.IsAbsoluteRootPath() ? target.GetLayer()->GetPseudoRoot()
                                                                       : PXR_NS::SdfCreatePrimInLayer(target.GetLayer(), target.MapToSpecPath(parent));
    if (!spec) return false;
    spec->SetNameChildrenOrder(order);
    return true;
}

std::vector<PXR_NS::TfToken> childNames(const PXR_NS::UsdStageRefPtr& usdStage, const PXR_NS::SdfPath& parent) {
    std::vector<PXR_NS::TfToken> names;
    const PXR_NS::UsdPrim prim = usdStage->GetPrimAtPath(parent);
    if (!prim) return names;
    for (const PXR_NS::UsdPrim& child : prim.GetFilteredChildren(PXR_NS::UsdPrimAllPrimsPredicate)) names.push_back(child.GetName());
    return names;
}

// The namespace edits, in one undo step, with whatever else goes in it, and what
// follows the prims announced now and on undo and redo. An editor holds one edit: with
// two moves declared it applies only the last, and says it applied them.
bool applyMoves(UndoManager& undo, const StageRefPtr& stage, const PrimMoves& moves, const QString& description,
                const std::vector<StageRefPtr>& openStages, const std::function<bool()>& also, QString* why) {
    const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
    PrimMoves inverse;
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) inverse.emplace_back(it->second, it->first);
    // The tool's target op: USD's change notice drops it when its prim goes.
    const PXR_NS::SdfPath targetPrim = stage->targetPrim();
    const PXR_NS::TfToken targetOp = stage->targetXformOp();
    const std::function<void(bool)> announce = [stage = std::weak_ptr<Stage>(stage), moves, inverse, targetPrim, targetOp](bool undone) {
        const StageRefPtr alive = stage.lock();
        if (!alive) return;
        alive->primsMoved(undone ? inverse : moves);
        if (!targetOp.IsEmpty()) alive->setTargetXformOp(undone ? targetPrim : pathAfterMoves(targetPrim, moves), targetOp);
    };
    const bool done = undo.applyAcrossLayers(
        stage, layersToRecord(stage, openStages), description,
        [&]() {
            // C++ gets USD's verification failures as posted errors, not exceptions.
            PXR_NS::TfErrorMark mark;
            for (const auto& [from, to] : moves) {
                PXR_NS::UsdNamespaceEditor editor(usdStage);
                for (const StageRefPtr& other : openStages) {
                    if (other != stage) editor.AddDependentStage(other->usdStage());
                }
                std::string whyNot = "USD refused it";
                if (!editor.MovePrimAtPath(from, to) || !editor.CanApplyEdits(&whyNot) || !mark.IsClean() || !editor.ApplyEdits() ||
                    !mark.IsClean()) {
                    mark.Clear();
                    if (why) *why = tr("Can't move %1: %2").arg(qs(from.GetString()), qs(whyNot));
                    return false;
                }
                tidyChildOrder(usdStage, from, to);
            }
            return !also || also();
        },
        announce);
    if (done) announce(false);
    return done;
}

// Whether a variant of an ancestor has opinions on the prim at `names`, starting from
// `specPath` at depth `depth`: the namespace editor would leave those behind. The prim's
// own variants don't count, since they move with it.
bool heldInVariant(const PXR_NS::SdfLayerHandle& layer, const PXR_NS::SdfPath& specPath,
                   const std::vector<PXR_NS::TfToken>& names, std::size_t depth, bool inVariant, PXR_NS::SdfPath* where) {
    if (depth == names.size()) return inVariant;
    const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(specPath);
    if (!spec) return false;
    const PXR_NS::SdfPath child = specPath.AppendChild(names[depth]);
    if (layer->GetPrimAtPath(child) && heldInVariant(layer, child, names, depth + 1, inVariant, where)) return true;
    const PXR_NS::SdfVariantSetsProxy sets = spec->GetVariantSets();
    for (auto set = sets.begin(); set != sets.end(); ++set) {
        const PXR_NS::SdfVariantSetSpecHandle setSpec = (*set).second;
        for (const PXR_NS::SdfVariantSpecHandle& variant : setSpec->GetVariantList()) {
            const PXR_NS::SdfPath variantPath = specPath.AppendVariantSelection(setSpec->GetName(), variant->GetName());
            if (heldInVariant(layer, variantPath, names, depth, true, where)) {
                if (where && where->IsEmpty()) *where = variantPath;
                return true;
            }
        }
    }
    return false;
}

// A prim's position in the tree, to sort by.
std::vector<int> treePosition(const PXR_NS::UsdStageRefPtr& usdStage, const PXR_NS::SdfPath& path) {
    std::vector<int> position;
    for (const PXR_NS::SdfPath& prefix : path.GetPrefixes()) {
        const std::vector<PXR_NS::TfToken> siblings = childNames(usdStage, prefix.GetParentPath());
        const auto it = std::find(siblings.begin(), siblings.end(), prefix.GetNameToken());
        position.push_back(int(it - siblings.begin()));
    }
    return position;
}

// Those not under another of them, in tree order.
std::vector<PXR_NS::SdfPath> topMost(const PXR_NS::UsdStageRefPtr& usdStage, const std::vector<PXR_NS::SdfPath>& paths) {
    std::vector<PXR_NS::SdfPath> top;
    for (const PXR_NS::SdfPath& path : paths) {
        const bool under = std::any_of(paths.begin(), paths.end(), [&](const PXR_NS::SdfPath& other) {
            return other != path && path.HasPrefix(other);
        });
        if (!under && std::find(top.begin(), top.end(), path) == top.end()) top.push_back(path);
    }
    std::map<PXR_NS::SdfPath, std::vector<int>> positions;
    for (const PXR_NS::SdfPath& path : top) positions[path] = treePosition(usdStage, path);
    std::sort(top.begin(), top.end(), [&](const PXR_NS::SdfPath& a, const PXR_NS::SdfPath& b) { return positions[a] < positions[b]; });
    return top;
}

bool isAnimated(PXR_NS::UsdPrim prim) {
    for (; prim && !prim.IsPseudoRoot(); prim = prim.GetParent()) {
        const PXR_NS::UsdGeomXformable xformable(prim);
        if (xformable && xformable.TransformMightBeTimeVarying()) return true;
    }
    return false;
}

bool pushLoadRules(UndoManager& undo, const StageRefPtr& stage, const QString& description,
                   std::function<void(const PXR_NS::UsdStageRefPtr&)> change) {
    QUndoStack* stack = undo.stack(stage);
    if (!stack) return false;
    stack->push(new LoadRulesCommand(stage->usdStage(), description, std::move(change)));
    return true;
}
}  // namespace

bool isEditable(const PXR_NS::UsdPrim& prim) {
    return prim && !prim.IsInstanceProxy() && editTargetReaches(prim);
}

bool canRename(const PXR_NS::UsdPrim& prim) {
    return isEditable(prim) && prim.GetStage()->GetEditTarget().GetMapFunction().IsIdentity();
}

bool setActive(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
               bool active) {
    std::vector<PXR_NS::UsdPrim> prims = editablePrims(stage, paths);
    prims.erase(std::remove_if(prims.begin(), prims.end(),
                               [active](const PXR_NS::UsdPrim& prim) { return prim.IsActive() == active; }),
                prims.end());
    if (prims.empty()) return false;
    std::vector<PXR_NS::SdfPath> changed;
    for (const PXR_NS::UsdPrim& prim : prims) changed.push_back(prim.GetPath());
    return undo.apply(stage, describe(active ? tr("Activate") : tr("Deactivate"), changed), [&]() {
        bool ok = true;
        // By path: activating or deactivating one prim can recompose another.
        for (const PXR_NS::SdfPath& path : changed) {
            ok = stage->usdStage()->GetPrimAtPath(path).SetActive(active) && ok;
        }
        return ok;
    });
}

bool setVisible(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
                bool visible) {
    std::vector<PXR_NS::SdfPath> imageable;
    for (const PXR_NS::UsdPrim& prim : editablePrims(stage, paths)) {
        if (PXR_NS::UsdGeomImageable(prim)) imageable.push_back(prim.GetPath());
    }
    if (imageable.empty()) return false;
    return undo.apply(stage, describe(visible ? tr("Show") : tr("Hide"), imageable), [&]() {
        for (const PXR_NS::SdfPath& path : imageable) {
            const PXR_NS::UsdGeomImageable prim(stage->usdStage()->GetPrimAtPath(path));
            visible ? prim.MakeVisible() : prim.MakeInvisible();
        }
        return true;
    });
}

bool showOnly(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths) {
    std::vector<PXR_NS::SdfPath> shown;
    for (const PXR_NS::UsdPrim& prim : editablePrims(stage, paths)) {
        if (PXR_NS::UsdGeomImageable(prim)) shown.push_back(prim.GetPath());
    }
    if (shown.empty()) return false;
    // The prims to show and their ancestors stay; everything beside them goes.
    std::set<PXR_NS::SdfPath> keep;
    for (const PXR_NS::SdfPath& path : shown) {
        for (PXR_NS::SdfPath p = path; !p.IsAbsoluteRootPath() && !p.IsEmpty(); p = p.GetParentPath()) keep.insert(p);
    }
    return undo.apply(stage, describe(tr("Show Only"), shown), [&]() {
        const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
        for (const PXR_NS::SdfPath& path : shown) PXR_NS::UsdGeomImageable(usdStage->GetPrimAtPath(path)).MakeVisible();
        for (const PXR_NS::SdfPath& path : keep) {
            const PXR_NS::UsdPrim parent = usdStage->GetPrimAtPath(path.GetParentPath());
            if (!parent) continue;
            for (const PXR_NS::UsdPrim& sibling : parent.GetChildren()) {
                if (keep.count(sibling.GetPath()) || !isEditable(sibling)) continue;
                if (const PXR_NS::UsdGeomImageable imageable{sibling}) imageable.MakeInvisible();
            }
        }
        return true;
    });
}

bool resetVisibility(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths) {
    std::vector<PXR_NS::SdfPath> authored;
    for (const PXR_NS::UsdPrim& prim : editablePrims(stage, paths)) {
        const PXR_NS::UsdGeomImageable imageable(prim);
        if (imageable && imageable.GetVisibilityAttr().HasAuthoredValue()) authored.push_back(prim.GetPath());
    }
    if (authored.empty()) return false;
    return undo.apply(stage, describe(tr("Reset Visibility of"), authored), [&]() {
        bool changed = false;
        for (const PXR_NS::SdfPath& path : authored) {
            changed = PXR_NS::UsdGeomImageable(stage->usdStage()->GetPrimAtPath(path)).GetVisibilityAttr().Clear() ||
                      changed;
        }
        return changed;
    });
}

bool setVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                const std::string& variantSet, const std::string& variant) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || !prim.GetVariantSets().HasVariantSet(variantSet)) return false;
    if (stage->isEditingVariantSet(path, variantSet)) return stage->enterVariantEdit(path, variantSet, variant);
    if (prim.GetVariantSet(variantSet).GetVariantSelection() == variant) return false;
    return undo.apply(stage,
                      tr("Set %1 to %2 on %3").arg(qs(variantSet), qs(variant), qs(path.GetName())), [&]() {
                          return writeSelection(stage->usdStage(), path, variantSet, variant);
                      });
}

bool clearVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                  const std::string& variantSet) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || !prim.GetVariantSets().HasVariantSet(variantSet)) return false;
    if (stage->isEditingVariantSet(path, variantSet)) return false;
    return undo.apply(stage, tr("Clear %1 on %2").arg(qs(variantSet), qs(path.GetName())),
                      [&]() { return writeSelection(stage->usdStage(), path, variantSet, std::string()); });
}

bool rename(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& name,
            const std::vector<StageRefPtr>& openStages) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!canRename(prim) || name.empty() || name == path.GetName() || !PXR_NS::SdfPath::IsValidIdentifier(name)) {
        return false;
    }
    const PXR_NS::SdfPath to = path.ReplaceName(PXR_NS::TfToken(name));
    if (stage->usdStage()->GetPrimAtPath(to)) return false;  // a sibling has it
    return applyMoves(undo, stage, {{path, to}}, tr("Rename %1 to %2").arg(qs(path.GetName()), qs(name)), openStages, {}, nullptr);
}

QString whyNotMove(const PXR_NS::UsdStageRefPtr& usdStage, const PXR_NS::SdfPath& path, const PXR_NS::SdfPath& newParent) {
    const PXR_NS::UsdPrim prim = usdStage->GetPrimAtPath(path);
    const QString name = qs(path.GetString());
    if (!prim || prim.IsPseudoRoot()) return tr("Can't move %1: it doesn't exist").arg(name);
    if (prim.IsInstanceProxy()) return tr("Can't move %1: it's part of an instance").arg(name);
    if (!usdStage->GetEditTarget().GetMapFunction().IsIdentity()) return tr("Can't move a prim while editing a variant: exit first");
    if (newParent.HasPrefix(path)) return tr("Can't move %1 into itself").arg(name);
    if (const QString why = whyNotCreateUnder(usdStage->GetPrimAtPath(newParent)); !why.isEmpty()) return why;
    if (path.GetParentPath() != newParent && usdStage->GetPrimAtPath(newParent.AppendChild(path.GetNameToken()))) {
        return tr("Can't move %1: %2 already has a %3").arg(name, qs(newParent.GetString()), qs(path.GetName()));
    }
    // USD leaves a muted layer alone, so its opinion would bring the old path back.
    for (const std::string& muted : usdStage->GetMutedLayers()) {
        const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(muted);
        if (layer && layer->GetPrimAtPath(path)) {
            return tr("Can't move %1: the muted layer %2 has opinions on it, which would bring it back").arg(name, qs(layer->GetDisplayName()));
        }
    }
    std::vector<PXR_NS::TfToken> names;
    for (const PXR_NS::SdfPath& prefix : path.GetPrefixes()) names.push_back(prefix.GetNameToken());
    for (const PXR_NS::SdfLayerHandle& layer : usdStage->GetLayerStack(/*includeSessionLayers=*/true)) {
        if (layer->GetPrimAtPath(path) && !layer->PermissionToEdit()) {
            return tr("Can't move %1: the layer %2 can't be edited").arg(name, qs(layer->GetDisplayName()));
        }
        PXR_NS::SdfPath where;
        if (heldInVariant(layer, PXR_NS::SdfPath::AbsoluteRootPath(), names, 0, false, &where)) {
            return tr("Can't move %1: the variant %2 has opinions on it, which would stay behind").arg(name, qs(where.GetString()));
        }
    }
    return {};
}

QString whyNotDrop(const PXR_NS::UsdStageRefPtr& usdStage, const std::vector<PXR_NS::SdfPath>& paths, const PXR_NS::SdfPath& newParent) {
    if (paths.empty()) return tr("Nothing to move");
    std::set<PXR_NS::TfToken> arriving;
    for (const PXR_NS::SdfPath& path : topMost(usdStage, paths)) {
        if (path.GetParentPath() == newParent) {  // a reorder: an opinion on the parent, nothing moves
            const PXR_NS::UsdPrim prim = usdStage->GetPrimAtPath(path);
            if (!prim || prim.IsInstanceProxy()) return tr("Can't reorder %1: it's part of an instance").arg(qs(path.GetString()));
            const PXR_NS::UsdPrim parent = usdStage->GetPrimAtPath(newParent);
            if (!parent || !isEditable(parent)) return tr("Can't reorder the children of %1 from here").arg(qs(newParent.GetString()));
            continue;
        }
        if (const QString why = whyNotMove(usdStage, path, newParent); !why.isEmpty()) return why;
        if (!arriving.insert(path.GetNameToken()).second) return tr("Can't move two prims called %1 to one parent").arg(qs(path.GetName()));
    }
    return {};
}

MoveNotes moveNotes(const PXR_NS::UsdStageRefPtr& usdStage, const std::vector<PXR_NS::SdfPath>& paths, const PXR_NS::SdfPath& newParent) {
    MoveNotes notes;
    const PXR_NS::SdfLayerHandle target = usdStage->GetEditTarget().GetLayer();
    for (const PXR_NS::SdfPath& path : topMost(usdStage, paths)) {
        if (path.GetParentPath() == newParent) continue;
        for (const PXR_NS::SdfLayerHandle& layer : usdStage->GetLayerStack(/*includeSessionLayers=*/false)) {
            if (layer == target || !layer->GetPrimAtPath(path)) continue;
            const std::string name = layer->GetDisplayName();
            if (std::find(notes.otherLayers.begin(), notes.otherLayers.end(), name) == notes.otherLayers.end()) notes.otherLayers.push_back(name);
        }
        for (const PXR_NS::UsdPrimCompositionQueryArc& arc : PXR_NS::UsdPrimCompositionQuery(usdStage->GetPrimAtPath(path)).GetCompositionArcs()) {
            if (arc.IsAncestral()) {
                notes.relocated.push_back(path);
                break;
            }
        }
    }
    return notes;
}

QString movePrims(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths,
                  const PXR_NS::SdfPath& newParent, const MoveOptions& options) {
    const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
    if (const QString why = whyNotDrop(usdStage, paths, newParent); !why.isEmpty()) return why;
    const std::vector<PXR_NS::SdfPath> top = topMost(usdStage, paths);

    PrimMoves moves;
    std::vector<PXR_NS::TfToken> movedNames;  // all of them, in order, reordered ones too
    for (const PXR_NS::SdfPath& path : top) {
        movedNames.push_back(path.GetNameToken());
        if (path.GetParentPath() != newParent) moves.emplace_back(path, newParent.AppendChild(path.GetNameToken()));
    }

    // The order to write: the children staying, with these at the row asked for.
    const std::vector<PXR_NS::TfToken> before = childNames(usdStage, newParent);
    std::vector<PXR_NS::TfToken> order;
    if (options.row >= 0) {
        std::vector<PXR_NS::TfToken> staying;
        std::size_t insertAt = 0;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (std::find(movedNames.begin(), movedNames.end(), before[i]) != movedNames.end()) continue;
            staying.push_back(before[i]);
            if (int(i) < options.row) insertAt = staying.size();
        }
        order = staying;
        order.insert(order.begin() + std::ptrdiff_t(insertAt), movedNames.begin(), movedNames.end());
    }
    const QString verb = moves.empty() ? tr("Reorder") : tr("Move");
    if (moves.empty()) {
        if (order.empty() || order == before) return {};  // it's where it was dropped
        if (!undo.apply(stage, describe(verb, top), [&]() { return writeChildOrder(usdStage, newParent, order); })) {
            return tr("Couldn't reorder the children of %1").arg(qs(newParent.GetString()));
        }
        return {};
    }

    // Before the move, the world transforms to keep.
    struct Keep {
        PXR_NS::SdfPath to;
        PXR_NS::GfMatrix4d world;
        PXR_NS::GfMatrix4d oldParent;
    };
    std::vector<Keep> keep;
    if (options.keepWorldPosition) {
        PXR_NS::UsdGeomXformCache cache;
        const bool parentAnimated = isAnimated(usdStage->GetPrimAtPath(newParent));
        for (const auto& [from, to] : moves) {
            const PXR_NS::UsdPrim prim = usdStage->GetPrimAtPath(from);
            const PXR_NS::UsdGeomXformable xformable(prim);
            if (!xformable || xformable.GetResetXformStack()) continue;  // no transform, or the parent's doesn't reach it
            if (parentAnimated || isAnimated(prim)) {
                USDCC_LOG_INFO("usdcc.stage", "{} is animated, or a parent is: keeping its local transform, so it may change place",
                               from.GetString());
                continue;
            }
            keep.push_back({to, cache.GetLocalToWorldTransform(prim), cache.GetParentToWorldTransform(prim)});
        }
    }

    QString why;
    const bool done = applyMoves(
        undo, stage, moves, moves.size() == 1 ? tr("Move %1 to %2").arg(qs(moves.front().first.GetName()), qs(newParent.GetString())) : describe(verb, top),
        options.openStages,
        [&]() {
            PXR_NS::UsdGeomXformCache cache;
            for (const Keep& kept : keep) {
                const PXR_NS::UsdPrim prim = usdStage->GetPrimAtPath(kept.to);
                const PXR_NS::GfMatrix4d newParentWorld = cache.GetParentToWorldTransform(prim);
                if (PXR_NS::GfIsClose(newParentWorld, kept.oldParent, 1e-9)) continue;
                if (!XformStack(prim).setLocalTransform(kept.world * newParentWorld.GetInverse())) return false;
            }
            return order.empty() || writeChildOrder(usdStage, newParent, order);
        },
        &why);
    if (!done && why.isEmpty()) why = tr("Couldn't move it");
    return done ? QString() : why;
}

QString whyNotCreateUnder(const PXR_NS::UsdPrim& parent) {
    if (!parent) return tr("Can't create here: it doesn't exist");
    const QString path = qs(parent.GetPath().GetString());
    if (parent.IsInstanceProxy()) return tr("Can't create in %1: it's part of an instance").arg(path);
    if (!editTargetReaches(parent)) return tr("Can't create in %1: the edit target doesn't reach it").arg(path);
    if (!parent.IsActive()) return tr("Can't create in %1: it's inactive").arg(path);
    if (!parent.IsLoaded()) return tr("Can't create in %1: it isn't loaded").arg(path);
    return {};
}

CreateTarget createTarget(const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& selection) {
    const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
    PXR_NS::UsdPrim parent = selection.size() == 1 ? usdStage->GetPrimAtPath(selection.front()) : PXR_NS::UsdPrim();
    if (const std::optional<VariantEdit>& edit = stage->variantEdit()) {
        if (!parent || !editTargetReaches(parent)) parent = usdStage->GetPrimAtPath(edit->prim);
    } else if (!parent) {
        parent = usdStage->GetPseudoRoot();
    }
    return {parent ? parent.GetPath() : PXR_NS::SdfPath(), whyNotCreateUnder(parent)};
}

PXR_NS::SdfPath createPrim(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& parent, const std::string& type) {
    const PXR_NS::UsdStageRefPtr& usdStage = stage->usdStage();
    if (!whyNotCreateUnder(usdStage->GetPrimAtPath(parent)).isEmpty()) return {};
    const PXR_NS::TfToken typeName(type);
    if (!type.empty() && !PXR_NS::UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(typeName)) return {};

    const std::string base =
        type.empty() ? std::string("Prim")
                     : PXR_NS::UsdSchemaRegistry::ParseSchemaFamilyAndVersionFromIdentifier(typeName).first.GetString();
    PXR_NS::SdfPath path = parent.AppendChild(PXR_NS::TfToken(base));
    // GetPrimAtPath finds an inactive prim too, and its name is just as taken.
    for (int i = 1; usdStage->GetPrimAtPath(path); ++i) path = parent.AppendChild(PXR_NS::TfToken(base + std::to_string(i)));

    if (!undo.apply(stage, tr("Create %1").arg(qs(base)),
                    [&]() { return static_cast<bool>(usdStage->DefinePrim(path, typeName)); })) {
        return {};
    }
    stage->primCreated(path);
    return path;
}

bool hasPayloads(const StageRefPtr& stage, const PXR_NS::SdfPath& path) {
    return !stage->usdStage()->FindLoadable(path).empty();
}

bool canLoad(const StageRefPtr& stage, const PXR_NS::SdfPath& path) {
    const PXR_NS::SdfPathSet loaded = stage->usdStage()->GetLoadSet();
    for (const PXR_NS::SdfPath& loadable : stage->usdStage()->FindLoadable(path)) {
        if (!loaded.count(loadable)) return true;
    }
    return false;
}

bool canUnload(const StageRefPtr& stage, const PXR_NS::SdfPath& path) {
    for (const PXR_NS::SdfPath& loaded : stage->usdStage()->GetLoadSet()) {
        if (loaded.HasPrefix(path)) return true;
    }
    return false;
}

bool load(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths) {
    PXR_NS::SdfPathSet targets;
    std::vector<PXR_NS::SdfPath> named;
    for (const PXR_NS::SdfPath& path : paths) {
        if (canLoad(stage, path)) {
            targets.insert(path);
            named.push_back(path);
        }
    }
    if (targets.empty()) return false;
    return pushLoadRules(undo, stage, describe(tr("Load"), named), [targets](const PXR_NS::UsdStageRefPtr& usdStage) {
        usdStage->LoadAndUnload(targets, {}, PXR_NS::UsdLoadWithDescendants);
    });
}

bool unload(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths) {
    PXR_NS::SdfPathSet targets;
    std::vector<PXR_NS::SdfPath> named;
    for (const PXR_NS::SdfPath& path : paths) {
        if (canUnload(stage, path)) {
            targets.insert(path);
            named.push_back(path);
        }
    }
    if (targets.empty()) return false;
    return pushLoadRules(undo, stage, describe(tr("Unload"), named), [targets](const PXR_NS::UsdStageRefPtr& usdStage) {
        usdStage->LoadAndUnload({}, targets);
    });
}

bool applySchema(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& schema,
                 bool addAttributes) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || schema.empty()) return false;
    const std::vector<PXR_NS::TfToken> attributes =
        addAttributes ? schemaAttributesToAdd(prim, schema) : std::vector<PXR_NS::TfToken>();
    return undo.apply(stage, tr("Apply %1 to %2").arg(qs(schema), qs(path.GetName())), [&]() {
        if (!stage->usdStage()->GetPrimAtPath(path).AddAppliedSchema(PXR_NS::TfToken(schema))) return false;
        // Looked up afresh: applying the schema recomposed the prim.
        const PXR_NS::UsdPrim applied = stage->usdStage()->GetPrimAtPath(path);
        const PXR_NS::UsdPrimDefinition& definition = applied.GetPrimDefinition();
        for (const PXR_NS::TfToken& name : attributes) {
            const PXR_NS::UsdPrimDefinition::Attribute attributeDefinition = definition.GetAttributeDefinition(name);
            if (!attributeDefinition) continue;
            const PXR_NS::UsdAttribute attribute = applied.CreateAttribute(
                name, attributeDefinition.GetTypeName(), /*custom=*/false, attributeDefinition.GetVariability());
            if (!attribute) return false;
            // Still reads as the schema's fallback, until this makes it authored.
            PXR_NS::VtValue value;
            if (attribute.Get(&value) && !value.IsEmpty() && !attribute.Set(value)) return false;
        }
        return true;
    });
}

namespace {
bool setTarget(const PXR_NS::UsdPrim& prim, const PXR_NS::TfToken& name, const PXR_NS::SdfPath& target, bool replace) {
    PXR_NS::UsdRelationship rel = prim.GetRelationship(name);
    if (!rel) rel = prim.CreateRelationship(name, /*custom=*/false);
    if (!rel) return false;
    return replace ? rel.SetTargets({target}) : rel.AddTarget(target);
}
}  // namespace

bool applySchemaWithTarget(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                           const std::string& schema, const PXR_NS::TfToken& relationship, const PXR_NS::SdfPath& target) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || schema.empty() || target.IsEmpty()) return false;
    return undo.apply(stage, tr("Apply %1 to %2").arg(qs(schema), qs(path.GetName())), [&]() {
        if (!stage->usdStage()->GetPrimAtPath(path).AddAppliedSchema(PXR_NS::TfToken(schema))) return false;
        return setTarget(stage->usdStage()->GetPrimAtPath(path), relationship, target, /*replace=*/true);
    });
}

bool addRelationshipTarget(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                           const PXR_NS::TfToken& relationship, const PXR_NS::SdfPath& target, bool replace) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || target.IsEmpty()) return false;
    return undo.apply(stage, tr("Set %1 on %2").arg(qs(relationship.GetString()), qs(path.GetName())),
                      [&]() { return setTarget(prim, relationship, target, replace); });
}

bool removeRelationshipTarget(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                              const PXR_NS::TfToken& relationship, const PXR_NS::SdfPath& target) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim)) return false;
    const PXR_NS::UsdRelationship rel = prim.GetRelationship(relationship);
    if (!rel) return false;
    return undo.apply(stage, tr("Remove a target of %1 on %2").arg(qs(relationship.GetString()), qs(path.GetName())), [&]() {
        if (!rel.RemoveTarget(target)) return false;
        PXR_NS::SdfPathVector left;
        rel.GetTargets(&left);
        if (left.empty() && rel.GetPropertyStack().size() == 1 &&
            stage->usdStage()->GetEditTarget().GetPropertySpecForScenePath(rel.GetPath())) {
            rel.ClearTargets(/*removeSpec=*/true);
        }
        return true;
    });
}

bool removeSchema(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& schema,
                  bool cleanUp) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim)) return false;
    const std::vector<AppliedSchema> schemas = appliedSchemas(prim);
    const auto it = std::find_if(schemas.begin(), schemas.end(), [&](const AppliedSchema& s) { return s.name == schema; });
    if (it == schemas.end() || it->state == AppliedSchema::State::BuiltIn) return false;
    // Before it goes: afterwards the prim no longer says what it defined.
    const PXR_NS::TfTokenVector orphans = cleanUp ? orphanedProperties(prim, schema).inEditTarget : PXR_NS::TfTokenVector();
    return undo.apply(stage, tr("Remove %1 from %2").arg(qs(schema), qs(path.GetName())), [&]() {
        const PXR_NS::UsdPrim p = stage->usdStage()->GetPrimAtPath(path);
        if (!p.RemoveAppliedSchema(PXR_NS::TfToken(schema))) return false;
        for (const PXR_NS::TfToken& name : orphans) {
            // Looked up afresh: removing the schema recomposed the prim.
            if (!stage->usdStage()->GetPrimAtPath(path).RemoveProperty(name)) return false;
        }
        return true;
    });
}

bool addArc(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, ArcInfo::Type type,
            const std::string& assetPath, const PXR_NS::SdfPath& target) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || type == ArcInfo::Type::Variant) return false;
    const bool asset = type == ArcInfo::Type::Reference || type == ArcInfo::Type::Payload;
    if (asset ? assetPath.empty() && target.IsEmpty() : !target.IsAbsolutePath() || !target.IsPrimPath()) return false;
    const QString description =
        tr("Add %1 to %2").arg(tr(arcTypeName(type)), qs(path.GetName()));
    return undo.apply(stage, description, [&]() {
        const PXR_NS::UsdPrim p = stage->usdStage()->GetPrimAtPath(path);
        switch (type) {
            case ArcInfo::Type::Reference: return p.GetReferences().AddReference(PXR_NS::SdfReference(assetPath, target));
            case ArcInfo::Type::Payload: return p.GetPayloads().AddPayload(PXR_NS::SdfPayload(assetPath, target));
            case ArcInfo::Type::Inherit: return p.GetInherits().AddInherit(target);
            case ArcInfo::Type::Specialize: return p.GetSpecializes().AddSpecialize(target);
            case ArcInfo::Type::Variant: return false;
        }
        return false;
    });
}

bool changeArc(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const ArcInfo& arc,
               const ArcInfo& changed) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || !arc.editable) return false;
    const bool same = changed.assetPath == arc.assetPath && changed.primPath == arc.primPath &&
                      changed.layerOffset == arc.layerOffset;
    if (same) return false;
    return undo.apply(stage, tr("Change %1 on %2").arg(tr(arcTypeName(arc.type)), qs(path.GetName())),
                      [&]() { return editArcEntry(stage->usdStage(), path, arc, &changed); });
}

bool removeArc(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const ArcInfo& arc) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || !arc.editable) return false;
    return undo.apply(stage, tr("Remove %1 from %2").arg(tr(arcTypeName(arc.type)), qs(path.GetName())),
                      [&]() { return editArcEntry(stage->usdStage(), path, arc, nullptr); });
}

std::vector<std::pair<QString, QString>> info(const PXR_NS::UsdPrim& prim) {
    std::vector<std::pair<QString, QString>> rows;
    if (!prim) return rows;
    rows.emplace_back(tr("Path"), qs(prim.GetPath().GetString()));
    rows.emplace_back(tr("Type"), prim.GetTypeName().IsEmpty() ? tr("(none)") : qs(prim.GetTypeName().GetString()));
    QStringList schemas;
    for (const PXR_NS::TfToken& schema : prim.GetAppliedSchemas()) schemas << qs(schema.GetString());
    if (!schemas.isEmpty()) rows.emplace_back(tr("Applied schemas"), schemas.join(QStringLiteral(", ")));

    const PXR_NS::SdfSpecifier specifier = prim.GetSpecifier();
    rows.emplace_back(tr("Specifier"), specifier == PXR_NS::SdfSpecifierDef     ? QStringLiteral("def")
                                       : specifier == PXR_NS::SdfSpecifierClass ? QStringLiteral("class")
                                                                                : QStringLiteral("over"));
    PXR_NS::TfToken kind;
    if (PXR_NS::UsdModelAPI(prim).GetKind(&kind) && !kind.IsEmpty()) rows.emplace_back(tr("Kind"), qs(kind.GetString()));

    QStringList state;
    state << (prim.IsActive() ? tr("active") : tr("inactive"));
    if (!prim.IsDefined()) state << tr("undefined");
    if (prim.IsAbstract()) state << tr("abstract");
    if (!prim.GetStage()->FindLoadable(prim.GetPath()).empty()) state << (prim.IsLoaded() ? tr("loaded") : tr("unloaded"));
    if (const PXR_NS::UsdGeomImageable imageable{prim}) {
        state << (imageable.ComputeVisibility() == PXR_NS::UsdGeomTokens->invisible ? tr("invisible") : tr("visible"));
    }
    if (prim.IsInstance()) state << tr("instance of %1").arg(qs(prim.GetPrototype().GetPath().GetString()));
    if (prim.IsInstanceProxy()) state << tr("instance proxy");
    if (prim.IsInPrototype()) state << tr("in a prototype");
    rows.emplace_back(tr("State"), state.join(QStringLiteral(", ")));

    int references = 0, payloads = 0, inherits = 0, specializes = 0;
    for (const PXR_NS::UsdPrimCompositionQueryArc& arc : PXR_NS::UsdPrimCompositionQuery(prim).GetCompositionArcs()) {
        switch (arc.GetArcType()) {
            case PXR_NS::PcpArcTypeReference: ++references; break;
            case PXR_NS::PcpArcTypePayload: ++payloads; break;
            case PXR_NS::PcpArcTypeInherit: ++inherits; break;
            case PXR_NS::PcpArcTypeSpecialize: ++specializes; break;
            default: break;
        }
    }
    rows.emplace_back(tr("Composition"), tr("%1 references, %2 payloads, %3 inherits, %4 specializes")
                                             .arg(references)
                                             .arg(payloads)
                                             .arg(inherits)
                                             .arg(specializes));
    QStringList variants;
    const PXR_NS::UsdVariantSets sets = prim.GetVariantSets();
    for (const std::string& name : sets.GetNames()) {
        const std::string selection = sets.GetVariantSelection(name);
        variants << QStringLiteral("%1 = %2").arg(qs(name), selection.empty() ? tr("(none)") : qs(selection));
    }
    if (!variants.isEmpty()) rows.emplace_back(tr("Variants"), variants.join(QStringLiteral(", ")));

    QStringList layers;
    for (const PXR_NS::SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
        const QString layer = qs(spec->GetLayer()->GetIdentifier());
        if (!layers.contains(layer)) layers << layer;
    }
    rows.emplace_back(tr("Defined in"), layers.join(QLatin1Char('\n')));

    const auto children = prim.GetFilteredChildren(PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimAllPrimsPredicate));
    rows.emplace_back(tr("Children"), QString::number(std::distance(children.begin(), children.end())));
    return rows;
}

QString infoText(const PXR_NS::UsdPrim& prim) {
    QStringList lines;
    for (const auto& [label, value] : info(prim)) {
        lines << QStringLiteral("%1: %2").arg(label, QString(value).replace(QLatin1Char('\n'), QStringLiteral("\n    ")));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace usdcc::usd::prim_actions
