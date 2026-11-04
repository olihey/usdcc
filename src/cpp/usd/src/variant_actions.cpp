#include "usdcc/usd/variant_actions.h"

#include "usdcc/usd/undo_manager.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>

#include <QCoreApplication>

#include <algorithm>
#include <map>
#include <sstream>

namespace usdcc::usd::variant_actions {

using namespace PXR_NS;

namespace {

QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::variant_actions", text); }
QString qs(const std::string& text) { return QString::fromStdString(text); }

// Where managing `set` on the prim writes (see the header).
UsdEditTarget targetFor(const StageRefPtr& stage, const SdfPath& path, const std::string& set) {
    const UsdEditTarget target = stage->usdStage()->GetEditTarget();
    return stage->isEditingVariantSet(path, set) ? UsdEditTarget(target.GetLayer()) : target;
}

SdfVariantSetSpecHandle setSpecOf(const SdfPrimSpecHandle& spec, const std::string& set) {
    if (!spec) return {};
    const SdfVariantSetsProxy sets = spec->GetVariantSets();
    const auto it = sets.find(set);
    return it == sets.end() ? SdfVariantSetSpecHandle() : (*it).second;
}

SdfVariantSpecHandle variantSpecOf(const SdfVariantSetSpecHandle& setSpec, const std::string& variant) {
    if (!setSpec) return {};
    for (const SdfVariantSpecHandle& spec : setSpec->GetVariantList()) {
        if (spec->GetName() == variant) return spec;
    }
    return {};
}

SdfPrimSpecHandle specHere(const StageRefPtr& stage, const SdfPath& path, const std::string& set) {
    const UsdEditTarget target = targetFor(stage, path, set);
    return target.GetLayer()->GetPrimAtPath(target.MapToSpecPath(path));
}

// The set's spec here, created with its name in the list when it's new.
SdfVariantSetSpecHandle makeSet(const SdfPrimSpecHandle& spec, const std::string& set) {
    if (!spec) return {};
    SdfVariantSetSpecHandle setSpec = setSpecOf(spec, set);
    if (!setSpec) setSpec = SdfVariantSetSpec::New(spec, set);
    if (!setSpec) return {};
    SdfVariantSetNamesProxy names = spec->GetVariantSetNameList();
    if (!names.ContainsItemEdit(set)) names.Prepend(set);
    return setSpec;
}

SdfPrimSpecHandle makeSpecHere(const StageRefPtr& stage, const SdfPath& path, const std::string& set) {
    const UsdEditTarget target = targetFor(stage, path, set);
    return SdfCreatePrimInLayer(target.GetLayer(), target.MapToSpecPath(path));
}

std::string selectionIn(const SdfPrimSpecHandle& spec, const std::string& set) {
    if (!spec) return {};
    const SdfVariantSelectionProxy selections = spec->GetVariantSelections();
    const auto it = selections.find(set);
    return it == selections.end() ? std::string() : std::string((*it).second);
}

bool setInStack(const SdfPrimSpecHandle& spec, const std::string& set) { return static_cast<bool>(setSpecOf(spec, set)); }

// The value a property holds, in a line.
template <typename Value>
std::string text(const Value& value) {
    std::ostringstream out;
    out << value;
    std::string result = out.str();
    constexpr std::size_t kMax = 120;
    if (result.size() > kMax) result = result.substr(0, kMax) + "...";
    return result;
}

void readPrim(const SdfLayerHandle& layer, const SdfPrimSpecHandle& spec, std::vector<Opinion>& out) {
    // Metadata and arcs: every field that says something, minus the structure, which
    // the rows themselves show.
    static const std::vector<TfToken> kStructure = {
        SdfChildrenKeys->PrimChildren,       SdfChildrenKeys->PropertyChildren, SdfChildrenKeys->VariantSetChildren,
        SdfChildrenKeys->VariantChildren,    SdfFieldKeys->Specifier,           SdfFieldKeys->TypeName,
        SdfFieldKeys->VariantSetNames,
    };
    const SdfPath path = spec->GetPath();
    for (const TfToken& field : layer->ListFields(path)) {
        if (std::find(kStructure.begin(), kStructure.end(), field) != kStructure.end()) continue;
        Opinion opinion;
        opinion.kind = Opinion::Kind::Metadata;
        opinion.label = field.GetString();
        opinion.value = text(layer->GetField(path, field));
        opinion.specPath = path;
        opinion.field = field;
        out.push_back(std::move(opinion));
    }
    const SdfVariantSetsProxy sets = spec->GetVariantSets();
    for (auto it = sets.begin(); it != sets.end(); ++it) {
        const SdfVariantSetSpecHandle setSpec = (*it).second;
        Opinion opinion;
        opinion.kind = Opinion::Kind::VariantSet;
        opinion.label = setSpec->GetName();
        opinion.specPath = path;
        std::vector<std::string> names;
        for (const SdfVariantSpecHandle& variant : setSpec->GetVariantList()) names.push_back(variant->GetName());
        opinion.value = TfStringJoin(names, ", ");
        out.push_back(std::move(opinion));
    }
    for (const SdfPropertySpecHandle& property : spec->GetProperties()) {
        Opinion opinion;
        const bool attribute = property->GetSpecType() == SdfSpecTypeAttribute;
        opinion.kind = attribute ? Opinion::Kind::Attribute : Opinion::Kind::Relationship;
        opinion.label = property->GetName();
        opinion.specPath = property->GetPath();
        if (!attribute) {
            opinion.value = text(layer->GetField(property->GetPath(), SdfFieldKeys->TargetPaths));
        } else if (const std::size_t samples = layer->GetNumTimeSamplesForPath(property->GetPath()); samples > 0) {
            opinion.value = samples == 1 ? std::string("1 time sample") : std::to_string(samples) + " time samples";
        } else if (property->HasDefaultValue()) {
            opinion.value = text(property->GetDefaultValue());
        } else {
            opinion.value = property->GetTypeName().GetAsToken().GetString() + ", no value";
        }
        out.push_back(std::move(opinion));
    }
    for (const SdfPrimSpecHandle& child : spec->GetNameChildren()) {
        Opinion opinion;
        opinion.kind = Opinion::Kind::Prim;
        opinion.label = child->GetName();
        const SdfSpecifier specifier = child->GetSpecifier();
        opinion.value = (specifier == SdfSpecifierDef ? "def" : specifier == SdfSpecifierClass ? "class" : "over");
        if (!child->GetTypeName().IsEmpty()) opinion.value += " " + child->GetTypeName().GetString();
        opinion.specPath = child->GetPath();
        readPrim(layer, child, opinion.children);
        out.push_back(std::move(opinion));
    }
}

void removeSetFrom(const SdfPrimSpecHandle& spec, const std::string& set) {
    if (setInStack(spec, set)) spec->RemoveVariantSet(set);
    spec->GetVariantSetNameList().RemoveItemEdits(set);
    if (!selectionIn(spec, set).empty()) spec->SetVariantSelection(set, std::string());
}

}  // namespace

bool isValidSetName(const std::string& name) { return SdfPath::IsValidIdentifier(name); }

bool isValidVariantName(const std::string& name) {
    return !name.empty() && static_cast<bool>(SdfSchema::IsValidVariantIdentifier(name));
}

SetPlace setPlace(const StageRefPtr& stage, const UsdPrim& prim, const std::string& set) {
    SetPlace place;
    if (!prim) return place;
    const UsdEditTarget target = targetFor(stage, prim.GetPath(), set);
    const SdfPath here = target.MapToSpecPath(prim.GetPath());
    for (const SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
        const bool isHere = spec->GetLayer() == target.GetLayer() && spec->GetPath() == here;
        if (isHere && spec->GetVariantSetNameList().ContainsItemEdit(set)) place.here = true;
        if (!setInStack(spec, set)) continue;
        if (isHere) {
            place.here = true;
        } else if (!place.elsewhere) {
            place.elsewhere = true;
            place.elsewhereLayer = spec->GetLayer()->GetDisplayName();
        }
    }
    return place;
}

bool VariantEntry::isHere() const {
    return std::any_of(places.begin(), places.end(), [](const VariantPlace& p) { return p.here; });
}

bool VariantEntry::isElsewhere() const {
    return std::any_of(places.begin(), places.end(), [](const VariantPlace& p) { return !p.here; });
}

std::vector<VariantEntry> variantsOf(const StageRefPtr& stage, const UsdPrim& prim, const std::string& set) {
    std::map<std::string, VariantEntry> entries;
    if (!prim) return {};
    const UsdEditTarget target = targetFor(stage, prim.GetPath(), set);
    const SdfPath here = target.MapToSpecPath(prim.GetPath());
    for (const SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
        const SdfVariantSetSpecHandle setSpec = setSpecOf(spec, set);
        if (!setSpec) continue;
        for (const SdfVariantSpecHandle& variant : setSpec->GetVariantList()) {
            VariantEntry& entry = entries[variant->GetName()];
            entry.name = variant->GetName();
            entry.places.push_back({spec->GetLayer(), spec->GetPath().AppendVariantSelection(set, variant->GetName()),
                                    spec->GetLayer() == target.GetLayer() && spec->GetPath() == here});
        }
    }
    std::vector<VariantEntry> list;
    for (auto& [name, entry] : entries) list.push_back(std::move(entry));
    return list;
}

bool canRenameVariant(const StageRefPtr& stage, const SdfPath& path, const std::string& set, const VariantEntry& entry,
                      QString* why) {
    const std::optional<VariantEdit>& edit = stage->variantEdit();
    QString reason;
    if (edit && edit->prim == path && edit->set == set && edit->variant == entry.name) {
        reason = tr("It's being edited: exit first");
    } else if (!entry.isHere()) {
        reason = tr("Defined in %1").arg(qs(entry.places.empty() ? std::string() : entry.places.front().layer->GetDisplayName()));
    } else if (entry.isElsewhere()) {
        reason = tr("Also defined in another layer, which would keep the old name");
    }
    if (why) *why = reason;
    return reason.isEmpty();
}

bool canRemoveVariant(const StageRefPtr& stage, const SdfPath& path, const std::string& set, const VariantEntry& entry,
                      QString* why) {
    const std::optional<VariantEdit>& edit = stage->variantEdit();
    QString reason;
    if (edit && edit->prim == path && edit->set == set && edit->variant == entry.name) {
        reason = tr("It's being edited: exit first");
    } else if (!entry.isHere()) {
        reason = tr("Defined in %1").arg(qs(entry.places.empty() ? std::string() : entry.places.front().layer->GetDisplayName()));
    }
    if (why) *why = reason;
    return reason.isEmpty();
}

bool canRenameSet(const StageRefPtr& stage, const UsdPrim& prim, const std::string& set, QString* why) {
    const SetPlace place = setPlace(stage, prim, set);
    QString reason;
    if (prim && stage->isEditingVariantSet(prim.GetPath(), set)) {
        reason = tr("A variant of it is being edited: exit first");
    } else if (!place.here) {
        reason = tr("Defined in %1").arg(qs(place.elsewhereLayer));
    } else if (place.elsewhere) {
        reason = tr("Also defined in %1, which would keep the old name").arg(qs(place.elsewhereLayer));
    }
    if (why) *why = reason;
    return reason.isEmpty();
}

bool canRemoveSet(const StageRefPtr& stage, const UsdPrim& prim, const std::string& set, QString* why) {
    const SetPlace place = setPlace(stage, prim, set);
    QString reason;
    if (prim && stage->isEditingVariantSet(prim.GetPath(), set)) {
        reason = tr("A variant of it is being edited: exit first");
    } else if (!place.here) {
        reason = tr("Defined in %1").arg(qs(place.elsewhereLayer));
    }
    if (why) *why = reason;
    return reason.isEmpty();
}

bool addSet(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, const std::string& set,
            const std::string& firstVariant) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!prim || !isValidSetName(set) || !isValidVariantName(firstVariant) || prim.GetVariantSets().HasVariantSet(set)) {
        return false;
    }
    return undo.apply(stage, targetFor(stage, path, set).GetLayer(), tr("Add Variant Set %1 to %2").arg(qs(set), qs(path.GetName())), [&]() {
        const SdfPrimSpecHandle spec = makeSpecHere(stage, path, set);
        const SdfVariantSetSpecHandle setSpec = makeSet(spec, set);
        if (!setSpec || !SdfVariantSpec::New(setSpec, firstVariant)) return false;
        spec->SetVariantSelection(set, firstVariant);
        return true;
    });
}

