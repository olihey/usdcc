// The Material Editor's preview stage (docs/MATERIAL_EDITOR.md §7.2–§7.6).
#include "test_harness.h"

#include "usdcc/usd/shading_preview.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/output.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdcc::usd;

namespace {
const SdfPath kWood("/World/Looks/Wood");
const SdfPath kSteel("/World/Looks/Steel");

UsdStageRefPtr materials() { return UsdStage::Open(std::string(USDCC_TEST_DATA_DIR) + "/materials.usda"); }

void write(const std::string& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

// A shape with a ball for the material and a stand that keeps its own, listed in
// collection:preview when `collection`.
std::string shapeFile(const std::string& folder, bool collection) {
    const std::string file = folder + (collection ? "/with_collection.usda" : "/plain.usda");
    std::string text = R"(#usda 1.0
(
    defaultPrim = "Shape"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "Shape")";
    if (collection) text += R"( (
    prepend apiSchemas = ["CollectionAPI:preview"]
))";
    text += R"(
{)";
    if (collection) text += R"(
    rel collection:preview:includes = </Shape/Ball>)";
    text += R"(
    def Sphere "Ball" {}
    def Cube "Stand" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </Shape/Grey>
    }
    def Material "Grey" {}
}
)";
    write(file, text);
    return file;
}

SdfPath boundMaterial(const UsdStageRefPtr& stage, const SdfPath& prim) {
    const UsdShadeMaterial material = UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(prim)).ComputeBoundMaterial();
    return material ? material.GetPath() : SdfPath();
}
}  // namespace

USDCC_TEST(shading_preview_shares_the_scenes_layers) {
    UsdStageRefPtr source = materials();
    ShadingPreviewStage preview(source);
    CHECK(preview.stage() != nullptr, "a preview stage");
    CHECK(preview.stage()->GetRootLayer() == source->GetRootLayer(), "over the scene's own root layer");
    CHECK(!preview.stage()->GetPrimAtPath(kWood), "with no material yet, none is there");

    std::string why;
    CHECK(preview.setMaterial(kWood, &why), "a Material can be shown: %s", why.c_str());
    CHECK(preview.stage()->GetPrimAtPath(kWood.AppendChild(TfToken("Surface"))), "its shaders are there");
    CHECK(!preview.stage()->GetPrimAtPath(SdfPath("/World/WoodBall")), "the scene's geometry isn't");
    CHECK(!preview.stage()->GetPrimAtPath(kSteel), "nor its other materials");
    CHECK(!preview.setMaterial(SdfPath("/World/WoodBall"), &why), "a sphere isn't a material: %s", why.c_str());
    CHECK(preview.material().IsEmpty(), "and then nothing is shown");
    preview.setMaterial(kWood);

    // An edit in the scene, to its root layer and to its session layer, shows at once.
    const SdfPath roughness("/World/Looks/Wood/Surface.inputs:roughness");
    source->GetAttributeAtPath(roughness).Set(0.1f);
    float value = 0;
    preview.stage()->GetAttributeAtPath(roughness).Get(&value);
    CHECK(value == 0.1f, "a root layer edit reaches the preview: %f", value);
    {
        const UsdEditContext session(source, source->GetSessionLayer());
        source->GetAttributeAtPath(roughness).Set(0.9f);
    }
    preview.stage()->GetAttributeAtPath(roughness).Get(&value);
    CHECK(value == 0.9f, "and a session layer one: %f", value);
    CHECK(!source->GetPrimAtPath(ShadingPreviewStage::root()), "the preview's prims never reach the scene");
}

