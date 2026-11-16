#include "usdcc/usd/prim_selection.h"

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <set>

namespace usdcc::usd::prim_selection {

namespace {

// What the Outliner shows (usd_prim_tree_model.cpp's kChildren).
template <class Visit>
void forEachPrim(const PXR_NS::UsdStageRefPtr& stage, Visit&& visit) {
    if (!stage) return;
    const auto predicate = PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimAllPrimsPredicate);
    for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(stage->GetPseudoRoot(), predicate)) {
        if (prim.IsPseudoRoot()) continue;
        visit(prim);
    }
}

// "CollectionAPI:ferns" -> "CollectionAPI".
std::string schemaName(const std::string& applied) { return applied.substr(0, applied.find(':')); }

}  // namespace

std::vector<PXR_NS::SdfPath> allPrims(const PXR_NS::UsdStageRefPtr& stage) {
    return primsWhere(stage, [](const PXR_NS::UsdPrim&) { return true; });
}

std::vector<PXR_NS::SdfPath> primsWhere(const PXR_NS::UsdStageRefPtr& stage,
                                        const std::function<bool(const PXR_NS::UsdPrim&)>& test) {
    std::vector<PXR_NS::SdfPath> paths;
    forEachPrim(stage, [&](const PXR_NS::UsdPrim& prim) {
        if (test(prim)) paths.push_back(prim.GetPath());
    });
    return paths;
}

std::vector<PXR_NS::SdfPath> inverted(const PXR_NS::UsdStageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& selected) {
    const std::set<PXR_NS::SdfPath> skip(selected.begin(), selected.end());
    return primsWhere(stage, [&](const PXR_NS::UsdPrim& prim) { return !skip.count(prim.GetPath()); });
}

std::vector<PXR_NS::SdfPath> primsOfType(const PXR_NS::UsdStageRefPtr& stage, const std::string& type) {
    return primsWhere(stage, [&](const PXR_NS::UsdPrim& prim) { return prim.GetTypeName().GetString() == type; });
}

bool hasSchema(const PXR_NS::UsdPrim& prim, const std::string& schema) {
    const PXR_NS::TfTokenVector applied = prim.GetAppliedSchemas();
    return std::any_of(applied.begin(), applied.end(),
                       [&](const PXR_NS::TfToken& name) { return schemaName(name.GetString()) == schema; });
}

std::vector<PXR_NS::SdfPath> primsWithSchema(const PXR_NS::UsdStageRefPtr& stage, const std::string& schema) {
    return primsWhere(stage, [&](const PXR_NS::UsdPrim& prim) { return hasSchema(prim, schema); });
}

Counts count(const PXR_NS::UsdStageRefPtr& stage) {
    Counts counts;
    forEachPrim(stage, [&](const PXR_NS::UsdPrim& prim) {
        ++counts.types[prim.GetTypeName().GetString()];
        // A multiple-apply schema with two instances is still one prim with it.
        std::set<std::string> schemas;
        for (const PXR_NS::TfToken& applied : prim.GetAppliedSchemas()) schemas.insert(schemaName(applied.GetString()));
        for (const std::string& schema : schemas) ++counts.schemas[schema];
    });
    return counts;
}

}  // namespace usdcc::usd::prim_selection
