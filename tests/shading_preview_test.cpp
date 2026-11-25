// The Material Editor's preview stage (docs/MATERIAL_EDITOR.md §7.2–§7.6).
#include "test_harness.h"

#include "usdcc/usd/shading_preview.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
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
    CHECK(preview.syncWithSource() == 1.0, "the same units scale nothing");
    source->SetMetadata(TfToken("metersPerUnit"), 0.01);
    const double factor = preview.syncWithSource();
    const double centimetres = preview.shapeBounds().GetSize()[1];
    CHECK(std::abs(centimetres / metres - 100.0) < 1e-6, "a metre is 100 units in a centimetre scene: %f / %f", centimetres, metres);
    CHECK(std::abs(factor - 100.0) < 1e-6, "and it says how much bigger the shape got: %f", factor);
    CHECK(preview.syncWithSource() == 1.0, "once");
    source->SetMetadata(TfToken("metersPerUnit"), 1.0);
    const double back = preview.syncWithSource();
    CHECK(std::abs(back - 0.01) < 1e-9 && std::abs(preview.shapeBounds().GetSize()[1] - metres) < 1e-6, "and back: %f", back);
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

// Storm takes an area light's power from its width, height and so on, without its transform's
// scale, but its distance with it: only a light with no scale left lights the same.
USDCC_TEST(shading_preview_sizes_the_shapes_lights) {
    QTemporaryDir temp;
    const std::string folder = temp.path().toStdString();
    UsdStageRefPtr source = materials();  // metersPerUnit = 1
    ShadingPreviewStage preview(source);
    const std::string file = folder + "/lit.usda";
    write(file, R"(#usda 1.0
(
    defaultPrim = "Shape"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "Shape"
{
    def Sphere "Ball" {}
    def Xform "Rig"
    {
        double3 xformOp:translate = (0, 100, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def RectLight "Rect"
        {
            float3[] extent = [(-10, -20, 0), (10, 20, 0)]
            float inputs:width = 20
            float inputs:height = 40
            float inputs:intensity = 9
            double3 xformOp:translate = (50, 0, 0)
            uniform token[] xformOpOrder = ["xformOp:translate"]
            def Scope "Parts"
            {
                def Cube "Housing"
                {
                    double3 xformOp:translate = (0, 0, 10)
                    uniform token[] xformOpOrder = ["xformOp:translate"]
                }
            }
        }
    }
    def SphereLight "Bulb"
    {
        bool inputs:normalize = 1
        float inputs:intensity = 5
        float inputs:intensity.timeSamples = { 1: 5, 2: 10 }
    }
    def CylinderLight "Tube" {}
    def DistantLight "Sun"
    {
        bool inputs:normalize = 1
        float inputs:intensity = 3
    }
    def Mesh "Glow" (
        prepend apiSchemas = ["MeshLightAPI"]
    )
    {
        bool inputs:normalize = 1
        float inputs:intensity = 7
    }
}
)");
    std::string why;
    CHECK(preview.setShape(file, &why), "a lit shape in centimetres: %s", why.c_str());

    const SdfPath asset = ShadingPreviewStage::root().AppendPath(SdfPath("Shape/Asset"));
    const auto prim = [&](const char* path) { return preview.stage()->GetPrimAtPath(asset.AppendPath(SdfPath(path))); };
    const auto value = [&](const char* path, const char* name, UsdTimeCode time = UsdTimeCode::Default()) {
        float held = -1;
        prim(path).GetAttribute(TfToken(name)).Get(&held, time);
        return static_cast<double>(held);
    };
    const auto world = [&](const char* path) {
        return UsdGeomXformable(prim(path)).ComputeLocalToWorldTransform(UsdTimeCode::Default());
    };
    const auto scaleOf = [&](const char* path) { return world(path).TransformDir(GfVec3d(1, 0, 0)).GetLength(); };
    const auto close = [](double a, double b) { return std::abs(a - b) <= 1e-5 * std::abs(b); };

    // A second time after another shape in between, which leaves nothing to scale again.
    for (int round = 0; round < 2; ++round) {
        CHECK(close(scaleOf("Rig/Rect"), 1) && close(scaleOf("Bulb"), 1) && close(scaleOf("Tube"), 1),
              "no scale on an area light in a metre scene: %f %f %f", scaleOf("Rig/Rect"), scaleOf("Bulb"), scaleOf("Tube"));
        CHECK(close(value("Rig/Rect", "inputs:width"), 0.2) && close(value("Rig/Rect", "inputs:height"), 0.4), "its size in metres: %f %f",
              value("Rig/Rect", "inputs:width"), value("Rig/Rect", "inputs:height"));
        CHECK(GfIsClose(world("Rig/Rect").ExtractTranslation(), GfVec3d(0.5, 1, 0), 1e-6), "where it was");
        CHECK(close(value("Rig/Rect", "inputs:intensity"), 9), "its intensity too: %f", value("Rig/Rect", "inputs:intensity"));
        CHECK(close(value("Bulb", "inputs:radius"), 0.005) && close(value("Tube", "inputs:length"), 0.01), "a size nobody authored: %f %f",
              value("Bulb", "inputs:radius"), value("Tube", "inputs:length"));
        VtVec3fArray extent;
        prim("Rig/Rect").GetAttribute(TfToken("extent")).Get(&extent);
        CHECK(extent.size() == 2 && GfIsClose(extent[1], GfVec3f(0.1f, 0.2f, 0), 1e-6), "and an authored extent");
        CHECK(close(scaleOf("Rig/Rect/Parts/Housing"), 0.01) &&
                  GfIsClose(world("Rig/Rect/Parts/Housing").ExtractTranslation(), GfVec3d(0.5, 1, 0.1), 1e-6),
              "what's under a light keeps the shape's scale: %f", scaleOf("Rig/Rect/Parts/Housing"));
        CHECK(close(value("Bulb", "inputs:intensity"), 5e-4) && close(value("Bulb", "inputs:intensity", 2), 1e-3),
              "normalized, its intensity goes by the distance squared, animated too: %g %g", value("Bulb", "inputs:intensity"),
              value("Bulb", "inputs:intensity", 2));
        CHECK(close(value("Glow", "inputs:intensity"), 7e-4) && close(scaleOf("Glow"), 0.01), "a normalized mesh light's too, its mesh as it was");
        CHECK(close(value("Sun", "inputs:intensity"), 3), "not a distant light's: %f", value("Sun", "inputs:intensity"));
        preview.setShape(shapeFile(folder, false));
        preview.setShape(file);
    }

    // Off under an HDRI when the units change, the lights follow them all the same.
    ShadingPreviewStage::Environment environment;
    environment.hdriFile = "studio.hdr";  // only its name matters here
    preview.setEnvironment(environment);
    source->SetMetadata(TfToken("metersPerUnit"), 0.1);
    preview.syncWithSource();
    CHECK(!prim("Rig/Rect").IsActive(), "off under the HDRI");
    environment.sceneLights = true;
    preview.setEnvironment(environment);
    CHECK(close(scaleOf("Rig/Rect"), 1) && close(value("Rig/Rect", "inputs:width"), 2), "on again, in decimetres: %f", value("Rig/Rect", "inputs:width"));

    // In the shape's own units, it's as its file has it.
    source->SetMetadata(TfToken("metersPerUnit"), 0.01);
    preview.syncWithSource();
    CHECK(close(value("Rig/Rect", "inputs:width"), 20) && close(scaleOf("Rig/Rect"), 1), "a centimetre scene: %f", value("Rig/Rect", "inputs:width"));
    CHECK(close(value("Bulb", "inputs:intensity"), 5) && close(value("Bulb", "inputs:intensity", 2), 10), "the normalized one: %f",
          value("Bulb", "inputs:intensity"));
    CHECK(close(scaleOf("Rig/Rect/Parts/Housing"), 1) &&
              GfIsClose(world("Rig/Rect/Parts/Housing").ExtractTranslation(), GfVec3d(50, 100, 10), 1e-4),
          "what's under a light");
    bool resets = false;
    CHECK(UsdGeomXformable(prim("Rig/Rect")).GetOrderedXformOps(&resets).size() == 1, "with no op left behind");
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

namespace {
// A quad with `uvSets`, all but the first mirrored.
std::string quad(const std::string& name, const std::vector<std::string>& uvSets, const std::string& more = {}) {
    std::string out = "    def Mesh \"" + name + R"(" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
)";
    for (std::size_t i = 0; i < uvSets.size(); ++i) {
        const std::string values = i == 0 ? "[(0, 0), (1, 1)]" : "[(1, 1), (0, 0)]";
        out += "        texCoord2f[] primvars:" + uvSets[i] + " = " + values + " (\n            interpolation = \"faceVarying\"\n        )\n";
        out += "        int[] primvars:" + uvSets[i] + ":indices = [0, 1, 1, 0]\n";
    }
    return out + more + "    }\n";
}

std::string shapeWith(const std::string& folder, const std::string& name, const std::string& content, const std::string& meta = {}) {
    const std::string file = folder + "/" + name + ".usda";
    write(file, "#usda 1.0\n(\n    defaultPrim = \"Shape\"\n)\n\ndef Xform \"Shape\"" + meta + "\n{\n" + content + "}\n");
    return file;
}

// A material reading uv1 through its interface, the primary set by MaterialX's own texcoord,
// uv2 from a MaterialX node, dirt as a colour, and an unconnected reader of uv3.
const char* kReaders = R"(#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def Material "Mat"
    {
        string inputs:uvName = "uv1"
        token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
        token outputs:mtlx:surface.connect = </World/Mat/MxSurface.outputs:out>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Mat/Tex.outputs:rgb>
            color3f inputs:emissiveColor.connect = </World/Mat/Dirt.outputs:result>
            token outputs:surface
        }
        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            float2 inputs:st.connect = </World/Mat/UV.outputs:result>
            float3 outputs:rgb
        }
        def Shader "UV"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname.connect = </World/Mat.inputs:uvName>
            float2 outputs:result
        }
        def Shader "Dirt"
        {
            uniform token info:id = "UsdPrimvarReader_float3"
            string inputs:varname = "dirt"
            float3 outputs:result
        }
        def Shader "Loose"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "uv3"
            float2 outputs:result
        }
        def Shader "MxSurface"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color.connect = </World/Mat/Image.outputs:out>
            color3f inputs:emission_color.connect = </World/Mat/Checker.outputs:out>
            token outputs:out
        }
        def Shader "Image"
        {
            uniform token info:id = "ND_image_color3"
            float2 inputs:texcoord.connect = </World/Mat/Geom.outputs:out>
            color3f outputs:out
        }
        def Shader "Checker"
        {
            uniform token info:id = "ND_checkerboard_color3"
            color3f outputs:out
        }
        def Shader "Geom"
        {
            uniform token info:id = "ND_geompropvalue_vector2"
            string inputs:geomprop = "uv2"
            float2 outputs:out
        }
    }
    def Material "Plain" {}
}
)";