bool renameSet(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, const std::string& set, const std::string& name) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!prim || !isValidSetName(name) || name == set || prim.GetVariantSets().HasVariantSet(name) ||
        !canRenameSet(stage, prim, set)) {
        return false;
    }
    const SdfLayerHandle layer = targetFor(stage, path, set).GetLayer();
    return undo.apply(stage, layer, tr("Rename Variant Set %1 to %2").arg(qs(set), qs(name)), [&]() {
        const SdfPrimSpecHandle spec = specHere(stage, path, set);
        if (!spec) return false;
        if (setInStack(spec, set)) {
            const SdfPath from = spec->GetPath().AppendVariantSelection(set, std::string());
            const SdfPath to = spec->GetPath().AppendVariantSelection(name, std::string());
            if (!SdfCopySpec(layer, from, layer, to)) return false;
            spec->RemoveVariantSet(set);
        }
        SdfVariantSetNamesProxy names = spec->GetVariantSetNameList();
        if (names.ContainsItemEdit(set)) {
            names.ReplaceItemEdits(set, name);  // keeps its place
        } else {
            names.Prepend(name);
        }
        const std::string selection = selectionIn(spec, set);
        if (!selection.empty()) {
            spec->SetVariantSelection(set, std::string());
            spec->SetVariantSelection(name, selection);
        }
        return true;
    });
}

