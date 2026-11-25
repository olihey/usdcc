#include "usdcc/usd/composition_arcs.h"

#include "usdcc/assets/no_wait.h"
#include "usdcc/usd/asset_servers.h"

#include <pxr/usd/ar/resolverContextBinder.h>

#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>

#include <algorithm>

namespace usdcc::usd {

namespace {

// Each item of a list op with the part of it it's in. An explicit list op
// has only its explicit items.
template <typename Proxy, typename Callback>
void forEachItem(const Proxy& list, Callback callback) {
    if (list.IsExplicit()) {
        for (const auto& item : list.GetExplicitItems()) callback(item, "explicit");
        return;
    }
    for (const auto& item : list.GetPrependedItems()) callback(item, "prepended");
    for (const auto& item : list.GetAddedItems()) callback(item, "added");
    for (const auto& item : list.GetAppendedItems()) callback(item, "appended");
}

ArcInfo where(const PXR_NS::SdfPrimSpecHandle& spec, const PXR_NS::UsdPrim& prim, bool inStageLayers,
              const PXR_NS::UsdEditTarget& editTarget, const char* position) {
    ArcInfo arc;
    const PXR_NS::SdfLayerHandle layer = spec->GetLayer();
    arc.layerIdentifier = layer->GetIdentifier();
    arc.layerName = layer->GetDisplayName();
    arc.specPath = spec->GetPath();
    arc.listPosition = position;
    // A variant of this prim (/Looks{look=red}) is still this prim's own, and can be
    // edited when it's where edits are going (variant edit mode).
    arc.nested = !inStageLayers || spec->GetPath().StripAllVariantSelections() != prim.GetPath();
    arc.editable = !arc.nested && layer == editTarget.GetLayer() && spec->GetPath() == editTarget.MapToSpecPath(prim.GetPath());
    return arc;
}

template <typename Item>
void fillAsset(ArcInfo& arc, const Item& item, const PXR_NS::SdfLayerHandle& layer) {
    arc.assetPath = item.GetAssetPath();
    arc.primPath = item.GetPrimPath();
    arc.layerOffset = item.GetLayerOffset();
    if (arc.assetPath.empty()) {
        arc.resolvedPath = layer->GetRealPath().empty() ? layer->GetIdentifier() : layer->GetRealPath();
        return;
    }
    const LocatedAsset located = locateAsset(PXR_NS::SdfComputeAssetPathRelativeToLayer(layer, arc.assetPath));
    arc.resolvedPath = located.path;
    arc.origin = located.origin;
    arc.resolving = located.resolving;
    arc.missing = located.path.empty() && !located.resolving;
}

}  // namespace

const char* arcTypeName(ArcInfo::Type type) {
    switch (type) {
        case ArcInfo::Type::Reference: return "Reference";
        case ArcInfo::Type::Payload: return "Payload";
        case ArcInfo::Type::Inherit: return "Inherit";
        case ArcInfo::Type::Specialize: return "Specialize";
        case ArcInfo::Type::Variant: return "Variant";
    }
    return "";
}

std::vector<ArcInfo> compositionArcs(const PXR_NS::UsdPrim& prim) {
    std::vector<ArcInfo> arcs;
    if (!prim) return arcs;
    const usdcc::assets::NoWait noWait;
    const PXR_NS::UsdStageRefPtr stage = prim.GetStage();
    // Resolved as the stage resolves them: a path on a server means its environment's
    // servers (docs/REMOTE_ASSETS.md §10).
    const PXR_NS::ArResolverContextBinder binder(stage->GetPathResolverContext());
    const PXR_NS::SdfLayerHandleVector stageLayers = stage->GetLayerStack(true);
    const PXR_NS::UsdEditTarget editTarget = stage->GetEditTarget();
    const bool loaded = prim.IsLoaded();

    for (const PXR_NS::SdfPrimSpecHandle& spec : prim.GetPrimStack()) {
        const PXR_NS::SdfLayerHandle layer = spec->GetLayer();
        const bool inStageLayers = std::find(stageLayers.begin(), stageLayers.end(), layer) != stageLayers.end();
        forEachItem(spec->GetReferenceList(), [&](const PXR_NS::SdfReference& item, const char* position) {
            ArcInfo arc = where(spec, prim, inStageLayers, editTarget, position);
            arc.type = ArcInfo::Type::Reference;
            fillAsset(arc, item, layer);
            arcs.push_back(arc);
        });
        forEachItem(spec->GetPayloadList(), [&](const PXR_NS::SdfPayload& item, const char* position) {
            ArcInfo arc = where(spec, prim, inStageLayers, editTarget, position);
            arc.type = ArcInfo::Type::Payload;
            fillAsset(arc, item, layer);
            arc.loaded = loaded;
            arcs.push_back(arc);
        });
        forEachItem(spec->GetInheritPathList(), [&](const PXR_NS::SdfPath& path, const char* position) {
            ArcInfo arc = where(spec, prim, inStageLayers, editTarget, position);
            arc.type = ArcInfo::Type::Inherit;
            arc.primPath = path;
            arcs.push_back(arc);
        });
        forEachItem(spec->GetSpecializesList(), [&](const PXR_NS::SdfPath& path, const char* position) {
            ArcInfo arc = where(spec, prim, inStageLayers, editTarget, position);
            arc.type = ArcInfo::Type::Specialize;
            arc.primPath = path;
            arcs.push_back(arc);
        });
    }

    // Variant sets as composed, with their selections.
    const PXR_NS::UsdVariantSets sets = prim.GetVariantSets();
    for (const std::string& name : sets.GetNames()) {
        ArcInfo arc;
        arc.type = ArcInfo::Type::Variant;
        arc.variantSet = name;
        arc.variantSelection = sets.GetVariantSelection(name);
        arcs.push_back(arc);
    }
    return arcs;
}

}  // namespace usdcc::usd
