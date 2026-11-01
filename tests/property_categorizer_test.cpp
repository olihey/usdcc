// PropertyCategorizer: sections, their order and labels for representative
// prims (docs/ATTRIBUTE_EDITOR.md §5), plus the label helpers.
#include "test_harness.h"

#include "usdcc/usd/property_categorizer.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>

using namespace PXR_NS;
using namespace usdcc::usd;

namespace {

std::vector<std::string> keys(const std::vector<PropertyCategory>& categories) {
    std::vector<std::string> out;
    for (const auto& c : categories) out.push_back(c.key);
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

const PropertyCategory* find(const std::vector<PropertyCategory>& categories, const std::string& key) {
    for (const auto& c : categories)
        if (c.key == key) return &c;
    return nullptr;
}

bool contains(const PropertyCategory* category, const char* property) {
    if (!category) return false;
    return std::any_of(category->properties.begin(), category->properties.end(),
                       [&](const UsdProperty& p) { return p.GetName() == property; });
}

int indexOf(const std::vector<std::string>& v, const std::string& x) {
    auto it = std::find(v.begin(), v.end(), x);
    return it == v.end() ? -1 : static_cast<int>(it - v.begin());
}

}  // namespace

USDCC_TEST(property_categorizer) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();

    // Mesh with transform ops, applied APIs, a primvar and custom properties.
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Mesh"));
    mesh.AddScaleOp();
    mesh.AddTranslateOp();
    mesh.AddRotateXYZOp();
    UsdLuxShadowAPI::Apply(mesh.GetPrim());
    UsdPhysicsRigidBodyAPI::Apply(mesh.GetPrim());
    UsdGeomPrimvarsAPI(mesh).CreatePrimvar(TfToken("st"), SdfValueTypeNames->TexCoord2fArray);
    mesh.GetPrim().CreateAttribute(TfToken("userProperties:weight"), SdfValueTypeNames->Float);
    mesh.GetPrim().CreateAttribute(TfToken("notes"), SdfValueTypeNames->String);
    UsdAttribute grouped = mesh.GetPrim().CreateAttribute(TfToken("shininess"), SdfValueTypeNames->Float);
    grouped.SetDisplayGroup("Look:Advanced");

    const auto m = categorizeProperties(mesh.GetPrim());
    const auto mk = keys(m);
    std::printf("  Mesh sections: %s\n", joined(mk).c_str());
    CHECK(!mk.empty() && mk.front() == "transform", "Transform comes first");
    CHECK(indexOf(mk, "schema:Mesh") < indexOf(mk, "schema:PointBased") &&
              indexOf(mk, "schema:PointBased") < indexOf(mk, "schema:Gprim") &&
              indexOf(mk, "schema:Gprim") < indexOf(mk, "schema:Boundable") &&
              indexOf(mk, "schema:Boundable") < indexOf(mk, "schema:Imageable"),
          "typed sections most derived first");
    CHECK(indexOf(mk, "schema:Imageable") < indexOf(mk, "api:ShadowAPI") &&
              indexOf(mk, "api:ShadowAPI") < indexOf(mk, "api:PhysicsRigidBodyAPI"),
          "applied APIs after typed sections, in applied order");
    CHECK(indexOf(mk, "api:PhysicsRigidBodyAPI") < indexOf(mk, "group:Look:Advanced") &&
              indexOf(mk, "group:Look:Advanced") < indexOf(mk, "primvars") &&
              indexOf(mk, "primvars") < indexOf(mk, "ns:userProperties") &&
              indexOf(mk, "ns:userProperties") < indexOf(mk, "custom"),
          "display groups, then primvars, namespaces, custom");
    CHECK(mk.back() == "custom", "Custom comes last");

