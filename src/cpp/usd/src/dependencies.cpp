#include "usdcc/usd/dependencies.h"

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

// Where `assetPath`, named in `anchor`, points: the file it resolves to
// (an open layer counts, as in compositionArcs()), else the anchored path,
// with `found` saying which.
std::string locate(const PXR_NS::SdfLayerHandle& anchor, const std::string& assetPath, bool* found) {
    const std::string anchored = anchor ? PXR_NS::SdfComputeAssetPathRelativeToLayer(anchor, assetPath) : assetPath;
    if (const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(anchored)) {
        *found = true;
        return open->GetRealPath().empty() ? open->GetIdentifier() : open->GetRealPath();
    }
    const std::string resolved = PXR_NS::ArGetResolver().Resolve(anchored).GetPathString();
    *found = !resolved.empty();
    return *found ? resolved : anchored;
}

// One spelling per file: forward slashes, and no case on Windows.
std::string fileKey(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
#ifdef _WIN32
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

    void add(Dependency::Kind kind, const std::string& assetPath, const std::string& located, bool found,
             const std::string& layer, const PXR_NS::SdfPath& prim = {}, bool loaded = true) {
        const std::string key = fileKey(located);
        Dependency& file = files[key];
        if (file.kinds == 0) {
            file.assetPath = assetPath;
            file.resolvedPath = located;
            file.missing = !found;
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
            bool found = !arc.missing;
            const std::string located =
                arc.missing ? locate(PXR_NS::SdfLayer::Find(arc.layerIdentifier), arc.assetPath, &found) : arc.resolvedPath;
            add(arc.type == ArcInfo::Type::Payload ? Dependency::Payload : Dependency::Reference, arc.assetPath, located,
                found, arc.layerIdentifier, prim.GetPath(), arc.type != ArcInfo::Type::Payload || arc.loaded);
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
                bool found = !path.GetResolvedPath().empty();
                std::string located = path.GetResolvedPath();
                if (!found) located = locate(layer, path.GetAssetPath(), &found);
                // A UDIM set ("tex.<UDIM>.png") resolves per tile, never as a whole.
                if (!found && path.GetAssetPath().find('<') != std::string::npos) found = true;
                add(Dependency::Asset, path.GetAssetPath(), located, found, layer ? layer->GetIdentifier() : std::string(),
                    prim.GetPath());
            }
        }
    }
};

}  // namespace

std::vector<Dependency> stageDependencies(const PXR_NS::UsdStageRefPtr& stage, bool assetAttributes) {
    if (!stage) return {};
    Collector collector;

    for (const PXR_NS::SdfLayerHandle& layer : stage->GetUsedLayers()) {
        for (const std::string& sublayer : layer->GetSubLayerPaths()) {
            bool found = false;
            const std::string located = locate(layer, sublayer, &found);
            collector.add(Dependency::Sublayer, sublayer, located, found, layer->GetIdentifier());
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
