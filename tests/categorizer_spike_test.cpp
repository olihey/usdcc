// Spike for docs/ATTRIBUTE_EDITOR.md §5 rules 3 and 4: can the attribute
// editor tell, from USD's schema registry alone, which schema "owns" each
// property of a prim? Applied API schemas first (FindAppliedAPIPrimDefinition),
// then the typed schema's TfType ancestry from the most basic schema up
// (FindAbstractPrimDefinition / FindConcretePrimDefinition), each property
// going to the first schema that declares it. Prints the assignment for a few
// representative prims (run with "categorizer" as the filter to see it) and
// checks the cases the plan's category names depend on.
#include "test_harness.h"

#include <pxr/base/tf/type.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/typed.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <map>

using namespace PXR_NS;

namespace {

// property name -> owning schema name ("" if no schema declares it)
std::map<TfToken, std::string> schemaOwnership(const UsdPrim& prim) {
    const UsdSchemaRegistry& registry = UsdSchemaRegistry::GetInstance();
    std::map<TfToken, std::string> owner;

    for (const TfToken& applied : prim.GetAppliedSchemas()) {
        const auto [schemaName, instance] = UsdSchemaRegistry::GetTypeNameAndInstance(applied);
        const UsdPrimDefinition* def = registry.FindAppliedAPIPrimDefinition(schemaName);
        if (!def) continue;
        for (const TfToken& property : def->GetPropertyNames()) {
            const TfToken name =
                instance.IsEmpty() ? property : UsdSchemaRegistry::MakeMultipleApplyNameInstance(property, instance);
            owner.emplace(name, applied.GetString());
        }
    }

    std::vector<TfType> chain;
    for (TfType t = UsdSchemaRegistry::GetTypeFromSchemaTypeName(prim.GetTypeName());
         t && t != TfType::Find<UsdTyped>();) {
        chain.push_back(t);
        const std::vector<TfType> bases = t.GetBaseTypes();
        t = bases.empty() ? TfType() : bases.front();
    }
    std::reverse(chain.begin(), chain.end());
    for (const TfType& t : chain) {
        const TfToken name = UsdSchemaRegistry::GetSchemaTypeName(t);
        const UsdPrimDefinition* def = registry.FindConcretePrimDefinition(name);
        if (!def) def = registry.FindAbstractPrimDefinition(name);
        if (!def) continue;
        for (const TfToken& property : def->GetPropertyNames()) {
            owner.emplace(property, name.GetString());
        }
    }

    for (const TfToken& property : prim.GetPropertyNames()) {
        owner.emplace(property, std::string());
    }
    return owner;
}

void print(const UsdPrim& prim, const std::map<TfToken, std::string>& owner) {
    std::printf("  %s (%s)\n", prim.GetPath().GetText(), prim.GetTypeName().GetText());
    std::map<std::string, std::vector<std::string>> bySchema;
    for (const auto& [property, schema] : owner) {
        bySchema[schema.empty() ? "(no schema)" : schema].push_back(property.GetString());
    }
    for (const auto& [schema, properties] : bySchema) {
        std::string line;
        for (const auto& p : properties) line += (line.empty() ? "" : ", ") + p;
        std::printf("    %-24s %s\n", schema.c_str(), line.c_str());
    }
}

std::string ownerOf(const std::map<TfToken, std::string>& owner, const char* property) {
    auto it = owner.find(TfToken(property));
    return it == owner.end() ? "<missing>" : it->second;
}

}  // namespace

USDCC_TEST(categorizer_spike) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Mesh"));
    UsdLuxShadowAPI::Apply(mesh.GetPrim());
    UsdPhysicsRigidBodyAPI::Apply(mesh.GetPrim());
    mesh.GetPrim().CreateAttribute(TfToken("userProperties:weight"), SdfValueTypeNames->Float);
    UsdGeomSphere::Define(stage, SdfPath("/Sphere"));
    UsdGeomCamera::Define(stage, SdfPath("/Camera"));
    UsdLuxDistantLight::Define(stage, SdfPath("/Sun"));
    UsdLuxSphereLight::Define(stage, SdfPath("/Bulb"));
    UsdShadeShader shader = UsdShadeShader::Define(stage, SdfPath("/Shader"));
    shader.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float);

    std::map<std::string, std::map<TfToken, std::string>> owners;
    for (const char* path : {"/Mesh", "/Sphere", "/Camera", "/Sun", "/Bulb", "/Shader"}) {
        const UsdPrim prim = stage->GetPrimAtPath(SdfPath(path));
        owners[path] = schemaOwnership(prim);
        print(prim, owners[path]);
    }

    const auto& m = owners["/Mesh"];
    CHECK(ownerOf(m, "visibility") == "Imageable", "visibility -> Imageable (%s)", ownerOf(m, "visibility").c_str());
    CHECK(ownerOf(m, "purpose") == "Imageable", "purpose -> Imageable");
    CHECK(ownerOf(m, "xformOpOrder") == "Xformable", "xformOpOrder -> Xformable");
    CHECK(ownerOf(m, "extent") == "Boundable", "extent -> Boundable");
    CHECK(ownerOf(m, "doubleSided") == "Gprim", "doubleSided -> Gprim");
    CHECK(ownerOf(m, "primvars:displayColor") == "Gprim", "displayColor -> Gprim");
    CHECK(ownerOf(m, "points") == "PointBased", "points -> PointBased");
    CHECK(ownerOf(m, "faceVertexCounts") == "Mesh", "faceVertexCounts -> Mesh");
    CHECK(ownerOf(m, "inputs:shadow:enable") == "ShadowAPI", "shadow input -> ShadowAPI (%s)",
          ownerOf(m, "inputs:shadow:enable").c_str());
    CHECK(ownerOf(m, "physics:rigidBodyEnabled") == "PhysicsRigidBodyAPI", "rigid body -> PhysicsRigidBodyAPI (%s)",
          ownerOf(m, "physics:rigidBodyEnabled").c_str());
    CHECK(ownerOf(m, "userProperties:weight").empty(), "custom property has no schema");
    CHECK(ownerOf(owners["/Sphere"], "radius") == "Sphere", "radius -> Sphere");
    CHECK(ownerOf(owners["/Camera"], "focalLength") == "Camera", "focalLength -> Camera");
    // Shader's info:* come from its built-in NodeDefAPI, not from Shader itself.
    CHECK(ownerOf(owners["/Shader"], "info:id") == "NodeDefAPI", "info:id -> NodeDefAPI (%s)",
          ownerOf(owners["/Shader"], "info:id").c_str());
    CHECK(ownerOf(owners["/Shader"], "inputs:roughness").empty(), "shader inputs are not schema properties");
    // Lights: built-in API schemas (LightAPI) are part of GetAppliedSchemas(),
    // so their inputs land on the API rather than on the typed light.
    const std::string sunIntensity = ownerOf(owners["/Sun"], "inputs:intensity");
    CHECK(sunIntensity == "LightAPI", "light intensity -> LightAPI (%s)", sunIntensity.c_str());
    CHECK(ownerOf(owners["/Sun"], "inputs:angle") == "DistantLight", "distant light angle -> DistantLight (%s)",
          ownerOf(owners["/Sun"], "inputs:angle").c_str());
}