bool removeSet(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, const std::string& set) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!prim || !canRemoveSet(stage, prim, set)) return false;
    const SdfLayerHandle layer = targetFor(stage, path, set).GetLayer();
    return undo.apply(stage, layer, tr("Remove Variant Set %1 from %2").arg(qs(set), qs(path.GetName())), [&]() {
        const SdfPrimSpecHandle spec = specHere(stage, path, set);
        if (!spec) return false;
        const SdfPath specPath = spec->GetPath();
        removeSetFrom(spec, set);
        removeEmptyOvers(layer, specPath);
        return true;
    });
}

bool addVariant(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, const std::string& set, const std::string& name) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!prim || !isValidVariantName(name) || prim.GetVariantSet(set).HasAuthoredVariant(name)) return false;
    return undo.apply(stage, targetFor(stage, path, set).GetLayer(), tr("Add Variant %1 to %2").arg(qs(name), qs(set)), [&]() {
        const SdfVariantSetSpecHandle setSpec = makeSet(makeSpecHere(stage, path, set), set);
        return setSpec && SdfVariantSpec::New(setSpec, name);
    });
}

bool renameVariant(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, const std::string& set,
                   const std::string& variant, const std::string& name) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!prim || !isValidVariantName(name) || name == variant || prim.GetVariantSet(set).HasAuthoredVariant(name)) return false;
    const std::vector<VariantEntry> entries = variantsOf(stage, prim, set);
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const VariantEntry& e) { return e.name == variant; });
    if (entry == entries.end() || !canRenameVariant(stage, path, set, *entry)) return false;
    const SdfLayerHandle layer = targetFor(stage, path, set).GetLayer();
    return undo.apply(stage, layer, tr("Rename Variant %1 to %2").arg(qs(variant), qs(name)), [&]() {
        const SdfPrimSpecHandle spec = specHere(stage, path, set);
        const SdfVariantSetSpecHandle setSpec = setSpecOf(spec, set);
        const SdfVariantSpecHandle old = variantSpecOf(setSpec, variant);
        if (!old || !SdfVariantSpec::New(setSpec, name)) return false;
        if (!SdfCopySpec(layer, spec->GetPath().AppendVariantSelection(set, variant), layer,
                         spec->GetPath().AppendVariantSelection(set, name))) {
            return false;
        }
        setSpec->RemoveVariant(old);
        if (selectionIn(spec, set) == variant) spec->SetVariantSelection(set, name);
        return true;
    });
}