UsdStageRefPtr stageFrom(const std::string& text) {
    const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("scene.usda");
    CHECK(layer->ImportFromString(text), "the test's scene is valid");
    return UsdStage::Open(layer);
}

std::string exported(const UsdStageRefPtr& stage) {
    std::string root, session;
    stage->GetRootLayer()->ExportToString(&root);
    stage->GetSessionLayer()->ExportToString(&session);
    return root + session;
}

using Names = std::vector<std::string>;
std::string joined(const Names& names) {
    std::string out;
    for (const std::string& name : names) out += (out.empty() ? "" : " ") + name;
    return out;
}

SdfPath assetChild(const char* name) { return ShadingPreviewStage::root().AppendPath(SdfPath("Shape/Asset")).AppendPath(SdfPath(name)); }
}  // namespace

USDCC_TEST(shading_preview_knows_what_a_material_reads) {
    UsdStageRefPtr source = stageFrom(kReaders);
    const auto reads = primvarsReadBy(UsdShadeMaterial(source->GetPrimAtPath(SdfPath("/World/Mat"))));
    const auto find = [&reads](const std::string& name) -> const MaterialPrimvar* {
        for (const MaterialPrimvar& read : reads) {
            if (read.name == name) return &read;
        }
        return nullptr;
    };
    CHECK(reads.size() == 5, "uv1, uv2, uv3, st and dirt: %zu", reads.size());
    for (const char* name : {"uv1", "uv2", "st"}) CHECK(find(name) && find(name)->uv && find(name)->reached, "%s is a UV set the material shows", name);
    CHECK(find("uv3") && find("uv3")->uv && !find("uv3")->reached, "uv3 is one it doesn't show");
    CHECK(find("dirt") && !find("dirt")->uv, "dirt isn't a UV set");

    // The checkerboard alone reads st: the image is told where by its texcoord.
    source->RemovePrim(SdfPath("/World/Mat/Checker"));
    for (const MaterialPrimvar& read : primvarsReadBy(UsdShadeMaterial(source->GetPrimAtPath(SdfPath("/World/Mat"))))) {
        CHECK(read.name != "st", "a connected texcoord isn't st");
    }
}

