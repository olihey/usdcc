// allSchemas() and schemaInfo() (docs/SCHEMAS.md §4): kinds, inheritance,
// built-in and auto-applied APIs, who declares each property, and a codeless
// plugin told from one with a library.
#include "test_harness.h"

#include "usdcc/usd/schema_info.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/tokens.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>

using usdcc::usd::SchemaInfo;
using usdcc::usd::SchemaProperty;

namespace {
const SchemaProperty* findProperty(const SchemaInfo& info, const std::string& name) {
    const auto it = std::find_if(info.properties.begin(), info.properties.end(),
                                 [&](const SchemaProperty& p) { return p.name == name; });
    return it == info.properties.end() ? nullptr : &*it;
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

bool endsWith(const std::string& path, const std::string& tail) {
    return PXR_NS::TfStringEndsWith(PXR_NS::TfStringReplace(path, "\\", "/"), tail);
}
}  // namespace

USDCC_TEST(schema_info_all) {
    const std::vector<SchemaInfo> schemas = usdcc::usd::allSchemas();
    std::map<SchemaInfo::Kind, int> kinds;
    for (const SchemaInfo& info : schemas) ++kinds[info.kind];
    std::printf("  %zu schemas: %d prim types, %d abstract, %d API, %d multiple-apply, %d non-applied, %d bases\n",
                schemas.size(), kinds[SchemaInfo::Kind::ConcreteTyped], kinds[SchemaInfo::Kind::AbstractTyped],
                kinds[SchemaInfo::Kind::SingleApplyAPI], kinds[SchemaInfo::Kind::MultipleApplyAPI],
                kinds[SchemaInfo::Kind::NonAppliedAPI], kinds[SchemaInfo::Kind::AbstractBase]);
    CHECK(kinds[SchemaInfo::Kind::ConcreteTyped] >= 60 && kinds[SchemaInfo::Kind::SingleApplyAPI] >= 40 &&
              kinds[SchemaInfo::Kind::NonAppliedAPI] >= 1 && kinds[SchemaInfo::Kind::AbstractBase] == 2,
          "every kind is there");
    CHECK(std::is_sorted(schemas.begin(), schemas.end(), [](const SchemaInfo& a, const SchemaInfo& b) { return a.name < b.name; }),
          "by name");
    CHECK(std::none_of(schemas.begin(), schemas.end(), [](const SchemaInfo& s) { return s.name.empty(); }), "each has a name");
}

USDCC_TEST(schema_info_typed) {
    const SchemaInfo mesh = usdcc::usd::schemaInfo("Mesh");
    CHECK(mesh.name == "Mesh" && mesh.typeName == "UsdGeomMesh" && mesh.kind == SchemaInfo::Kind::ConcreteTyped,
          "a prim type");
    CHECK(mesh.plugin == "usdGeom" && mesh.category == "Geometry" && !mesh.codeless, "from a plugin with a library");
    CHECK(mesh.base == "PointBased", "what it inherits");
    CHECK(endsWith(mesh.sourcePath, "usdGeom/schema.usda") && std::filesystem::exists(mesh.sourcePath),
          "its plugin's schema.usda");
    CHECK(endsWith(mesh.generatedPath, "generatedSchema.usda"), "and generatedSchema.usda");
    CHECK(!mesh.documentation.empty(), "documented");

    const SchemaProperty* points = findProperty(mesh, "points");
    CHECK(points && points->typeName == "point3f[]" && points->declaredBy == "PointBased", "an inherited property's owner");
    const SchemaProperty* scheme = findProperty(mesh, "subdivisionScheme");
    CHECK(scheme && scheme->uniform && scheme->hasFallback && scheme->fallback == "\"catmullClark\"" &&
              contains(scheme->allowedTokens, "loop") && scheme->declaredBy == "Mesh",
          "its own: uniform, a quoted token fallback, allowed tokens");
    const SchemaProperty* proxy = findProperty(mesh, "proxyPrim");
    CHECK(proxy && proxy->relationship && proxy->typeName.empty() && proxy->declaredBy == "Imageable", "a relationship");

    const SchemaInfo gprim = usdcc::usd::schemaInfo("Gprim");
    CHECK(gprim.kind == SchemaInfo::Kind::AbstractTyped && gprim.base == "Boundable" && findProperty(gprim, "doubleSided"),
          "an abstract type has a definition too");
    const SchemaInfo typed = usdcc::usd::schemaInfo("Typed");
    CHECK(typed.kind == SchemaInfo::Kind::AbstractBase && typed.base.empty() && typed.category == "Core", "a root");

    const SchemaInfo light = usdcc::usd::schemaInfo("RectLight");
    CHECK(contains(light.builtInAPIs, "LightAPI") && light.autoAppliedAPIs.empty(), "a type's built-in APIs");
    const SchemaProperty* intensity = findProperty(light, "inputs:intensity");
    const SchemaProperty* width = findProperty(light, "inputs:width");
    CHECK(intensity && intensity->declaredBy == "LightAPI" && width && width->declaredBy == "RectLight",
          "a built-in API's properties are its");
    const SchemaProperty* includes = findProperty(light, "collection:lightLink:includes");
    CHECK(includes && includes->declaredBy == "CollectionAPI:lightLink", "a nested instance's are the instance's");

    const SchemaInfo capsule = usdcc::usd::schemaInfo("Capsule_1");
    CHECK(capsule.family == "Capsule" && capsule.version == 1, "a version of a family");
}

USDCC_TEST(schema_info_api) {
    const SchemaInfo collection = usdcc::usd::schemaInfo("CollectionAPI:ferns");
    CHECK(collection.name == "CollectionAPI" && collection.kind == SchemaInfo::Kind::MultipleApplyAPI,
          "an instance names its schema");
    CHECK(collection.propertyNamespacePrefix == "collection", "a multiple-apply API's prefix");
    CHECK(findProperty(collection, "collection:__INSTANCE_NAME__:includes"), "its properties as templates");
    CHECK(collection.category == "Core", "the usd plugin's are Core here");
    CHECK(collection.builtInAPIs.empty(), "not itself among its built-in APIs");
    const SchemaProperty* includes = findProperty(collection, "collection:__INSTANCE_NAME__:includes");
    CHECK(includes && includes->declaredBy == "CollectionAPI", "its properties are its own");

    const SchemaInfo meshLight = usdcc::usd::schemaInfo("MeshLightAPI");
    CHECK(contains(meshLight.builtInAPIs, "LightAPI") && !contains(meshLight.builtInAPIs, "MeshLightAPI"),
          "an API's built-in APIs, not itself");
    const SchemaProperty* intensity = findProperty(meshLight, "inputs:intensity");
    CHECK(intensity && intensity->declaredBy == "LightAPI", "a nested API's property is the nested API's");

    const SchemaInfo materialX = usdcc::usd::schemaInfo("MaterialXConfigAPI");
    CHECK(contains(materialX.canOnlyApplyTo, "Material"), "where an API can apply");

    const SchemaInfo model = usdcc::usd::schemaInfo("ModelAPI");
    CHECK(model.kind == SchemaInfo::Kind::NonAppliedAPI && !model.documentation.empty(),
          "a non-applied API, documented from its file");

    CHECK(usdcc::usd::schemaInfo("NotASchemaAPI").name.empty(), "an unknown one has no name");
    CHECK(usdcc::usd::schemaKindName(SchemaInfo::Kind::MultipleApplyAPI) == "Multiple-apply API", "kind names");
}

// Needs test_data/schemas/usdccTestSchemas registered at startup, which the
// usdcc_schema_info test does (PXR_PLUGINPATH_NAME): a schema registered later
// has no definition (docs/SCHEMAS.md §2, trap 1).
USDCC_TEST(schema_info_codeless) {
    const SchemaInfo asset = usdcc::usd::schemaInfo("UsdccTestAsset");
    if (asset.name.empty()) {
        std::printf("  skipped: the usdccTestSchemas plugin isn't registered (run the usdcc_schema_info test)\n");
        return;
    }
    CHECK(asset.kind == SchemaInfo::Kind::ConcreteTyped && asset.codeless && asset.plugin == "usdccTestSchemas" &&
              asset.category == "usdccTestSchemas",
          "a codeless plugin's prim type");
    CHECK(asset.base == "UsdccTestBase" && asset.documentation == "A test asset, with a shot built in.", "inherits, documented");
    CHECK(endsWith(asset.sourcePath, "usdccTestSchemas/usdccTestSchemas/schema.usda"), "its schema.usda");
    CHECK(asset.builtInAPIs == std::vector<std::string>({"UsdccTestShotAPI"}) &&
              asset.autoAppliedAPIs == std::vector<std::string>({"UsdccTestMarkAPI"}),
          "built-in and auto-applied APIs apart");

    const SchemaProperty* status = findProperty(asset, "usdccTest:status");
    CHECK(status && status->uniform && status->fallback == "\"wip\"" &&
              status->allowedTokens == std::vector<std::string>({"wip", "review", "final"}) &&
              status->declaredBy == "UsdccTestAsset",
          "its own property");
    const SchemaProperty* id = findProperty(asset, "usdccTest:assetId");
    CHECK(id && id->declaredBy == "UsdccTestBase" && id->displayName == "Asset ID" && id->displayGroup == "Tracking",
          "from its abstract base, with display name and group");
    const SchemaProperty* frame = findProperty(asset, "usdccTest:shot:frameStart");
    CHECK(frame && frame->declaredBy == "UsdccTestShotAPI" && frame->fallback == "1001", "from its built-in API");
    const SchemaProperty* marked = findProperty(asset, "usdccTest:marked");
    CHECK(marked && marked->declaredBy == "UsdccTestMarkAPI" && marked->fallback == "false",
          "from the API applied to it; a bool as USDA writes it");
    const SchemaProperty* visibility = findProperty(asset, "visibility");
    CHECK(visibility && visibility->declaredBy == "Imageable", "and USD's own");

    const SchemaInfo base = usdcc::usd::schemaInfo("UsdccTestBase");
    CHECK(base.kind == SchemaInfo::Kind::AbstractTyped && base.base == "Xformable", "an abstract codeless type");

    const SchemaInfo tag = usdcc::usd::schemaInfo("UsdccTestTagAPI");
    CHECK(tag.kind == SchemaInfo::Kind::MultipleApplyAPI && tag.propertyNamespacePrefix == "usdccTestTag" &&
              findProperty(tag, "usdccTestTag:__INSTANCE_NAME__:value"),
          "a codeless multiple-apply API");
    const SchemaInfo mark = usdcc::usd::schemaInfo("UsdccTestMarkAPI");
    CHECK(mark.autoApplyTo == std::vector<std::string>({"UsdccTestAsset"}) &&
              mark.canOnlyApplyTo == std::vector<std::string>({"UsdccTestAsset"}),
          "auto-apply and can-only-apply-to");
    const SchemaInfo shot = usdcc::usd::schemaInfo("UsdccTestShotAPI");
    const SchemaProperty* camera = findProperty(shot, "usdccTest:shot:camera");
    CHECK(camera && camera->relationship && camera->documentation == "The shot's camera.", "a relationship");

    const SchemaInfo mesh = usdcc::usd::schemaInfo("Mesh");
    CHECK(!mesh.codeless, "and USD's own aren't codeless");
}

// What a stage uses (docs/SCHEMAS.md §4, This stage), and applying a schema to
// several prims in one undo step.
USDCC_TEST(schema_info_stage_use) {
    using namespace PXR_NS;
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/Cube"), TfToken("Cube"));
    stage->DefinePrim(SdfPath("/Cube2"), TfToken("Cube"));
    const UsdPrim mesh = stage->DefinePrim(SdfPath("/Mesh"), TfToken("Mesh"));
    mesh.AddAppliedSchema(TfToken("MotionAPI"));
    stage->DefinePrim(SdfPath("/Light"), TfToken("RectLight"));
    const UsdPrim off = stage->DefinePrim(SdfPath("/Off"), TfToken("Xform"));
    off.SetActive(false);
    stage->CreateClassPrim(SdfPath("/_class_Cube")).SetTypeName(TfToken("Cube"));
    // An instance: its prototype's Sphere counts, through the instance proxy.
    stage->DefinePrim(SdfPath("/Proto"), TfToken("Scope"));
    stage->DefinePrim(SdfPath("/Proto/Ball"), TfToken("Sphere"));
    const UsdPrim instance = stage->DefinePrim(SdfPath("/Instance"));
    instance.GetReferences().AddInternalReference(SdfPath("/Proto"));
    instance.SetInstanceable(true);
    // Names USD doesn't know.
    stage->DefinePrim(SdfPath("/Thing"), TfToken("StudioThing"));
    const UsdPrim tagged = stage->DefinePrim(SdfPath("/Tagged"), TfToken("Xform"));
    tagged.AddAppliedSchema(TfToken("OldRigAPI"));
    tagged.AddAppliedSchema(TfToken("OldTagAPI:a"));
    tagged.AddAppliedSchema(TfToken("OldTagAPI:b"));

    const usdcc::usd::StageSchemaUse use = usdcc::usd::stageSchemaUse(stage);
    CHECK(use.count("Cube") == 2, "a type's prims, not its class: [%zu]", use.count("Cube"));
    CHECK(use.count("MotionAPI") == 1 && use.prims.at("MotionAPI").front() == SdfPath("/Mesh"), "an applied API's");
    CHECK(use.count("LightAPI") == 1, "a built-in API counts");
    CHECK(use.count("CollectionAPI") == 1, "two instances of one schema count the prim once");
    CHECK(use.count("Xform") == 2, "an inactive prim counts");
    CHECK(use.count("Sphere") == 2, "inside an instance too, and its prototype source: [%zu]", use.count("Sphere"));
    CHECK(use.unregistered.count("StudioThing") && use.unregistered.at("StudioThing").front() == SdfPath("/Thing"),
          "an unknown type");
    CHECK(use.composesAs.count("StudioThing") && use.composesAs.at("StudioThing").empty(), "composing as nothing");
    CHECK(use.unregistered.count("OldRigAPI") && use.unregistered.count("OldTagAPI") &&
              use.unregistered.at("OldTagAPI").size() == 1,
          "unknown APIs, an instance's under its schema, once a prim");
    CHECK(!use.composesAs.count("OldRigAPI"), "an API composes as nothing in particular");
    CHECK(usdcc::usd::stageSchemaUse(nullptr).prims.empty(), "no stage, nothing");

    // A fallback type: the prim composes as it.
    const UsdStageRefPtr fallback = UsdStage::CreateInMemory();
    VtDictionary fallbacks;
    fallbacks["StudioAsset"] = VtTokenArray{TfToken("Xform")};
    fallback->SetMetadata(TfToken("fallbackPrimTypes"), fallbacks);
    fallback->DefinePrim(SdfPath("/A"), TfToken("StudioAsset"));
    const usdcc::usd::StageSchemaUse fallbackUse = usdcc::usd::stageSchemaUse(fallback);
    CHECK(fallbackUse.composesAs.count("StudioAsset") && fallbackUse.composesAs.at("StudioAsset") == "Xform",
          "an unknown type with a fallback composes as it");
}
