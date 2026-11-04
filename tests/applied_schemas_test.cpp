// appliedSchemas(), schemaCatalog() and orphanedProperties()
// (docs/APPLIED_SCHEMAS.md): built-in, nested and unregistered schemas; the
// categories; what a removal leaves behind, with a shared property, instances
// and another layer.
#include "test_harness.h"

#include "usdcc/usd/applied_schemas.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>

using usdcc::usd::AppliedSchema;
using usdcc::usd::SchemaCatalogEntry;
using usdcc::usd::SchemaCategory;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const AppliedSchema* findSchema(const std::vector<AppliedSchema>& schemas, const char* name) {
    const auto it = std::find_if(schemas.begin(), schemas.end(), [&](const AppliedSchema& s) { return s.name == name; });
    return it == schemas.end() ? nullptr : &*it;
}

const SchemaCatalogEntry* findEntry(const std::vector<SchemaCategory>& catalog, const char* category, const char* name) {
    for (const SchemaCategory& c : catalog) {
        if (c.name != category) continue;
        for (const SchemaCatalogEntry& entry : c.schemas) {
            if (entry.name == name) return &entry;
        }
    }
    return nullptr;
}

bool has(const TfTokenVector& tokens, const char* name) {
    return std::find(tokens.begin(), tokens.end(), TfToken(name)) != tokens.end();
}
}  // namespace

USDCC_TEST(applied_schemas_states) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();

    const UsdPrim light = stage->DefinePrim(SdfPath("/L"), TfToken("RectLight"));
    std::vector<AppliedSchema> schemas = usdcc::usd::appliedSchemas(light);
    const AppliedSchema* lightApi = findSchema(schemas, "LightAPI");
    CHECK(lightApi && lightApi->state == AppliedSchema::State::BuiltIn && lightApi->cameFrom == "RectLight",
          "a type's schema is built in, from the type");

    const UsdPrim mesh = stage->DefinePrim(SdfPath("/M"), TfToken("Mesh"));
    mesh.AddAppliedSchema(TfToken("MeshLightAPI"));
    schemas = usdcc::usd::appliedSchemas(mesh);
    CHECK(findSchema(schemas, "MeshLightAPI") && findSchema(schemas, "MeshLightAPI")->state == AppliedSchema::State::Authored,
          "an authored one can be removed");
    const AppliedSchema* nested = findSchema(schemas, "LightAPI");
    CHECK(nested && nested->state == AppliedSchema::State::BuiltIn && nested->cameFrom == "MeshLightAPI",
          "one it brings is built in, from it");

    const UsdPrim unknown = stage->DefinePrim(SdfPath("/U"), TfToken("Xform"));
    unknown.AddAppliedSchema(TfToken("NotARealAPI"));
    schemas = usdcc::usd::appliedSchemas(unknown);
    CHECK(schemas.size() == 1 && schemas[0].name == "NotARealAPI" && schemas[0].state == AppliedSchema::State::Unregistered,
          "an unregistered one is listed, though GetAppliedSchemas() leaves it out");
    CHECK(usdcc::usd::appliedSchemas(UsdPrim()).empty(), "no prim: none");
}

USDCC_TEST(applied_schemas_catalog) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdPrim prim = stage->DefinePrim(SdfPath("/X"), TfToken("Xform"));
    prim.AddAppliedSchema(TfToken("MotionAPI"));
    const std::vector<SchemaCategory> catalog = usdcc::usd::schemaCatalog(prim);

    CHECK(std::is_sorted(catalog.begin(), catalog.end(), [](const SchemaCategory& a, const SchemaCategory& b) { return a.name < b.name; }),
          "categories by name");
    const SchemaCatalogEntry* rigid = findEntry(catalog, "Physics", "PhysicsRigidBodyAPI");
    CHECK(rigid && rigid->available && !rigid->documentation.empty(), "an available one, with its documentation");
    const SchemaCatalogEntry* motion = findEntry(catalog, "Geometry", "MotionAPI");
    CHECK(motion && !motion->available && motion->whyNot.find("Already") != std::string::npos, "one already applied");
    const SchemaCatalogEntry* mtlx = findEntry(catalog, "Shading", "MaterialXConfigAPI");
    CHECK(mtlx && !mtlx->available && mtlx->whyNot.find("Material") != std::string::npos,
          "one that can't apply here, with USD's reason: [%s]", mtlx ? mtlx->whyNot.c_str() : "");
    CHECK(findEntry(catalog, "Shading", "MaterialBindingAPI") && findEntry(catalog, "Shading", "RiMaterialAPI"),
          "Shading gathers its three plugins");
    const SchemaCatalogEntry* collection = findEntry(catalog, "Collections & Color", "CollectionAPI");
    CHECK(collection && collection->multipleApply && collection->available, "a multiple-apply one is always available");
    bool nonApplied = false;
    for (const SchemaCategory& category : catalog) {
        for (const SchemaCatalogEntry& entry : category.schemas) {
            nonApplied = nonApplied || entry.name == "PrimvarsAPI" || entry.name == "ModelAPI";
        }
    }
    CHECK(!nonApplied, "non-applied schemas are left out");

    std::string why;
    CHECK(usdcc::usd::canApplyInstance(prim, "CollectionAPI", "ferns", &why), "a good instance name: [%s]", why.c_str());
    CHECK(!usdcc::usd::canApplyInstance(prim, "CollectionAPI", "not valid", &why), "an invalid name is refused");
    prim.AddAppliedSchema(TfToken("CollectionAPI:ferns"));
    CHECK(!usdcc::usd::canApplyInstance(prim, "CollectionAPI", "ferns", &why) && why.find("already") != std::string::npos,
          "a used name is refused: [%s]", why.c_str());
    const auto names = usdcc::usd::schemaPropertyNames("CollectionAPI:ferns");
    CHECK(std::find(names.begin(), names.end(), "collection:ferns:includes") != names.end(), "an instance's properties");
}