USDCC_TEST(shading_preview_fills_in_uv_sets) {
    QTemporaryDir temp;
    const std::string folder = temp.path().toStdString();
    UsdStageRefPtr source = stageFrom(kReaders);
    const std::string before = exported(source);
    ShadingPreviewStage preview(source);
    std::string why;
    CHECK(preview.setShape(shapeWith(folder, "quad", quad("Quad", {"st"})), &why), "a shape with one UV set: %s", why.c_str());
    CHECK(preview.filledUvSets().empty(), "without a material, nothing's filled in");
    const UsdGeomPrimvarsAPI mesh(preview.stage()->GetPrimAtPath(assetChild("Quad")));
    preview.setMaterial(SdfPath("/World/Mat"));
    CHECK(preview.filledUvSets() == Names({"uv1", "uv2", "uv3"}), "what it reads and the shape lacks: %s", joined(preview.filledUvSets()).c_str());
    CHECK(preview.uvSetSource() == "st", "from st: %s", preview.uvSetSource().c_str());
    for (const char* name : {"uv1", "uv2", "uv3"}) {
        const UsdGeomPrimvar primvar = mesh.GetPrimvar(TfToken(name));
        VtVec2fArray values, st;
        VtIntArray indices;
        CHECK(primvar && primvar.Get(&values) && mesh.GetPrimvar(TfToken("st")).Get(&st) && values == st, "%s has st's values", name);
        CHECK(primvar.GetInterpolation() == UsdGeomTokens->faceVarying, "its interpolation");
        CHECK(primvar.GetIndices(&indices) && indices == VtIntArray({0, 1, 1, 0}), "and its indices");
    }
    CHECK(!mesh.HasPrimvar(TfToken("dirt")), "a colour isn't made up");
    CHECK(preview.missingPrimvars() == Names({"dirt"}), "but said to be missing: %s", joined(preview.missingPrimvars()).c_str());

    // Renaming it on the material's interface follows.
    const SdfPath uvName("/World/Mat.inputs:uvName");
    source->GetAttributeAtPath(uvName).Set(std::string("other"));
    preview.sceneChanged({}, {uvName});
    CHECK(mesh.HasPrimvar(TfToken("other")) && !mesh.HasPrimvar(TfToken("uv1")), "the new name in, the old one out: %s",
          joined(preview.filledUvSets()).c_str());
    source->GetAttributeAtPath(uvName).Set(std::string("uv1"));
    preview.sceneChanged({}, {uvName});

    // A shape's own sets stay as they are.
    preview.setShape(shapeWith(folder, "two", quad("Quad", {"st", "uv1"})));
    VtVec2fArray own;
    UsdGeomPrimvarsAPI(preview.stage()->GetPrimAtPath(assetChild("Quad"))).GetPrimvar(TfToken("uv1")).Get(&own);
    CHECK(own == VtVec2fArray({GfVec2f(1, 1), GfVec2f(0, 0)}), "its own uv1 is kept");
    CHECK(preview.filledUvSets() == Names({"uv2", "uv3"}), "the others are filled in: %s", joined(preview.filledUvSets()).c_str());

    // One without st gives st what it has.
    preview.setShape(shapeWith(folder, "mapped", quad("Quad", {"UVMap"})));
    CHECK(preview.uvSetSource() == "UVMap" && preview.filledUvSets() == Names({"st", "uv1", "uv2", "uv3"}), "from UVMap: %s",
          joined(preview.filledUvSets()).c_str());

    // Only what shows the material, and a shape with no UVs says so.
    preview.setShape(shapeWith(folder, "listed", "    rel collection:preview:includes = </Shape/Quad>\n" + quad("Quad", {"st"}) + quad("Stand", {"st"}),
                               " (\n    prepend apiSchemas = [\"CollectionAPI:preview\"]\n)"));
    CHECK(UsdGeomPrimvarsAPI(preview.stage()->GetPrimAtPath(assetChild("Quad"))).HasPrimvar(TfToken("uv1")) &&
              !UsdGeomPrimvarsAPI(preview.stage()->GetPrimAtPath(assetChild("Stand"))).HasPrimvar(TfToken("uv1")),
          "the listed quad gets them, the stand doesn't");
    preview.setShape(shapeWith(folder, "bare", quad("Quad", {})));
    CHECK(preview.filledUvSets().empty() && preview.missingPrimvars().size() == 4, "no UVs to copy: %s", joined(preview.missingPrimvars()).c_str());

    preview.setMaterial(SdfPath("/World/Plain"));
    CHECK(preview.filledUvSets().empty() && preview.missingPrimvars().empty(), "a material that reads nothing needs nothing");
    CHECK(exported(source) == before, "the scene is untouched");
}