bool duplicateVariant(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, const std::string& set,
                      const std::string& variant, const std::string& name) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!prim || !isValidVariantName(name) || prim.GetVariantSet(set).HasAuthoredVariant(name)) return false;
    const std::vector<VariantEntry> entries = variantsOf(stage, prim, set);
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const VariantEntry& e) { return e.name == variant; });
    if (entry == entries.end() || entry->places.empty()) return false;
    const VariantPlace source = entry->places.front();
    const SdfLayerHandle layer = targetFor(stage, path, set).GetLayer();
    return undo.apply(stage, layer, tr("Duplicate Variant %1 as %2").arg(qs(variant), qs(name)), [&]() {
        const SdfPrimSpecHandle spec = makeSpecHere(stage, path, set);
        const SdfVariantSetSpecHandle setSpec = makeSet(spec, set);
        if (!setSpec || !SdfVariantSpec::New(setSpec, name)) return false;
        return SdfCopySpec(source.layer, source.specPath, layer, spec->GetPath().AppendVariantSelection(set, name));
    });
}

bool removeVariant(UndoManager& undo, const StageRefPtr& stage, const SdfPath& path, const std::string& set, const std::string& variant) {
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
    if (!prim) return false;
    const std::vector<VariantEntry> entries = variantsOf(stage, prim, set);
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const VariantEntry& e) { return e.name == variant; });
    if (entry == entries.end() || !canRemoveVariant(stage, path, set, *entry)) return false;
    return undo.apply(stage, targetFor(stage, path, set).GetLayer(), tr("Remove Variant %1 from %2").arg(qs(variant), qs(set)), [&]() {
        const SdfPrimSpecHandle spec = specHere(stage, path, set);
        const SdfVariantSetSpecHandle setSpec = setSpecOf(spec, set);
        const SdfVariantSpecHandle old = variantSpecOf(setSpec, variant);
        if (!old) return false;
        setSpec->RemoveVariant(old);
        if (selectionIn(spec, set) == variant) spec->SetVariantSelection(set, std::string());
        return true;
    });
}