USDCC_TEST(applied_schemas_attributes_to_add) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdPrim prim = stage->DefinePrim(SdfPath("/X"), TfToken("Mesh"));
    auto contains = [](const std::vector<TfToken>& names, const char* name) {
        return std::find(names.begin(), names.end(), TfToken(name)) != names.end();
    };
    std::vector<TfToken> names = usdcc::usd::schemaAttributesToAdd(prim, "PhysicsRigidBodyAPI");
    CHECK(contains(names, "physics:velocity") && contains(names, "physics:mass") == false, "its own attributes");
    CHECK(!contains(names, "physics:simulationOwner"), "no relationships");
    CHECK(usdcc::usd::schemaAttributesToAdd(prim, "MaterialBindingAPI").empty(), "a schema with none");
    CHECK(usdcc::usd::schemaAttributesToAdd(prim, "NotARealAPI").empty(), "an unregistered one");

    names = usdcc::usd::schemaAttributesToAdd(prim, "MeshLightAPI");
    CHECK(contains(names, "inputs:intensity"), "a nested schema's too");
    names = usdcc::usd::schemaAttributesToAdd(prim, "CollectionAPI:ferns");
    CHECK(contains(names, "collection:ferns:expansionRule") && !contains(names, "collection:ferns:includes"),
          "an instance's, by its name");

    // One already authored stays out of the list.
    prim.AddAppliedSchema(TfToken("PhysicsMassAPI"));
    prim.GetAttribute(TfToken("physics:mass")).Set(5.0f);
    names = usdcc::usd::schemaAttributesToAdd(prim, "PhysicsMassAPI");
    CHECK(!contains(names, "physics:mass") && contains(names, "physics:density"), "only what isn't authored yet");
}