USDCC_TEST(shading_preview_shows_a_scene_prim) {
    QTemporaryDir temp;
    UsdStageRefPtr source = materials();
    const std::string before = exported(source);
    ShadingPreviewStage preview(source);
    std::string why;
    preview.setShape(shapeFile(temp.path().toStdString(), false));
    preview.setMaterial(kWood);
    const SdfPath ball("/World/WoodBall"), box("/World/SteelBox");

    CHECK(preview.setScenePrim(ball, &why), "a prim of the scene: %s", why.c_str());
    source->SetMetadata(TfToken("metersPerUnit"), 0.01);
    CHECK(preview.syncWithSource() == 1.0, "it isn't scaled when the units change, so nor is the view");
    source->SetMetadata(TfToken("metersPerUnit"), 1.0);
    CHECK(preview.syncWithSource() == 1.0, "and back");
    CHECK(preview.scenePrim() == ball && preview.stage()->GetPrimAtPath(ball), "is in the preview");
    CHECK(!preview.stage()->GetPrimAtPath(box), "without the rest of the scene");
    CHECK(!preview.stage()->GetPrimAtPath(assetChild("Ball")), "instead of the shape");
    CHECK(preview.scenePrimKeepsBindings() && boundMaterial(preview.stage(), ball) == kWood, "which has the material already");
    CHECK(!preview.shapeBounds().IsEmpty(), "and bounds to frame");

    // Another material: all of it shows that one.
    preview.setMaterial(kSteel);
    CHECK(!preview.scenePrimKeepsBindings() && boundMaterial(preview.stage(), ball) == kSteel, "another material, on all of it: %s",
          boundMaterial(preview.stage(), ball).GetText());
    for (const TfToken& purpose : {UsdShadeTokens->full, UsdShadeTokens->preview}) {
        CHECK(UsdShadeMaterialBindingAPI(preview.stage()->GetPrimAtPath(ball)).ComputeBoundMaterial(purpose).GetPath() == kSteel,
              "whatever the purpose: %s", purpose.GetText());
    }
    CHECK(preview.setScenePrim(box, &why) && preview.scenePrimKeepsBindings() && !preview.stage()->GetPrimAtPath(ball), "another prim: %s",
          why.c_str());

    // Not everything can be.
    CHECK(!preview.setScenePrim(SdfPath("/World/Looks"), &why) && !why.empty(), "a scope without geometry can't be: %s", why.c_str());
    CHECK(preview.scenePrim().IsEmpty() && preview.stage()->GetPrimAtPath(assetChild("Ball")), "and the shape is back");
    CHECK(!preview.setScenePrim(SdfPath("/World/Nothing"), &why), "nor what isn't there: %s", why.c_str());
    CHECK(preview.setScenePrim(ball, &why) && preview.setScenePrim(SdfPath(), &why), "none is the shape again");
    CHECK(preview.stage()->GetPrimAtPath(assetChild("Ball")) && !preview.stage()->GetPrimAtPath(ball), "and the prim is gone");
    CHECK(boundMaterial(preview.stage(), assetChild("Ball")) == kSteel, "the shape shows the material");
    CHECK(exported(source) == before, "the scene is untouched");

    // Gone from the scene, the shape shows.
    preview.setScenePrim(ball);
    source->RemovePrim(ball);
    preview.sceneChanged({ball}, {});
    CHECK(preview.scenePrim().IsEmpty() && preview.stage()->GetPrimAtPath(assetChild("Ball")), "a removed prim gives way to the shape");
}

