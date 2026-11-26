// The UV View plugin's layout (docs/PRIMVAR_VIEWER.md §18): a UV set's faces from a mesh and its
// primvar, flipped and degenerate faces, seams, UDIM tiles, finding a face, what can't be laid
// out, and the textures a bound material reads with a set. Runs in usdcc_tests.
#include "test_harness.h"

#include "uv_layout.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::uv;

namespace {
UsdStageRefPtr stageOf(const std::string& text) {
    const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    return layer->ImportFromString("#usda 1.0\n" + text) ? UsdStage::Open(layer) : UsdStageRefPtr();
}

UvLayout layoutOf(const UsdStageRefPtr& stage, const std::string& path, const std::string& primvar = "st") {
    return computeUvLayout(readUvSource(stage->GetPrimAtPath(SdfPath(path)), primvar, UsdTimeCode::Default()));
}

// Two quads side by side, sharing the edge between points 1 and 4.
const char* const kTwoQuads = R"(
def Mesh "Grid"
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
    texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (
        interpolation = "faceVarying"
    )
    int[] primvars:st:indices = [0, 1, 4, 3, 1, 2, 5, 4]
    texCoord2f[] primvars:perPoint = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (
        interpolation = "vertex"
    )
    texCoord2f[] primvars:split = [(0, 0), (0.5, 0), (0.5, 1), (0, 1), (1.1, 0), (1.6, 0), (1.6, 1), (1.1, 1)] (
        interpolation = "faceVarying"
    )
    float2[] primvars:wide = [(0, 0), (1, 0), (1, 1), (0, 1), (1, 1), (2, 1), (2, 2), (1, 2)] (
        interpolation = "faceVarying"
    )
    texCoord2f[] primvars:short = [(0, 0), (1, 0)] (
        interpolation = "faceVarying"
    )
    texCoord2f[] primvars:badIndex = [(0, 0), (1, 0), (1, 1), (0, 1)] (
        interpolation = "faceVarying"
    )
    int[] primvars:badIndex:indices = [0, 1, 2, 3, 0, 1, 99, 3]
    float2 primvars:one = (0.5, 0.5) (
        interpolation = "constant"
    )
    float3[] primvars:three = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)] (
        interpolation = "faceVarying"
    )
}

def Xform "NotAMesh"
{
}

def Mesh "Problems"
{
    int[] faceVertexCounts = [4, 4, 4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0), (3, 0, 0), (3, 1, 0), (2, 1, 0), (4, 0, 0), (5, 0, 0), (5, 1, 0), (4, 1, 0), (6, 0, 0), (7, 0, 0), (7, 1, 0)]
    texCoord2f[] primvars:st = [(0, 0), (0.3, 0), (0.3, 0.3), (0, 0.3), (0.4, 0), (0.7, 0), (0.7, 0.3), (0.4, 0.3), (1, 0.5), (0.7, 0.5), (0.7, 0.8), (1, 0.8), (0, 0.9), (0.05, 0.95), (0.1, 1)] (
        interpolation = "faceVarying"
    )
}
)";
}  // namespace

USDCC_TEST(uv_layout_faces) {
    const UsdStageRefPtr stage = stageOf(kTwoQuads);
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const UvLayout layout = layoutOf(stage, "/Grid");
    CHECK(layout.error.empty(), "lays out: %s", layout.error.c_str());
    CHECK(layout.faces() == 2 && layout.corners.size() == 8 && (layout.faceCounts == std::vector<int>{4, 4}), "two quads, eight corners");
    CHECK(layout.corner(0, 2) == GfVec2f(0.5f, 1.0f) && layout.corner(1, 1) == GfVec2f(1.0f, 0.0f), "corners through the indices");
    CHECK(layout.flipped == 0 && layout.degenerate == 0 && layout.outside == 0 && layout.seams.empty(), "nothing wrong");
    CHECK((layout.udimTiles == std::vector<int>{1001}), "in 1001");
    CHECK(layout.bounds.GetMin() == GfVec2f(0) && layout.bounds.GetMax() == GfVec2f(1), "its bounds");
    CHECK(layout.faceAt(GfVec2f(0.25f, 0.5f)) == std::optional<std::size_t>(0) &&
              layout.faceAt(GfVec2f(0.75f, 0.5f)) == std::optional<std::size_t>(1) && !layout.faceAt(GfVec2f(2, 2)),
          "the face at a point");
    CHECK((layout.cornerPoints == std::vector<int>{0, 1, 4, 3, 1, 2, 5, 4}), "each corner's point");

    // The same, a UV a point.
    const UvLayout perPoint = layoutOf(stage, "/Grid", "perPoint");
    CHECK(perPoint.error.empty() && perPoint.interpolation == TfToken("vertex") && perPoint.corners == layout.corners && perPoint.seams.empty(),
          "vertex UVs lay out the same: %s", perPoint.error.c_str());
}