    const PropertyCategory* transform = find(m, "transform");
    CHECK(transform && transform->properties.size() == 4, "transform has 3 ops + xformOpOrder");
    if (transform && transform->properties.size() == 4) {
        CHECK(transform->properties[0].GetName() == "xformOp:scale" &&
                  transform->properties[1].GetName() == "xformOp:translate" &&
                  transform->properties[2].GetName() == "xformOp:rotateXYZ" &&
                  transform->properties[3].GetName() == "xformOpOrder",
              "transform rows follow the op order, xformOpOrder last");
    }
    CHECK(contains(find(m, "schema:Imageable"), "visibility"), "visibility in Visibility");
    CHECK(contains(find(m, "schema:Gprim"), "primvars:displayColor"), "displayColor in Display");
    CHECK(contains(find(m, "schema:PointBased"), "points"), "points in Geometry");
    CHECK(contains(find(m, "api:ShadowAPI"), "inputs:shadow:enable"), "shadow input in Shadow");
    CHECK(contains(find(m, "primvars"), "primvars:st"), "unschematized primvar in Primvars");
    CHECK(contains(find(m, "group:Look:Advanced"), "shininess"), "display group respected");
    CHECK(find(m, "schema:Imageable")->label == "Visibility" && find(m, "schema:Boundable")->label == "Bounds" &&
              find(m, "schema:Gprim")->label == "Display" && find(m, "schema:PointBased")->label == "Geometry",
          "friendly typed labels");
    CHECK(find(m, "api:PhysicsRigidBodyAPI")->label == "Physics Rigid Body", "API label: '%s'",
          find(m, "api:PhysicsRigidBodyAPI")->label.c_str());
    CHECK(find(m, "group:Look:Advanced")->label == "Look / Advanced", "nested display group label: '%s'",
          find(m, "group:Look:Advanced")->label.c_str());
    CHECK(find(m, "ns:userProperties")->label == "User Properties", "namespace label");

    // Options.
    const auto authored = categorizeProperties(mesh.GetPrim(), {/*authoredOnly=*/true, false});
    CHECK(!find(authored, "schema:Imageable"), "authored-only drops fallback-only sections");
    CHECK(contains(find(authored, "transform"), "xformOp:translate"), "authored-only keeps authored properties");
    mesh.GetPrim().GetAttribute(TfToken("notes")).SetHidden(true);
    CHECK(!find(categorizeProperties(mesh.GetPrim()), "custom"), "hidden properties dropped by default");
    CHECK(contains(find(categorizeProperties(mesh.GetPrim(), {false, true}), "custom"), "notes"),
          "includeHidden shows them");