USDCC_TEST(shading_preview_binds_the_shape) {
    QTemporaryDir temp;
    const std::string folder = temp.path().toStdString();
    UsdStageRefPtr source = materials();
    ShadingPreviewStage preview(source);
    preview.setMaterial(kWood);

    std::string why;
    CHECK(preview.setShape(shapeFile(folder, false), &why), "a shape: %s", why.c_str());
    const SdfPath ball = ShadingPreviewStage::root().AppendPath(SdfPath("Shape/Asset/Ball"));
    const SdfPath stand = ShadingPreviewStage::root().AppendPath(SdfPath("Shape/Asset/Stand"));
    CHECK(boundMaterial(preview.stage(), ball) == kWood, "every gprim shows the material");
    CHECK(boundMaterial(preview.stage(), stand) == kWood, "even one with a binding of its own");
    CHECK(!preview.shapeBounds().IsEmpty(), "and it has bounds to frame");

    CHECK(preview.setShape(shapeFile(folder, true), &why), "a shape with collection:preview: %s", why.c_str());
    CHECK(boundMaterial(preview.stage(), ball) == kWood, "what it lists shows the material");
    CHECK(boundMaterial(preview.stage(), stand) == ShadingPreviewStage::root().AppendPath(SdfPath("Shape/Asset/Grey")),
          "what it doesn't keeps its own: %s", boundMaterial(preview.stage(), stand).GetText());

    const std::string broken = folder + "/no_default.usda";
    write(broken, "#usda 1.0\ndef Sphere \"Ball\" {}\n");
    CHECK(!preview.setShape(broken, &why) && !why.empty(), "a file with no defaultPrim is refused: %s", why.c_str());
    CHECK(boundMaterial(preview.stage(), ball) == kWood, "and the last shape stays");
    CHECK(!preview.setShape(folder + "/missing.usda", &why), "so is a missing one");

    preview.setMaterial(kSteel);
    CHECK(boundMaterial(preview.stage(), ball) == kSteel, "another material, on the same shape");
}

USDCC_TEST(shading_preview_scales_for_the_scenes_units) {
    QTemporaryDir temp;
    UsdStageRefPtr source = materials();  // metersPerUnit = 1
    ShadingPreviewStage preview(source);
    preview.setShape(shapeFile(temp.path().toStdString(), false));
    const double metres = preview.shapeBounds().GetSize()[1];
    source->SetMetadata(TfToken("metersPerUnit"), 0.01);
    preview.syncWithSource();
    const double centimetres = preview.shapeBounds().GetSize()[1];
    CHECK(std::abs(centimetres / metres - 100.0) < 1e-6, "a metre is 100 units in a centimetre scene: %f / %f", centimetres, metres);
}

