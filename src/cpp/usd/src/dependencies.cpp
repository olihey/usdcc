#include "usdcc/usd/dependencies.h"

#include "usdcc/assets/no_wait.h"
#include "usdcc/usd/asset_servers.h"

#include <pxr/usd/ar/resolverContextBinder.h>

#include "usdcc/usd/composition_arcs.h"

#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace usdcc::usd {

namespace {

struct Located {
    std::string path;
    std::string origin;
    bool found = false;
    bool resolving = false;
};

// Where `assetPath`, named in `anchor`, points: the file it resolves to, else the anchored
// path (locateAsset()).
Located locate(const PXR_NS::SdfLayerHandle& anchor, const std::string& assetPath) {
    const std::string anchored = anchor ? PXR_NS::SdfComputeAssetPathRelativeToLayer(anchor, assetPath) : assetPath;
    const LocatedAsset asset = locateAsset(anchored);
    Located located;
    located.found = !asset.path.empty();
    located.path = located.found ? asset.path : anchored;
    located.origin = asset.origin;
    located.resolving = asset.resolving;
    return located;
}

// One spelling per file: forward slashes, and no case on Windows and macOS.
std::string fileKey(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
#if defined(_WIN32) || defined(__APPLE__)
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return path;
}

std::string fileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct Collector {
    std::map<std::string, Dependency> files;
    std::map<std::string, std::set<PXR_NS::SdfPath>> prims, unloaded;
    std::map<std::string, std::set<std::string>> layers;

    void add(Dependency::Kind kind, const std::string& assetPath, const Located& located, const std::string& layer,
             const PXR_NS::SdfPath& prim = {}, bool loaded = true) {
        const std::string key = fileKey(located.path);
        Dependency& file = files[key];
        if (file.kinds == 0) {
            file.assetPath = assetPath;
            file.resolvedPath = located.path;
            file.origin = located.origin;
            file.missing = !located.found && !located.resolving;
            file.resolving = located.resolving;
        }
        file.kinds |= kind;
        if (!layer.empty()) layers[key].insert(layer);
        if (!prim.IsEmpty()) prims[key].insert(prim);
        if (!loaded) unloaded[key].insert(prim);
    }

    void addArcs(const PXR_NS::UsdPrim& prim) {
        for (const ArcInfo& arc : compositionArcs(prim)) {
            if ((arc.type != ArcInfo::Type::Reference && arc.type != ArcInfo::Type::Payload) || arc.assetPath.empty()) {
                continue;  // an internal arc, or not a file at all
            }
            const Located located = arc.missing || arc.resolving
                                        ? locate(PXR_NS::SdfLayer::Find(arc.layerIdentifier), arc.assetPath)
                                        : Located{arc.resolvedPath, arc.origin, true, false};
            add(arc.type == ArcInfo::Type::Payload ? Dependency::Payload : Dependency::Reference, arc.assetPath, located,
                arc.layerIdentifier, prim.GetPath(), arc.type != ArcInfo::Type::Payload || arc.loaded);
        }
    }

    void addAssetAttributes(const PXR_NS::UsdPrim& prim) {
        for (const PXR_NS::UsdAttribute& attr : prim.GetAttributes()) {
            const PXR_NS::SdfValueTypeName type = attr.GetTypeName();
            if (type != PXR_NS::SdfValueTypeNames->Asset && type != PXR_NS::SdfValueTypeNames->AssetArray) continue;
            if (!attr.HasAuthoredValue()) continue;
            PXR_NS::VtValue value;
            if (!attr.Get(&value, PXR_NS::UsdTimeCode::Default()) || value.IsEmpty()) {
                attr.Get(&value, PXR_NS::UsdTimeCode::EarliestTime());
            }
            std::vector<PXR_NS::SdfAssetPath> paths;
            if (value.IsHolding<PXR_NS::SdfAssetPath>()) {
                paths.push_back(value.UncheckedGet<PXR_NS::SdfAssetPath>());
            } else if (value.IsHolding<PXR_NS::VtArray<PXR_NS::SdfAssetPath>>()) {
                const auto& array = value.UncheckedGet<PXR_NS::VtArray<PXR_NS::SdfAssetPath>>();
                paths.assign(array.begin(), array.end());
            }
            const std::vector<PXR_NS::SdfPropertySpecHandle> stack = attr.GetPropertyStack();
            const PXR_NS::SdfLayerHandle layer = stack.empty() ? PXR_NS::SdfLayerHandle() : stack.front()->GetLayer();
            for (const PXR_NS::SdfAssetPath& path : paths) {
                if (path.GetAssetPath().empty()) continue;
                const std::string& resolved = path.GetResolvedPath();
                Located located{displayAssetPath(resolved), assetOrigin(path.GetAssetPath(), resolved), !resolved.empty(), false};
                if (!located.found) located = locate(layer, path.GetAssetPath());
                // A UDIM set ("tex.<UDIM>.png") resolves per tile, never as a whole.
                if (!located.found && path.GetAssetPath().find('<') != std::string::npos) {
                    located.found = true;
                    located.resolving = false;
                }
                add(Dependency::Asset, path.GetAssetPath(), located, layer ? layer->GetIdentifier() : std::string(),
                    prim.GetPath());
            }
        }
    }
};

}  // namespace