USDCC_TEST(shading_preview_scene_prim_bindings) {
    // A car: paint on its body, glass on a subset of it, chrome by a collection on /World, and
    // a hood inside a scope that's bound stronger than what's in it.
    UsdStageRefPtr source = stageFrom(std::string(R"(#usda 1.0
def Xform "World" (
    prepend apiSchemas = ["CollectionAPI:shiny", "MaterialBindingAPI"]
)
{
    rel collection:shiny:includes = </World/Car/Bumper>
    rel material:binding:collection:shiny = [</World.collection:shiny>, </World/Looks/Chrome>]
    def Scope "Looks"
    {
        def Material "Paint" {}
        def Material "Glass" {}
        def Material "Chrome" {}
        def Material "Other" {}
    }
    def Mesh "Road" {}
    def Xform "Car"
    {
        double3 xformOp:translate = (100, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    )") + quad("Body", {"st"}, R"(
        rel material:binding = </World/Looks/Paint>
        def GeomSubset "Window" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [0]
            rel material:binding = </World/Looks/Glass>
        }
)") + quad("Bumper", {"st"}) +
                                      R"(
    }
    def Xform "Strong" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Looks/Paint> (
            bindMaterialAs = "strongerThanDescendants"
        )
)" + quad("Hood", {"st"}) + "    }\n}\n");
    const std::string before = exported(source);
    const SdfPath car("/World/Car"), body("/World/Car/Body"), window("/World/Car/Body/Window"), bumper("/World/Car/Bumper");
    const SdfPath paint("/World/Looks/Paint"), glass("/World/Looks/Glass"), chrome("/World/Looks/Chrome"), other("/World/Looks/Other");
    ShadingPreviewStage preview(source);
    std::string why;
    preview.setMaterial(paint);
    CHECK(preview.setScenePrim(car, &why) && preview.scenePrimKeepsBindings(), "the car has paint on it: %s", why.c_str());
    const UsdStageRefPtr& stage = preview.stage();
    CHECK(boundMaterial(stage, body) == paint && boundMaterial(stage, window) == glass, "its other materials stay: %s",
          boundMaterial(stage, window).GetText());
    CHECK(boundMaterial(stage, bumper) == chrome, "one by a collection of an ancestor too: %s", boundMaterial(stage, bumper).GetText());
    CHECK(!stage->GetPrimAtPath(SdfPath("/World/Road")), "which doesn't bring all of the scene");
    CHECK(GfIsClose(preview.shapeBounds().GetMidpoint(), GfVec3d(100.5, 0.5, 0), 1e-6), "it's where it is in the scene");

    preview.setMaterial(glass);
    CHECK(preview.scenePrimKeepsBindings() && boundMaterial(stage, body) == paint, "a subset's material counts");
    preview.setMaterial(other);
    CHECK(!preview.scenePrimKeepsBindings(), "nothing has this one");
    for (const SdfPath& path : {body, window, bumper}) CHECK(boundMaterial(stage, path) == other, "so all of it shows it: %s", path.GetText());

    // Bound in the scene while it's shown.
    UsdShadeMaterialBindingAPI(source->GetPrimAtPath(bumper)).Bind(UsdShadeMaterial(source->GetPrimAtPath(other)));
    preview.sceneChanged({}, {bumper.AppendProperty(UsdShadeTokens->materialBinding)});
    CHECK(preview.scenePrimKeepsBindings() && boundMaterial(stage, bumper) == other && boundMaterial(stage, body) == paint &&
              boundMaterial(stage, window) == glass,
          "bound to it in the scene, the rest has its own again: %s", boundMaterial(stage, body).GetText());
    source->GetPrimAtPath(bumper).RemoveProperty(UsdShadeTokens->materialBinding);
    preview.sceneChanged({bumper.AppendProperty(UsdShadeTokens->materialBinding)}, {});
    CHECK(!preview.scenePrimKeepsBindings() && boundMaterial(stage, body) == other, "and unbound again, all of it shows it");

    const SdfPath hood("/World/Strong/Hood");
    CHECK(preview.setScenePrim(hood, &why) && boundMaterial(stage, hood) == other, "a stronger binding above it gives way: %s",
          boundMaterial(stage, hood).GetText());
    // The bumper itself, whose own collection binding is above it.
    CHECK(preview.setScenePrim(bumper, &why) && boundMaterial(stage, bumper) == other, "and so does a collection's: %s",
          boundMaterial(stage, bumper).GetText());
    // A subset is shown on its mesh.
    CHECK(preview.setScenePrim(window, &why) && stage->GetPrimAtPath(body) && boundMaterial(stage, body) == other, "a subset shows its mesh: %s",
          why.c_str());
    CHECK(exported(source) == before, "the scene is untouched");
}