USDCC_TEST(shading_preview_finds_the_shapes_camera) {
    QTemporaryDir temp;
    const std::string folder = temp.path().toStdString();
    UsdStageRefPtr source = materials();  // metersPerUnit = 1
    ShadingPreviewStage preview(source);
    preview.setShape(shapeFile(folder, true));
    CHECK(!preview.shapeCamera(), "a shape without a camera has none");

    // In centimetres, a camera 4 m back along +Z and 1 m up, looking along -Z; 50 mm on a
    // 24 mm high aperture. The ball, what shows the material, is a 2 cm sphere at the origin.
    const std::string file = folder + "/with_camera.usda";
    write(file, R"(#usda 1.0
(
    defaultPrim = "Shape"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "Shape" (
    prepend apiSchemas = ["CollectionAPI:preview"]
)
{
    rel collection:preview:includes = </Shape/Ball>
    def Sphere "Ball" {}
    def Cube "Floor"
    {
        double3 xformOp:translate = (0, -50, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Camera "Ortho"
    {
        token projection = "orthographic"
    }
    def Camera "Camera"
    {
        float focalLength = 50
        float verticalAperture = 24
        double3 xformOp:translate = (0, 100, 400)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
)");
    std::string why;
    CHECK(preview.setShape(file, &why), "a shape with a camera: %s", why.c_str());
    const auto camera = preview.shapeCamera();
    CHECK(camera.has_value(), "has one");
    if (!camera) return;
    CHECK(camera->path.GetName() == "Camera", "the first perspective one: %s", camera->path.GetText());
    CHECK(GfIsClose(camera->eye, GfVec3d(0, 1, 4), 1e-6), "where it is, scaled to the scene's metres: %f %f %f", camera->eye[0],
          camera->eye[1], camera->eye[2]);
    CHECK(GfIsClose(camera->target, GfVec3d(0, 1, 0), 1e-6), "turning about the ball, on its line of sight: %f %f %f", camera->target[0],
          camera->target[1], camera->target[2]);
    const double fov = 2.0 * GfRadiansToDegrees(std::atan(12.0 / 50.0));
    CHECK(std::abs(camera->verticalFov - fov) < 1e-3, "its lens: %f, not %f", camera->verticalFov, fov);
}

USDCC_TEST(shading_preview_uses_the_shapes_lights) {
    QTemporaryDir temp;
    const std::string folder = temp.path().toStdString();
    ShadingPreviewStage preview(materials());
    preview.setShape(shapeFile(folder, false));
    CHECK(!preview.shapeHasLights(), "a shape without lights has none");

    const std::string file = folder + "/lit.usda";
    write(file, R"(#usda 1.0
(
    defaultPrim = "Shape"
)

def Xform "Shape"
{
    def Sphere "Ball" {}
    def Xform "Rig"
    {
        def SphereLight "Lamp" {}
    }
    def Mesh "Glow" (
        prepend apiSchemas = ["MeshLightAPI"]
    )
    {
    }
}
)");
    std::string why;
    CHECK(preview.setShape(file, &why), "a shape with a light: %s", why.c_str());
    CHECK(preview.shapeHasLights(), "has one, however deep");

    const SdfPath dome = ShadingPreviewStage::root().AppendChild(TfToken("Env"));
    const SdfPath key = ShadingPreviewStage::root().AppendChild(TfToken("Key"));
    const auto active = [&preview](const SdfPath& path) { return preview.stage()->GetPrimAtPath(path).IsActive(); };
    CHECK(active(dome) && active(key), "the dome and, with no HDRI, the key light are on");
    ShadingPreviewStage::Environment environment;
    environment.sceneLights = true;
    preview.setEnvironment(environment);
    CHECK(!active(dome) && !active(key), "the shape's lights alone: both off");
    environment.sceneLights = false;
    environment.exposure = 1;
    preview.setEnvironment(environment);
    CHECK(active(dome) && active(key), "and on again");
    float exposure = 0;
    UsdLuxDomeLight(preview.stage()->GetPrimAtPath(dome)).GetExposureAttr().Get(&exposure);
    CHECK(exposure == 1.0f, "still written while it was off: %f", exposure);

    // An HDRI lights the shape alone: its lights go off, but a mesh light is geometry too.
    const SdfPath lamp = ShadingPreviewStage::root().AppendPath(SdfPath("Shape/Asset/Rig/Lamp"));
    const SdfPath glow = ShadingPreviewStage::root().AppendPath(SdfPath("Shape/Asset/Glow"));
    CHECK(active(lamp) && active(glow), "with the grey dome, the shape's lights are on");
    environment.hdriFile = "studio.hdr";  // only its name matters here
    preview.setEnvironment(environment);
    CHECK(!active(lamp) && active(glow), "under an HDRI, its lights are off but its mesh light");
    CHECK(preview.shapeHasLights(), "and it still has lights to offer");
    environment.sceneLights = true;
    preview.setEnvironment(environment);
    CHECK(active(lamp) && !active(dome), "Use scene lights turns them on, and the dome off");
    environment.sceneLights = false;
    preview.setEnvironment(environment);
    CHECK(!active(lamp), "off again under the HDRI");

    // Another shape, and back: what was switched off for this one doesn't stay behind.
    preview.setShape(shapeFile(folder, false));
    CHECK(!preview.shapeHasLights(), "a shape without lights");
    CHECK(preview.setShape(file, &why) && preview.shapeHasLights() && !active(lamp), "back again, its lamp off under the HDRI");
    environment.hdriFile.clear();
    preview.setEnvironment(environment);
    CHECK(active(lamp), "and on with the grey dome");
    CHECK(!preview.stage()->GetPrimAtPath(lamp).HasAuthoredActive(), "with no active opinion left behind");
}

USDCC_TEST(shading_preview_shows_one_output) {
    UsdStageRefPtr source = materials();
    const std::string before = [&]() {
        std::string text;
        source->GetRootLayer()->ExportToString(&text);
        return text;
    }();
    ShadingPreviewStage preview(source);
    std::string why;

    // MaterialX: the colour onto an unlit surface, through the mtlx terminal.
    preview.setMaterial(kSteel);
    CHECK(preview.setPreviewedOutput(SdfPath("/World/Looks/Steel/Multiply.outputs:out"), &why), "a colour output: %s", why.c_str());
    const UsdShadeMaterial steel(preview.stage()->GetPrimAtPath(kSteel));
    const UsdShadeOutput mtlx = steel.GetSurfaceOutput(TfToken("mtlx"));
    UsdShadeConnectableAPI source2;
    TfToken sourceName;
    UsdShadeAttributeType sourceType;
    CHECK(mtlx.GetConnectedSource(&source2, &sourceName, &sourceType), "the terminal is connected");
    CHECK(source2.GetPath() == kSteel.AppendChild(TfToken("__usdccPreviewNode")), "to the preview's node: %s", source2.GetPath().GetText());
    // A float, through a conversion.
    CHECK(preview.setPreviewedOutput(SdfPath("/World/Looks/Steel/Surface.outputs:out"), &why), "a surface output: %s", why.c_str());
    CHECK(mtlx.GetConnectedSource(&source2, &sourceName, &sourceType) && source2.GetPath() == kSteel.AppendChild(TfToken("Surface")),
          "a surface goes straight to the terminal");

    // USD's preview nodes: a vector output, and the other terminals blocked.
    preview.setMaterial(kWood);
    CHECK(preview.previewedOutput().IsEmpty(), "another material drops the node preview");
    CHECK(!preview.stage()->GetPrimAtPath(kSteel.AppendChild(TfToken("__usdccPreviewNode"))), "and what it wrote");
    CHECK(preview.setPreviewedOutput(SdfPath("/World/Looks/Wood/Maps/Tex.outputs:rgb"), &why), "a texture's rgb: %s", why.c_str());
    const UsdShadeMaterial wood(preview.stage()->GetPrimAtPath(kWood));
    CHECK(wood.GetSurfaceOutput().GetConnectedSource(&source2, &sourceName, &sourceType) &&
              source2.GetPath() == kWood.AppendChild(TfToken("__usdccPreviewNode")),
          "the universal terminal shows it");
    CHECK(!preview.setPreviewedOutput(SdfPath("/World/Looks/Wood/Maps/UV.outputs:result"), &why), "a float2 can't be shown with them: %s",
          why.c_str());
    CHECK(!preview.setPreviewedOutput(SdfPath("/World/Looks/Steel/Multiply.outputs:out"), &why), "nor something outside the material");

    CHECK(preview.setPreviewedOutput(SdfPath()), "back to the material");
    CHECK(wood.GetSurfaceOutput().GetConnectedSource(&source2, &sourceName, &sourceType) &&
              source2.GetPath() == kWood.AppendChild(TfToken("Surface")),
          "whose own surface shows again");
    std::string after;
    source->GetRootLayer()->ExportToString(&after);
    CHECK(after == before, "the scene's layer is untouched");
    std::string session;
    source->GetSessionLayer()->ExportToString(&session);
    CHECK(session.find("__usdcc") == std::string::npos, "and so is its session layer");
}

USDCC_TEST(shading_preview_lists_assets) {
    QTemporaryDir one, two;
    write(one.filePath("Ball.usda").toStdString(), "#usda 1.0\n");
    write(one.filePath("notes.txt").toStdString(), "");
    write(two.filePath("ball.usdc").toStdString(), "");
    write(two.filePath("Cloth.USDA").toStdString(), "");
    const std::vector<PreviewAsset> assets =
        listPreviewAssets({one.path().toStdString(), "", two.path().toStdString(), "/no/such/folder"}, previewShapeExtensions());
    CHECK(assets.size() == 2, "a file per name, USD files only: %zu", assets.size());
    if (assets.size() == 2) {
        CHECK(assets[0].name == "Ball" && assets[0].file.find(one.path().toStdString()) == 0, "the first folder wins a name");
        CHECK(assets[1].name == "Cloth", "an upper-case extension counts");
    }
}