std::vector<Dependency> stageDependencies(const PXR_NS::UsdStageRefPtr& stage, bool assetAttributes) {
    if (!stage) return {};
    const usdcc::assets::NoWait noWait;
    // Resolved as the stage resolves them (docs/REMOTE_ASSETS.md §10).
    const PXR_NS::ArResolverContextBinder binder(stage->GetPathResolverContext());
    Collector collector;

    for (const PXR_NS::SdfLayerHandle& layer : stage->GetUsedLayers()) {
        for (const std::string& sublayer : layer->GetSubLayerPaths()) {
            collector.add(Dependency::Sublayer, sublayer, locate(layer, sublayer), layer->GetIdentifier());
        }
    }

    auto visit = [&](const PXR_NS::UsdPrim& prim) {
        collector.addArcs(prim);
        if (assetAttributes) collector.addAssetAttributes(prim);
    };
    std::set<PXR_NS::UsdPrim> prototypes;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse(PXR_NS::UsdPrimAllPrimsPredicate)) {
        visit(prim);
        if (!prim.IsInstance() || !prototypes.insert(prim.GetPrototype()).second) continue;
        // The first instance of each prototype: its contents, as instance proxies.
        PXR_NS::UsdPrimRange contents(prim, PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimAllPrimsPredicate));
        for (auto it = ++contents.begin(); it != contents.end(); ++it) {
            visit(*it);
            if (it->IsInstance()) prototypes.insert(it->GetPrototype());
        }
    }

    std::vector<Dependency> result;
    for (auto& [key, file] : collector.files) {
        file.prims.assign(collector.prims[key].begin(), collector.prims[key].end());
        file.unloaded.assign(collector.unloaded[key].begin(), collector.unloaded[key].end());
        file.layers.assign(collector.layers[key].begin(), collector.layers[key].end());
        result.push_back(std::move(file));
    }
    std::stable_sort(result.begin(), result.end(), [](const Dependency& a, const Dependency& b) {
        if (a.missing != b.missing) return a.missing;
        return fileKey(fileName(a.resolvedPath)) < fileKey(fileName(b.resolvedPath));
    });
    return result;
}

std::string dependencyKindNames(unsigned kinds) {
    std::string names;
    for (const auto& [kind, name] : {std::pair{Dependency::Sublayer, "Sublayer"}, std::pair{Dependency::Reference, "Reference"},
                                     std::pair{Dependency::Payload, "Payload"}, std::pair{Dependency::Asset, "Asset"}}) {
        if (!(kinds & kind)) continue;
        if (!names.empty()) names += ", ";
        names += name;
    }
    return names;
}

}  // namespace usdcc::usd