USDCC_TEST(shading_preview_scene_prim_hidden_instanced_and_lit) {
    UsdStageRefPtr source = stageFrom(std::string(R"(#usda 1.0
(
    upAxis = "Z"
)
def Xform "Asset"
{
)") + quad("Quad", {"st"}) + R"(
    def SphereLight "Lamp" {}
}
def Xform "World"
{
    def Material "Mat" {}
    def Xform "A" (
        instanceable = true
        references = </Asset>
    ) {}
    def Xform "Hidden"
    {
        token visibility = "invisible"
        def Xform "Plain" (
            references = </Asset>
        ) {}
    }
}
)");
    // Hidden by the session layer too, where usdcc edits.
    const SdfPath hidden("/World/Hidden/Plain");
    {
        const UsdEditContext session(source, source->GetSessionLayer());
        UsdGeomImageable(source->GetPrimAtPath(hidden)).MakeInvisible();
        UsdGeomImageable(source->GetPrimAtPath(SdfPath("/World/Hidden"))).GetVisibilityAttr().Set(UsdGeomTokens->invisible);
    }
    const std::string before = exported(source);
    ShadingPreviewStage preview(source);
    std::string why;
    const SdfPath mat("/World/Mat");
    preview.setMaterial(mat);
    CHECK(preview.setScenePrim(hidden, &why), "a hidden prim: %s", why.c_str());
    const auto quadPath = hidden.AppendChild(TfToken("Quad"));
    CHECK(UsdGeomImageable(preview.stage()->GetPrimAtPath(quadPath)).ComputeVisibility() == UsdGeomTokens->inherited, "shows in the preview");

    // Its light goes under an HDRI, and the sky stands up in a Z-up scene.
    const SdfPath lamp = hidden.AppendChild(TfToken("Lamp"));
    ShadingPreviewStage::Environment environment;
    environment.hdriFile = "studio.hdr";
    CHECK(preview.shapeHasLights() && preview.stage()->GetPrimAtPath(lamp).IsActive(), "its own light lights it");
    preview.setEnvironment(environment);
    CHECK(!preview.stage()->GetPrimAtPath(lamp).IsActive(), "but not under an HDRI");
    const UsdGeomXformable dome(preview.stage()->GetPrimAtPath(ShadingPreviewStage::root().AppendChild(TfToken("Env"))));
    GfMatrix4d matrix;
    bool resets = false;
    dome.GetLocalTransformation(&matrix, &resets);
    CHECK(GfIsClose(matrix.TransformDir(GfVec3d(0, 1, 0)), GfVec3d(0, 0, 1), 1e-6), "the HDRI's up is the scene's");
    preview.setScenePrim(SdfPath());
    dome.GetLocalTransformation(&matrix, &resets);
    CHECK(GfIsClose(matrix.TransformDir(GfVec3d(0, 1, 0)), GfVec3d(0, 1, 0), 1e-6), "and Y for a shape");
    CHECK(!preview.shapeHasLights(), "which has no light");

    // Inside an instance, the instance is what's shown, and takes the binding.
    const SdfPath proxy("/World/A/Quad");
    CHECK(source->GetPrimAtPath(proxy).IsInstanceProxy(), "an instance proxy");
    CHECK(preview.setScenePrim(proxy, &why) && preview.scenePrim() == proxy, "can be picked: %s", why.c_str());
    CHECK(preview.stage()->GetPrimAtPath(SdfPath("/World/A")).IsInstance() && boundMaterial(preview.stage(), proxy) == mat,
          "its instance shows the material: %s", boundMaterial(preview.stage(), proxy).GetText());

    // A material that reads what it doesn't have: said, not made up.
    CHECK(exported(source) == before, "the scene is untouched");
    UsdStageRefPtr readers = stageFrom(kReaders);
    readers->DefinePrim(SdfPath("/World/Geo"), TfToken("Xform")).GetReferences().AddReference(source->GetRootLayer()->GetIdentifier(),
                                                                                              SdfPath("/Asset"));
    ShadingPreviewStage other(readers);
    other.setMaterial(SdfPath("/World/Mat"));
    CHECK(other.setScenePrim(SdfPath("/World/Geo"), &why), "a prim with st only: %s", why.c_str());
    CHECK(other.filledUvSets().empty(), "gets no UV sets");
    Names missing = other.missingPrimvars();
    std::sort(missing.begin(), missing.end());
    CHECK(missing == Names({"dirt", "uv1", "uv2"}), "what it lacks is said: %s", joined(missing).c_str());
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