    // Other prim types.
    const auto sphere = categorizeProperties(UsdGeomSphere::Define(stage, SdfPath("/Sphere")).GetPrim());
    CHECK(contains(find(sphere, "schema:Sphere"), "radius") && find(sphere, "schema:Sphere")->label == "Sphere",
          "Sphere section");
    const auto camera = categorizeProperties(UsdGeomCamera::Define(stage, SdfPath("/Camera")).GetPrim());
    CHECK(contains(find(camera, "schema:Camera"), "focalLength"), "Camera section");
    const auto sun = categorizeProperties(UsdLuxDistantLight::Define(stage, SdfPath("/Sun")).GetPrim());
    CHECK(contains(find(sun, "api:LightAPI"), "inputs:intensity") && find(sun, "api:LightAPI")->label == "Light",
          "light inputs in the Light section");
    // The light's built-in light-link collection gets its own section rather
    // than being claimed by the LightAPI that builds it in.
    const PropertyCategory* lightLink = find(sun, "api:CollectionAPI:lightLink");
    CHECK(contains(lightLink, "collection:lightLink:includeRoot"), "light-link collection has its own section");
    CHECK(!contains(find(sun, "api:LightAPI"), "collection:lightLink:includeRoot"), "...not in Light");
    if (lightLink) {
        CHECK(lightLink->label == "Collection (lightLink)", "collection section label");
        const UsdProperty includeRoot = sun.empty() ? UsdProperty()
                                                    : stage->GetPrimAtPath(SdfPath("/Sun")).GetProperty(
                                                          TfToken("collection:lightLink:includeRoot"));
        CHECK(propertyLabelInCategory(includeRoot, *lightLink) == "Include Root", "label within collection: '%s'",
              propertyLabelInCategory(includeRoot, *lightLink).c_str());
    }
    CHECK(contains(find(sun, "schema:DistantLight"), "inputs:angle") &&
              find(sun, "schema:DistantLight")->label == "Distant Light",
          "Distant Light section");
    UsdShadeShader shader = UsdShadeShader::Define(stage, SdfPath("/Shader"));
    shader.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float);
    shader.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token);
    const auto sh = categorizeProperties(shader.GetPrim());
    CHECK(contains(find(sh, "inputs"), "inputs:roughness"), "shader inputs in Inputs");
    CHECK(contains(find(sh, "outputs"), "outputs:surface"), "shader outputs in Outputs");
    CHECK(contains(find(sh, "api:NodeDefAPI"), "info:id") && find(sh, "api:NodeDefAPI")->label == "Node Definition",
          "info:id in Node Definition");
    const auto plain = categorizeProperties(stage->DefinePrim(SdfPath("/Plain")));
    CHECK(plain.empty(), "a typeless prim without properties has no sections");

    // Labels.
    CHECK(prettifyIdentifier("rigidBodyEnabled") == "Rigid Body Enabled", "prettify camelCase");
    CHECK(prettifyIdentifier("fStop") == "F Stop", "prettify fStop: '%s'", prettifyIdentifier("fStop").c_str());
    CHECK(prettifyIdentifier("UVSet") == "UV Set", "prettify acronym: '%s'", prettifyIdentifier("UVSet").c_str());
    CHECK(prettifyIdentifier("rotateXYZ") == "Rotate XYZ", "prettify rotateXYZ: '%s'",
          prettifyIdentifier("rotateXYZ").c_str());
    UsdPrim p = mesh.GetPrim();
    CHECK(propertyLabel(p.GetProperty(TfToken("primvars:displayColor"))) == "Display Color", "primvar label");
    UsdGeomXformOp pivot = mesh.AddTranslateOp(UsdGeomXformOp::PrecisionFloat, TfToken("pivot"));
    CHECK(propertyLabel(pivot.GetAttr()) == "Translate (pivot)", "op suffix label: '%s'",
          propertyLabel(pivot.GetAttr()).c_str());
    // Schemas can declare display names; those win over the generated label.
    CHECK(propertyLabel(p.GetProperty(TfToken("inputs:shadow:enable"))) == "Enable Shadows",
          "schema displayName: '%s'", propertyLabel(p.GetProperty(TfToken("inputs:shadow:enable"))).c_str());
    CHECK(propertyLabel(shader.GetPrim().GetProperty(TfToken("inputs:roughness"))) == "Roughness",
          "input label drops inputs:");
    // Labels relative to their section drop what the section title says.
    CHECK(propertyLabelInCategory(p.GetProperty(TfToken("userProperties:weight")), *find(m, "ns:userProperties")) ==
              "Weight",
          "namespace dropped in its own section");
    // (physics:velocity has the schema displayName "Linear Velocity"; either
    // way the section's "physics" namespace must not reappear in the label.)
    for (const UsdProperty& prop : find(m, "api:PhysicsRigidBodyAPI")->properties) {
        const std::string label = propertyLabelInCategory(prop, *find(m, "api:PhysicsRigidBodyAPI"));
        CHECK(label.rfind("Physics ", 0) != 0, "API namespace dropped: %s -> '%s'", prop.GetName().GetText(),
              label.c_str());
    }
    CHECK(propertyLabelInCategory(p.GetProperty(TfToken("points")), *find(m, "schema:PointBased")) == "Points",
          "plain name unchanged");
    grouped.SetDisplayName("Shine");
    CHECK(propertyLabel(grouped) == "Shine", "displayName wins");
    CHECK(schemaLabel(TfToken("CollectionAPI:lightLink")) == "Collection (lightLink)", "multiple-apply label: '%s'",
          schemaLabel(TfToken("CollectionAPI:lightLink")).c_str());
}