USDCC_TEST(applied_schemas_orphans) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdPrim prim = stage->DefinePrim(SdfPath("/R"), TfToken("Xform"));
    prim.AddAppliedSchema(TfToken("PhysicsRigidBodyAPI"));
    prim.AddAppliedSchema(TfToken("PhysicsMassAPI"));
    prim.GetAttribute(TfToken("physics:velocity")).Set(GfVec3f(1, 2, 3));
    prim.GetAttribute(TfToken("physics:mass")).Set(5.0f);
    prim.CreateAttribute(TfToken("custom:mine"), SdfValueTypeNames->Float).Set(1.0f);

    const TfTokenVector schemasBefore = prim.GetAppliedSchemas();
    usdcc::usd::OrphanedProperties orphans = usdcc::usd::orphanedProperties(prim, "PhysicsRigidBodyAPI");
    CHECK(orphans.inEditTarget == TfTokenVector({TfToken("physics:velocity")}) && orphans.inOtherLayers.empty(),
          "only what's authored and only its own: [%zu]", orphans.inEditTarget.size());
    CHECK(prim.GetAppliedSchemas() == schemasBefore, "asking changes nothing");

    // A property two schemas define isn't orphaned by removing one.
    const UsdPrim both = stage->DefinePrim(SdfPath("/B"), TfToken("Mesh"));
    both.AddAppliedSchema(TfToken("MeshLightAPI"));
    both.AddAppliedSchema(TfToken("LightAPI"));
    both.GetAttribute(TfToken("inputs:intensity")).Set(2.0f);
    CHECK(usdcc::usd::orphanedProperties(both, "MeshLightAPI").empty(), "LightAPI still defines inputs:intensity");

    // An instance's properties.
    const UsdPrim collection = stage->DefinePrim(SdfPath("/C"), TfToken("Xform"));
    collection.AddAppliedSchema(TfToken("CollectionAPI:ferns"));
    collection.AddAppliedSchema(TfToken("CollectionAPI:rocks"));
    collection.GetRelationship(TfToken("collection:ferns:includes")).AddTarget(SdfPath("/R"));
    collection.GetRelationship(TfToken("collection:rocks:includes")).AddTarget(SdfPath("/R"));
    orphans = usdcc::usd::orphanedProperties(collection, "CollectionAPI:ferns");
    CHECK(orphans.inEditTarget == TfTokenVector({TfToken("collection:ferns:includes")}), "that instance's only");

    // Bindings, which MaterialBindingAPI doesn't define.
    const UsdPrim bound = stage->DefinePrim(SdfPath("/M"), TfToken("Mesh"));
    bound.AddAppliedSchema(TfToken("MaterialBindingAPI"));
    bound.CreateRelationship(TfToken("material:binding")).AddTarget(SdfPath("/Mat"));
    bound.CreateRelationship(TfToken("material:binding:preview")).AddTarget(SdfPath("/Mat"));
    bound.CreateAttribute(TfToken("material:bindingOther"), SdfValueTypeNames->Float).Set(1.0f);
    CHECK(usdcc::usd::orphanedProperties(bound, "MaterialBindingAPI").inEditTarget.size() == 2,
          "its bindings count, and nothing else");

    // The prim's only authored schema: its type's properties are never orphans.
    const UsdPrim only = stage->DefinePrim(SdfPath("/Only"), TfToken("Mesh"));
    only.AddAppliedSchema(TfToken("MaterialBindingAPI"));
    only.CreateRelationship(TfToken("material:binding")).AddTarget(SdfPath("/Mat"));
    only.GetAttribute(TfToken("points")).Set(VtVec3fArray{GfVec3f(0, 0, 0)});
    only.GetAttribute(TfToken("faceVertexCounts")).Set(VtIntArray{3});
    orphans = usdcc::usd::orphanedProperties(only, "MaterialBindingAPI");
    CHECK(orphans.inEditTarget == TfTokenVector({TfToken("material:binding")}),
          "only the binding, not the mesh's points: [%zu]", orphans.inEditTarget.size());
    const UsdPrim onlyRigid = stage->DefinePrim(SdfPath("/OnlyRigid"), TfToken("Mesh"));
    onlyRigid.AddAppliedSchema(TfToken("PhysicsRigidBodyAPI"));
    onlyRigid.GetAttribute(TfToken("physics:velocity")).Set(GfVec3f(1, 2, 3));
    onlyRigid.GetAttribute(TfToken("points")).Set(VtVec3fArray{GfVec3f(0, 0, 0)});
    orphans = usdcc::usd::orphanedProperties(onlyRigid, "PhysicsRigidBodyAPI");
    CHECK(orphans.inEditTarget == TfTokenVector({TfToken("physics:velocity")}), "its own, not the type's: [%zu]",
          orphans.inEditTarget.size());
    // A typeless prim has no type's properties to keep.
    const UsdPrim typeless = stage->DefinePrim(SdfPath("/Typeless"));
    typeless.AddAppliedSchema(TfToken("MotionAPI"));
    typeless.GetAttribute(TfToken("motion:velocityScale")).Set(2.0f);
    CHECK(usdcc::usd::orphanedProperties(typeless, "MotionAPI").inEditTarget ==
              TfTokenVector({TfToken("motion:velocityScale")}),
          "a typeless prim's only schema");

    // Authored in a weaker layer: can't be removed from the edit target.
    const SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    const UsdStageRefPtr layered = UsdStage::CreateInMemory();
    layered->GetRootLayer()->GetSubLayerPaths().push_back(weak->GetIdentifier());
    layered->SetEditTarget(weak);
    const UsdPrim p = layered->DefinePrim(SdfPath("/P"), TfToken("Xform"));
    p.AddAppliedSchema(TfToken("MotionAPI"));
    p.GetAttribute(TfToken("motion:velocityScale")).Set(2.0f);
    layered->SetEditTarget(layered->GetRootLayer());
    orphans = usdcc::usd::orphanedProperties(p, "MotionAPI");
    CHECK(orphans.inEditTarget.empty() && orphans.inOtherLayers == TfTokenVector({TfToken("motion:velocityScale")}),
          "another layer's property is counted apart");
}