std::vector<Opinion> variantOpinions(const SdfLayerHandle& layer, const SdfPath& variantSpecPath) {
    std::vector<Opinion> opinions;
    if (!layer) return opinions;
    if (const SdfPrimSpecHandle spec = layer->GetPrimAtPath(variantSpecPath)) readPrim(layer, spec, opinions);
    return opinions;
}

int opinionCount(const std::vector<Opinion>& opinions) {
    int count = 0;
    for (const Opinion& opinion : opinions) count += 1 + opinionCount(opinion.children);
    return count;
}

int opinionsHere(const VariantEntry& entry) {
    int count = 0;
    for (const VariantPlace& place : entry.places) {
        if (place.here) count += opinionCount(variantOpinions(place.layer, place.specPath));
    }
    return count;
}

bool removeOpinion(UndoManager& undo, const StageRefPtr& stage, const SdfLayerHandle& layer, const Opinion& opinion) {
    if (!layer || layer != stage->usdStage()->GetEditTarget().GetLayer()) return false;
    return undo.apply(stage, layer, tr("Remove %1 from a variant").arg(qs(opinion.label)), [&]() {
        const SdfPath& path = opinion.specPath;
        SdfPath owner;
        switch (opinion.kind) {
            case Opinion::Kind::Prim: {
                owner = path.GetParentPath();
                const SdfPrimSpecHandle parent = layer->GetPrimAtPath(owner);
                const SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
                if (!parent || !spec) return false;
                parent->RemoveNameChild(spec);
                break;
            }
            case Opinion::Kind::Attribute:
            case Opinion::Kind::Relationship: {
                owner = path.GetPrimOrPrimVariantSelectionPath();
                const SdfPrimSpecHandle parent = layer->GetPrimAtPath(owner);
                const SdfPropertySpecHandle spec = layer->GetPropertyAtPath(path);
                if (!parent || !spec) return false;
                parent->RemoveProperty(spec);
                break;
            }
            case Opinion::Kind::Metadata:
                owner = path;
                layer->EraseField(path, opinion.field);
                break;
            case Opinion::Kind::VariantSet: {
                owner = path;
                const SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
                if (!spec) return false;
                removeSetFrom(spec, opinion.label);
                break;
            }
        }
        removeEmptyOvers(layer, owner);
        return true;
    });
}

}  // namespace usdcc::usd::variant_actions
