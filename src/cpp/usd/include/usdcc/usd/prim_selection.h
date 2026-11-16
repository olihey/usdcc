#pragma once

// What Edit > Selection selects (docs/SELECTION_MENU.md): prims of the stage picked by
// type, applied schema or a test of your own. Qt-free; the menu is selection_menu.h.

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/prim.h>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace usdcc::usd::prim_selection {

// Every prim the Outliner lists, in its order: inactive, unloaded and abstract ones,
// and those inside instances (instance proxies). Not the pseudo-root.
std::vector<PXR_NS::SdfPath> allPrims(const PXR_NS::UsdStageRefPtr& stage);

// The prims of allPrims() that `test` says yes to.
std::vector<PXR_NS::SdfPath> primsWhere(const PXR_NS::UsdStageRefPtr& stage,
                                        const std::function<bool(const PXR_NS::UsdPrim&)>& test);

// Every prim that isn't in `selected`.
std::vector<PXR_NS::SdfPath> inverted(const PXR_NS::UsdStageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& selected);

// Prims whose type is `type` exactly ("Capsule" leaves Capsule_1 out); "" for those with
// no type.
std::vector<PXR_NS::SdfPath> primsOfType(const PXR_NS::UsdStageRefPtr& stage, const std::string& type);

// Prims with the applied API schema `schema` ("MaterialBindingAPI"), whether authored or
// from their type. A multiple-apply one ("CollectionAPI") matches any of its instances.
std::vector<PXR_NS::SdfPath> primsWithSchema(const PXR_NS::UsdStageRefPtr& stage, const std::string& schema);
bool hasSchema(const PXR_NS::UsdPrim& prim, const std::string& schema);

// How many prims of allPrims() have each type, and each applied schema (by schema name,
// instances counted under it), in one pass: what the menu greys out and shows.
struct Counts {
    std::map<std::string, std::size_t> types;    // "" for no type
    std::map<std::string, std::size_t> schemas;  // "CollectionAPI", not "CollectionAPI:ferns"
};
Counts count(const PXR_NS::UsdStageRefPtr& stage);

}  // namespace usdcc::usd::prim_selection