USDCC_TEST(uv_layout_seams_and_tiles) {
    const UsdStageRefPtr stage = stageOf(kTwoQuads);
    if (!stage) return;
    // The shared edge has other UVs on each side.
    const UvLayout split = layoutOf(stage, "/Grid", "split");
    CHECK(split.seams.size() == 2, "one seam edge, its two sides: %zu", split.seams.size());
    if (split.seams.size() == 2) {
        const auto points = [&](const std::pair<std::size_t, std::size_t>& edge) -> std::pair<int, int> {
            return std::minmax(split.cornerPoints[edge.first], split.cornerPoints[edge.second]);
        };
        CHECK(points(split.seams[0]) == std::make_pair(1, 4) && points(split.seams[1]) == std::make_pair(1, 4), "between points 1 and 4");
    }
    CHECK((split.udimTiles == std::vector<int>{1001, 1002}) && split.outside == 1, "the second face is in 1002 (%zu tiles)", split.udimTiles.size());

    const UvLayout wide = layoutOf(stage, "/Grid", "wide");
    CHECK((wide.udimTiles == std::vector<int>{1001, 1012}), "a face is in the tile its middle is in, whatever its corners touch (%zu)",
          wide.udimTiles.size());
    CHECK(udimTile(GfVec2f(0.5f, 0.5f)) == 1001 && udimTile(GfVec2f(1.5f, 0.5f)) == 1002 && udimTile(GfVec2f(0.5f, 1.5f)) == 1011 &&
              udimTile(GfVec2f(9.5f, 2.5f)) == 1030 && udimTile(GfVec2f(-0.5f, 0.5f)) == 0 && udimTile(GfVec2f(10.5f, 0)) == 0 &&
              udimTile(GfVec2f(0.5f, -0.5f)) == 0,
          "UDIM numbers");
}

USDCC_TEST(uv_layout_problems) {
    const UsdStageRefPtr stage = stageOf(kTwoQuads);
    if (!stage) return;
    const UvLayout layout = layoutOf(stage, "/Problems");
    CHECK(layout.error.empty() && layout.faces() == 4, "four faces: %s", layout.error.c_str());
    if (layout.faces() != 4) return;
    CHECK(layout.flipped == 1 && (layout.flags[2] & UvLayout::Flipped), "the third is wound the other way");
    CHECK(layout.degenerate == 1 && (layout.flags[3] & UvLayout::Degenerate), "the fourth has no area");
    CHECK(!(layout.flags[0] & (UvLayout::Flipped | UvLayout::Degenerate)) && !(layout.flags[1] & UvLayout::Flipped), "the rest are fine");
    CHECK(!layout.faceAt(GfVec2f(0.05f, 0.95f)), "a degenerate face isn't found");

    const UvLayout bad = layoutOf(stage, "/Grid", "badIndex");
    CHECK(bad.error.empty() && bad.badIndices == 1, "an index past the values (%zu)", bad.badIndices);
}

USDCC_TEST(uv_layout_cannot) {
    const UsdStageRefPtr stage = stageOf(kTwoQuads);
    if (!stage) return;
    const auto why = [&](const std::string& path, const std::string& primvar) { return layoutOf(stage, path, primvar).error; };
    CHECK(why("/NotAMesh", "st").find("isn't a mesh") != std::string::npos, "not a mesh");
    CHECK(why("/Grid", "nope").find("has no nope") != std::string::npos, "no such primvar");
    CHECK(why("/Grid", "one").find("constant") != std::string::npos, "one value for the mesh: %s", why("/Grid", "one").c_str());
    CHECK(why("/Grid", "three").find("isn't two numbers") != std::string::npos, "three numbers");
    CHECK(why("/Grid", "short").find("2 UVs for 8 face corners") != std::string::npos, "too few: %s", why("/Grid", "short").c_str());
    const UvLayout none = layoutOf(stage, "/Grid", "short");
    CHECK(none.faces() == 0 && !none.faceAt(GfVec2f(0.5f)), "and no faces");
}

USDCC_TEST(uv_layout_textures_reading) {
    const UsdStageRefPtr stage = stageOf(R"(
def Mesh "Ball" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Mat>
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}

def Material "Mat"
{
    token outputs:surface.connect = </Mat/Surface.outputs:surface>

    def Shader "Surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </Mat/Wood.outputs:rgb>
        float inputs:occlusion.connect = </Mat/Occlusion.outputs:r>
        float inputs:roughness.connect = </Mat/Rough.outputs:r>
        token outputs:surface
    }

    def Shader "Wood"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @textures/wood.<UDIM>.png@
        float2 inputs:st.connect = </Mat/Moved.outputs:result>
        float3 outputs:rgb
    }

    def Shader "Moved"
    {
        uniform token info:id = "UsdTransform2d"
        float2 inputs:in.connect = </Mat/ReadSt.outputs:result>
        float2 outputs:result
    }

    def Shader "ReadSt"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname = "st"
        float2 outputs:result
    }

    def Shader "Occlusion"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @ao.png@
        float2 inputs:st.connect = </Mat/ReadSecond.outputs:result>
        float outputs:r
    }

    def Shader "ReadSecond"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        token inputs:varname = "st1"
        float2 outputs:result
    }

    def Shader "Rough"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @rough.png@
        float outputs:r
    }
}
)");
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const UsdPrim ball = stage->GetPrimAtPath(SdfPath("/Ball"));
    const std::vector<UvTexture> st = texturesReading(ball, "st");
    std::vector<std::string> names;
    for (const UvTexture& texture : st) names.push_back(texture.shader.GetName());
    std::sort(names.begin(), names.end());
    CHECK((names == std::vector<std::string>{"Rough", "Wood"}), "st: through a transform, and the default (%zu)", names.size());
    const auto wood = std::find_if(st.begin(), st.end(), [](const UvTexture& t) { return t.shader.GetName() == "Wood"; });
    CHECK(wood != st.end() && wood->file.find("wood.<UDIM>.png") != std::string::npos, "the UDIM file as written: %s",
          wood != st.end() ? wood->file.c_str() : "");
    const std::vector<UvTexture> st1 = texturesReading(ball, "st1");
    CHECK(st1.size() == 1 && st1.front().shader.GetName() == "Occlusion", "st1: a token varname");
    CHECK(texturesReading(ball, "other").empty() && texturesReading(stage->GetPrimAtPath(SdfPath("/Mat")), "st").empty(),
          "nothing reads others, and nothing's bound to the material");
}
