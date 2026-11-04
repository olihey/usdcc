#include "usdcc/usd/prim_actions.h"

#include "usdcc/usd/applied_schemas.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/inherits.h>
#include <pxr/usd/usd/namespaceEditor.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/specializes.h>
#include <pxr/usd/usd/primCompositionQuery.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stageLoadRules.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QCoreApplication>
#include <QStringList>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <functional>
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

bool pushLoadRules(UndoManager& undo, const StageRefPtr& stage, const QString& description,
                   std::function<void(const PXR_NS::UsdStageRefPtr&)> change) {
    QUndoStack* stack = undo.stack(stage);
    if (!stack) return false;
    stack->push(new LoadRulesCommand(stage->usdStage(), description, std::move(change)));
    return true;
}
}  // namespace

bool isEditable(const PXR_NS::UsdPrim& prim) { return prim && !prim.IsInstanceProxy(); }

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
    if (prim.GetVariantSet(variantSet).GetVariantSelection() == variant) return false;
    return undo.apply(stage,
                      tr("Set %1 to %2 on %3").arg(qs(variantSet), qs(variant), qs(path.GetName())), [&]() {
                          return stage->usdStage()->GetPrimAtPath(path).GetVariantSet(variantSet).SetVariantSelection(
                              variant);
                      });
}

bool clearVariant(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path,
                  const std::string& variantSet) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || !prim.GetVariantSets().HasVariantSet(variantSet)) return false;
    return undo.apply(stage, tr("Clear %1 on %2").arg(qs(variantSet), qs(path.GetName())), [&]() {
        return stage->usdStage()->GetPrimAtPath(path).GetVariantSet(variantSet).ClearVariantSelection();
    });
}

bool rename(UndoManager& undo, const StageRefPtr& stage, const PXR_NS::SdfPath& path, const std::string& name) {
    const PXR_NS::UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!isEditable(prim) || name.empty() || name == path.GetName() || !PXR_NS::SdfPath::IsValidIdentifier(name)) {
        return false;
    }
    return undo.apply(stage, tr("Rename %1 to %2").arg(qs(path.GetName()), qs(name)), [&]() {
        PXR_NS::UsdNamespaceEditor editor(stage->usdStage());
        return editor.RenamePrim(stage->usdStage()->GetPrimAtPath(path), PXR_NS::TfToken(name)) && editor.ApplyEdits();
    });
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
