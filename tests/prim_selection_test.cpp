// What Edit > Selection selects (docs/SELECTION_MENU.md): prim_selection's queries.
#include "test_harness.h"

#include "usdcc/usd/prim_selection.h"

#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <algorithm>
#include <string>
#include <vector>

namespace prim_selection = usdcc::usd::prim_selection;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

UsdStageRefPtr scene() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/Cube"), TfToken("Cube"));
    stage->DefinePrim(SdfPath("/World/Ball"), TfToken("Sphere"));
    stage->DefinePrim(SdfPath("/World/ball2"), TfToken("Sphere"));
    stage->DefinePrim(SdfPath("/World/Empty"));
    stage->DefinePrim(SdfPath("/Off"), TfToken("Sphere")).SetActive(false);
    UsdShadeMaterialBindingAPI::Apply(stage->GetPrimAtPath(SdfPath("/World/Cube")));
    UsdCollectionAPI::Apply(stage->GetPrimAtPath(SdfPath("/World")), TfToken("ferns"));
    UsdCollectionAPI::Apply(stage->GetPrimAtPath(SdfPath("/World")), TfToken("rocks"));
    return stage;
}

std::string joined(const std::vector<SdfPath>& paths) {
    std::string text;
    for (const SdfPath& path : paths) text += (text.empty() ? "" : " ") + path.GetString();
    return text;
}

}  // namespace

USDCC_TEST(prim_selection_all_and_invert) {
    const UsdStageRefPtr stage = scene();
    const std::vector<SdfPath> all = prim_selection::allPrims(stage);
    CHECK(joined(all) == "/World /World/Cube /World/Ball /World/ball2 /World/Empty /Off",
          "every prim, inactive ones too, in the Outliner's order: [%s]", joined(all).c_str());
    const std::vector<SdfPath> rest = prim_selection::inverted(stage, {SdfPath("/World/Cube"), SdfPath("/Off")});
    CHECK(joined(rest) == "/World /World/Ball /World/ball2 /World/Empty", "inverted: [%s]", joined(rest).c_str());
    CHECK(prim_selection::inverted(stage, all).empty(), "everything selected inverts to nothing");
    CHECK(prim_selection::allPrims(UsdStageRefPtr()).empty(), "no stage, no prims");
}

USDCC_TEST(prim_selection_by_type_and_schema) {
    const UsdStageRefPtr stage = scene();
    CHECK(joined(prim_selection::primsOfType(stage, "Sphere")) == "/World/Ball /World/ball2 /Off",
          "spheres: [%s]", joined(prim_selection::primsOfType(stage, "Sphere")).c_str());
    CHECK(joined(prim_selection::primsOfType(stage, "")) == "/World/Empty", "no type");
    CHECK(prim_selection::primsOfType(stage, "Cone").empty(), "a type nothing has");
    CHECK(joined(prim_selection::primsWithSchema(stage, "MaterialBindingAPI")) == "/World/Cube", "a schema");
    CHECK(joined(prim_selection::primsWithSchema(stage, "CollectionAPI")) == "/World", "any instance of a multiple-apply one");
    CHECK(prim_selection::primsWithSchema(stage, "Collection").empty(), "the whole name, not a prefix");

    const prim_selection::Counts counts = prim_selection::count(stage);
    CHECK(counts.types.at("Sphere") == 3 && counts.types.at("") == 1 && counts.types.at("Xform") == 1, "type counts");
    CHECK(counts.schemas.at("CollectionAPI") == 1, "two instances on one prim count once");
    CHECK(counts.schemas.at("MaterialBindingAPI") == 1 && !counts.schemas.count("CollectionAPI:ferns"), "by schema name");
}
