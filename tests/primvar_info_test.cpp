// What a prim's primvars are, and what's wrong with them (docs/PRIMVAR_VIEWER.md §4): element
// counts, sources, layers, relations, readers, and the validation rules made of it. Runs in
// usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/primvar_info.h"
#include "usdcc/usd/validation_profile.h"
#include "usdcc/usd/validation_run.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/base/gf/matrix4d.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::usd;

namespace {
UsdStageRefPtr stageOf(const std::string& text) {
    const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    return layer->ImportFromString("#usda 1.0\n" + text) ? UsdStage::Open(layer) : UsdStageRefPtr();
}

UsdStageRefPtr brokenStage() { return UsdStage::Open(std::string(USDCC_TEST_DATA_DIR) + "/broken_primvars.usda"); }

PrimvarSet infoAt(const UsdStageRefPtr& stage, const std::string& path, const PrimvarInfoOptions& options = {}) {
    return primvarInfo(stage->GetPrimAtPath(SdfPath(path)), UsdTimeCode::EarliestTime(), options);
}

std::string codes(const std::vector<PrimvarProblem>& problems) {
    std::multiset<std::string> all;
    for (const PrimvarProblem& problem : problems) all.insert(problem.code);
    std::string result;
    for (const std::string& code : all) result += (result.empty() ? "" : ",") + code;
    return result;
}

std::string messages(const PrimvarSet& set) {
    std::string result;
    for (const PrimvarProblem& problem : set.allProblems()) result += problem.message + " | ";
    return result;
}

// Two faces, six points, eight corners.
const std::string kMesh = R"(
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
)";
}  // namespace

USDCC_TEST(primvar_info_counts) {
    const UsdStageRefPtr stage = stageOf("def Mesh \"Mesh\" {" + kMesh + R"(}
def Points "Points" {
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0)]
}
def BasisCurves "Bezier" {
    uniform token type = "cubic"
    uniform token basis = "bezier"
    int[] curveVertexCounts = [4, 7, 10, 4, 7]
    point3f[] points = [(0, 0, 0)]
}
def BasisCurves "Linear" {
    uniform token type = "linear"
    int[] curveVertexCounts = [2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0), (4, 0, 0)]
}
def NurbsCurves "Nurbs" {
    int[] curveVertexCounts = [4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0)]
}
def PointInstancer "Instancer" {
    int[] protoIndices = [0, 0, 0, 0]
}
def Cube "Cube" {}
def Mesh "Empty" {}
)");
    CHECK(stage, "the stage reads");
    if (!stage) return;
    const auto count = [&stage](const char* path, const TfToken& interpolation) -> long {
        const auto found = primvarElementCounts(stage->GetPrimAtPath(SdfPath(path)), UsdTimeCode::EarliestTime()).of(interpolation);
        return found.count ? long(*found.count) : -1;
    };
    const TfToken constant = UsdGeomTokens->constant, uniform = UsdGeomTokens->uniform, varying = UsdGeomTokens->varying,
                  vertex = UsdGeomTokens->vertex, faceVarying = UsdGeomTokens->faceVarying;
    CHECK(count("/Mesh", constant) == 1 && count("/Mesh", uniform) == 2 && count("/Mesh", varying) == 6 &&
              count("/Mesh", vertex) == 6 && count("/Mesh", faceVarying) == 8, "a mesh");
    CHECK(count("/Points", uniform) == 1 && count("/Points", varying) == 3 && count("/Points", vertex) == 3 &&
              count("/Points", faceVarying) == 3, "points");
    // basisCurves.h's own example: 2 3 4 2 3 ends of segments.
    CHECK(count("/Bezier", uniform) == 5 && count("/Bezier", varying) == 14 && count("/Bezier", vertex) == 1,
          "cubic curves: varying is %ld", count("/Bezier", varying));
    CHECK(count("/Linear", uniform) == 2 && count("/Linear", varying) == 5, "linear curves");
    CHECK(count("/Nurbs", uniform) == 1 && count("/Nurbs", vertex) == 4 && count("/Nurbs", varying) == -1, "NURBS curves");
    CHECK(count("/Instancer", vertex) == 4 && count("/Instancer", varying) == 4 && count("/Instancer", faceVarying) == 4 &&
              count("/Instancer", uniform) == -1, "a point instancer");
    CHECK(count("/Cube", vertex) == -1 && count("/Cube", constant) == 1, "a cube's vertices aren't specified");
    CHECK(count("/Empty", vertex) == -1 && count("/Empty", faceVarying) == -1, "a mesh without topology");
    CHECK(count("/Mesh", TfToken("perhaps")) == -1, "not an interpolation");
    const auto mesh = primvarElementCounts(stage->GetPrimAtPath(SdfPath("/Mesh")), UsdTimeCode::EarliestTime());
    CHECK(mesh.fitting(6) == std::vector<TfToken>{vertex}, "six is points once, not varying too");
    CHECK(mesh.fitting(8) == std::vector<TfToken>{faceVarying} && mesh.fitting(5).empty(), "what else fits");
}

USDCC_TEST(primvar_info_records) {
    const UsdStageRefPtr stage = brokenStage();
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const PrimvarSet set = infoAt(stage, "/World/Good");
    CHECK(set.allProblems().empty(), "nothing's wrong with it: %s", messages(set).c_str());
    CHECK(set.primvars.size() == 6 && set.primvars.front().name == "dirt", "its own five and an inherited one, by name (%zu)",
          set.primvars.size());
    const PrimvarInfo* st = set.find("st");
    CHECK(st && st->type == SdfValueTypeNames->TexCoord2fArray && st->interpolation == UsdGeomTokens->faceVarying &&
              st->interpolationAuthored && st->indexed && st->valueCount == 4 && st->elementCount == 8 &&
              st->expectedCount == 8u && st->expectedOf == "face corners" && !st->timeVarying, "an indexed one");
    CHECK(st && st->source == PrimvarSource::Local && st->sourcePrim == SdfPath("/World/Good") && !st->valuesLayer.empty() &&
              st->valuesLayer == st->indicesLayer && st->valuesLayer == st->interpolationLayer, "its layers");
    const PrimvarInfo* wetness = set.find("wetness");
    CHECK(wetness && wetness->source == PrimvarSource::Inherited && wetness->sourcePrim == SdfPath("/World") &&
              wetness->expectedCount == 1u, "an inherited one");
    CHECK(!set.find("st:indices"), "indices aren't a primvar");
    PrimvarInfoOptions own;
    own.inherited = false;
    CHECK(!infoAt(stage, "/World/Good", own).find("wetness"), "inherited ones left out");
    CHECK(primvarInfo(UsdPrim()).primvars.empty() && primvarInfo(stage->GetPseudoRoot()).primvars.empty(), "no prim");
}

USDCC_TEST(primvar_info_problems) {
    const UsdStageRefPtr stage = brokenStage();
    if (!stage) return;
    const struct {
        const char* prim;
        const char* codes;
        const char* says;
    } expected[] = {
        {"ElementCount", "ElementCount", "vertex needs 6 values (6 points), and it has 8, which fits faceVarying (face corners)"},
        {"ElementCountIndexed", "ElementCount", "faceVarying needs 8 indices (8 face corners), and it has 6, which fits vertex (points)"},
        {"IndexRange", "IndexRange", "2 of 8 indices are outside its 2 values"},
        {"IndicesType", "IndicesType", "Its indices are float[], not int[]"},
        {"IndicesOnly", "IndicesOnly", "st has indices but isn't there"},
        {"NoInterpolation", "NoInterpolation", "8 values: that fits faceVarying (face corners)"},
        {"ConstantArray", "ConstantArray", "It's constant, with 3 values: only the first is used"},
        {"ScalarPerPoint", "ElementCount", "vertex needs 6 values (6 points), and it has 1"},
        {"NonFinite", "NonFinite", "2 values are infinite or not a number"},
        {"Overrides", "Overrides", "The normals attribute isn't used"},
        {"ElementSize", "ElementCount", "uniform needs 6 values (2 faces, 3 each), and it has 5"},
        {"Incomplete", "Incomplete", "It has skel:jointIndices but not skel:jointWeights"},
        {"SkinningDiffers", "ElementSizeDiffers,InterpolationDiffers", "skel:jointIndices is constant and skel:jointWeights is vertex"},
        {"SkinningValues", "JointRange,WeightSum", "1 of 6 joint indices is outside the 2 joints"},
        {"NotGeometry", "NothingToInterpolate", "only constant primvars are inherited"},
    };
    for (const auto& each : expected) {
        const PrimvarSet set = infoAt(stage, std::string("/World/") + each.prim);
        const std::string found = codes(set.allProblems());
        CHECK(found == each.codes, "%s: %s", each.prim, found.c_str());
        CHECK(messages(set).find(each.says) != std::string::npos, "%s says: %s", each.prim, messages(set).c_str());
    }
    // Every prim of the file is in the table, or is fine.
    std::size_t prims = 0;
    for (const UsdPrim& prim : stage->GetPrimAtPath(SdfPath("/World")).GetChildren()) prims += prim.GetName() != TfToken("Good");
    CHECK(prims == std::size(expected), "the file's prims are all checked (%zu of %zu)", std::size(expected), prims);

    PrimvarInfoOptions quick;
    quick.scan = false;
    CHECK(infoAt(stage, "/World/IndexRange", quick).allProblems().empty() && infoAt(stage, "/World/NonFinite", quick).allProblems().empty(),
          "without the scan, values aren't looked at");
    CHECK(codes(infoAt(stage, "/World/ElementCount", quick).allProblems()) == "ElementCount", "but counts are");
}

USDCC_TEST(primvar_info_special_cases) {
    const UsdStageRefPtr stage = stageOf("def Mesh \"Mesh\" {" + kMesh + R"(
    texCoord2f[] primvars:st = [(0, 0)] (
        interpolation = "faceVarying"
        unauthoredValuesIndex = 9
    )
    int[] primvars:st:indices = [0, 0, 0, 0, 9, 0, 0, 0]
    float[] primvars:sh = [0, 0, 0, 0, 0, 0] (
        interpolation = "uniform"
        elementSize = 3
    )
    float[] primvars:shared = [0, 0, 1, 1] (
        interpolation = "uniform"
        elementSize = 2
    )
    int[] primvars:shared:indices = [1, 0]
    float[] primvars:declared ( interpolation = "vertex" )
    float[] primvars:blocked = None ( interpolation = "vertex" )
    string primvars:target ( interpolation = "constant" )
    rel primvars:target:idFrom = </Gone>
    float[] primvars:moving ( interpolation = "vertex" )
    float[] primvars:moving.timeSamples = {
        1: [0, 1, 2, 3, 4, 5],
        2: [5, 4, 3, 2, 1],
    }
}
def PointInstancer "Instancer" {
    int[] protoIndices = [0, 0]
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)] ( interpolation = "constant" )
    float[] primvars:age = [1, 2, 3] ( interpolation = "vertex" )
}
def Cube "Cube" {
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0)] ( interpolation = "vertex" )
}
)");
    CHECK(stage, "the stage reads");
    if (!stage) return;
    const PrimvarSet set = infoAt(stage, "/Mesh");
    const auto problemsOf = [&set](const char* name) { return set.find(name) ? codes(set.find(name)->problems) : std::string("none"); };
    CHECK(problemsOf("st").empty() && set.find("st")->unauthoredValuesIndex == 9, "the index for no value isn't out of range: %s",
          problemsOf("st").c_str());
    CHECK(problemsOf("sh").empty() && set.find("sh")->elementCount == 2, "three values an element");
    CHECK(problemsOf("shared").empty() && set.find("shared")->valueCount == 4, "indices with an element size");
    CHECK(set.find("declared") && !set.find("declared")->hasValue && problemsOf("declared").empty(), "declared only");
    CHECK(set.find("blocked") && !set.find("blocked")->hasValue && problemsOf("blocked").empty(), "blocked");
    CHECK(set.find("target") && set.find("target")->idTarget == SdfPath("/Gone") && problemsOf("target") == "IdTarget",
          "a path to nothing: %s", problemsOf("target").c_str());
    CHECK(set.find("moving") && set.find("moving")->timeVarying && problemsOf("moving").empty(), "time samples, at the earliest");
    const PrimvarSet later = primvarInfo(stage->GetPrimAtPath(SdfPath("/Mesh")), UsdTimeCode(2));
    CHECK(later.find("moving") && codes(later.find("moving")->problems) == "ElementCount", "and at a time of its own");
    CHECK(codes(infoAt(stage, "/Instancer").allProblems()) == "ElementCount", "an instancer's constant values apply whole: %s",
          messages(infoAt(stage, "/Instancer")).c_str());
    CHECK(infoAt(stage, "/Cube").allProblems().empty(), "nothing to say about a cube's vertices");

    const UsdAttribute sh = stage->GetAttributeAtPath(SdfPath("/Mesh.primvars:sh"));
    sh.SetMetadata(UsdGeomTokens->elementSize, 0);
    CHECK(codes(infoAt(stage, "/Mesh").find("sh")->problems) == "ElementSize", "an element size of 0");
    sh.SetMetadata(UsdGeomTokens->elementSize, 3);
    sh.SetMetadata(UsdGeomTokens->interpolation, TfToken("perhaps"));
    CHECK(codes(infoAt(stage, "/Mesh").find("sh")->problems) == "Interpolation", "not an interpolation: %s",
          codes(infoAt(stage, "/Mesh").find("sh")->problems).c_str());
}

USDCC_TEST(primvar_info_layers) {
    // Values authored over a weaker layer's indices.
    const SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    weak->ImportFromString("#usda 1.0\ndef Mesh \"Mesh\" {" + kMesh + R"(
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] ( interpolation = "faceVarying" )
    int[] primvars:st:indices = [0, 1, 2, 3, 0, 1, 2, 3]
})");
    const SdfLayerRefPtr strong = SdfLayer::CreateAnonymous("strong.usda");
    strong->InsertSubLayerPath(weak->GetIdentifier());
    const UsdStageRefPtr stage = UsdStage::Open(strong);
    const UsdGeomPrimvar st = UsdGeomPrimvarsAPI(stage->GetPrimAtPath(SdfPath("/Mesh"))).GetPrimvar(TfToken("st"));
    st.Set(VtVec2fArray(2));
    const PrimvarSet set = infoAt(stage, "/Mesh");
    const PrimvarInfo* info = set.find("st");
    CHECK(info && info->valuesLayer == strong->GetIdentifier() && info->indicesLayer == weak->GetIdentifier() &&
              info->interpolationLayer == weak->GetIdentifier(), "each part's layer");
    CHECK(codes(set.allProblems()) == "IndexRange" &&
              messages(set).find("values from strong.usda, indices from weak.usda") != std::string::npos, "and why: %s", messages(set).c_str());
    // Flat values with the weaker indices blocked are fine.
    st.Set(VtVec2fArray(8));
    st.BlockIndices();
    const PrimvarSet flat = infoAt(stage, "/Mesh");
    CHECK(flat.allProblems().empty() && flat.find("st") && !flat.find("st")->indexed && flat.find("st")->elementCount == 8,
          "blocked indices: %s", messages(flat).c_str());
}

USDCC_TEST(primvar_info_sources_and_readers) {
    const UsdStageRefPtr stage = stageOf(R"(
def Xform "World" {
    float primvars:wetness = 0.5 ( interpolation = "constant" )
    float primvars:tint = 0.5 ( interpolation = "constant" )
    def Mesh "Mesh" ( prepend apiSchemas = ["MaterialBindingAPI"] ) {)" + kMesh + R"(
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (2, 0), (0, 1), (1, 1), (2, 1)] ( interpolation = "vertex" )
        float primvars:wetness ( interpolation = "constant" )
        rel material:binding = </World/Material>
        def GeomSubset "Part" ( prepend apiSchemas = ["MaterialBindingAPI"] ) {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [1]
            rel material:binding = </World/Other>
        }
    }
    def Material "Material" {
        float primvars:tint = 1 ( interpolation = "constant" )
        float primvars:gloss = 1 ( interpolation = "constant" )
        token outputs:surface.connect = </World/Material/Surface.outputs:surface>
        def Shader "Surface" {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Material/Texture.outputs:rgb>
            float inputs:roughness.connect = </World/Material/Dirt.outputs:result>
            token outputs:surface
        }
        def Shader "Texture" {
            uniform token info:id = "UsdUVTexture"
            float2 inputs:st.connect = </World/Material/Reader.outputs:result>
            float3 outputs:rgb
        }
        def Shader "Reader" {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
        def Shader "Dirt" {
            uniform token info:id = "UsdPrimvarReader_float"
            string inputs:varname = "dirt"
            float outputs:result
        }
    }
    def Material "Other" {
        token outputs:surface.connect = </World/Other/Surface.outputs:surface>
        def Shader "Surface" {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Other/Color.outputs:result>
            token outputs:surface
        }
        def Shader "Color" {
            uniform token info:id = "UsdPrimvarReader_float3"
            string inputs:varname = "displayColor"
            float3 outputs:result
        }
        def Shader "Second" {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st1"
            float2 outputs:result
        }
    }
}
)");
    CHECK(stage, "the stage reads");
    if (!stage) return;
    const PrimvarSet set = infoAt(stage, "/World/Mesh");
    const auto source = [&set](const char* name) { return set.find(name) ? set.find(name)->source : PrimvarSource::Local; };
    CHECK(source("wetness") == PrimvarSource::Inherited && set.find("wetness")->hasValue,
          "one declared without a value doesn't hide the inherited one");
    CHECK(source("tint") == PrimvarSource::Inherited, "inherited wins over the material's");
    CHECK(source("gloss") == PrimvarSource::Material && set.find("gloss")->sourcePrim == SdfPath("/World/Material"), "the material's");
    CHECK(set.find("st") && set.find("st")->readers == SdfPathVector{SdfPath("/World/Material")}, "who reads it");
    CHECK(set.missing.size() == 1 && set.missing.front().name == "dirt" && !set.missing.front().uv &&
              set.missing.front().materials == SdfPathVector{SdfPath("/World/Material")},
          "what's read and missing: not displayColor, nor what isn't connected (%zu)", set.missing.size());
    PrimvarInfoOptions quick;
    quick.readers = false;
    CHECK(infoAt(stage, "/World/Mesh", quick).missing.empty(), "readers left out");
    CHECK(infoAt(stage, "/World").missing.empty(), "a group has no surface");
}

USDCC_TEST(primvar_info_relations) {
    PrimvarRelations& relations = PrimvarRelations::instance();
    const std::size_t builtIn = relations.relations().size();
    const UsdStageRefPtr stage = stageOf("def Mesh \"Mesh\" {" + kMesh + R"(
    float[] primvars:groom:length = [1, 2] ( interpolation = "uniform" )
    float[] primvars:groom:width = [1, 2, 3, 4, 5, 6] ( interpolation = "vertex" )
    color3f[] primvars:displayColor = [(1, 0, 0)] ( interpolation = "constant" )
    float[] primvars:displayOpacity = [1, 1] ( interpolation = "uniform" )
    normal3f[] primvars:normals = [(0, 0, 1), (0, 0, 1)] ( interpolation = "uniform" )
}
def Xform "Rig" {
    int[] primvars:skel:jointIndices = [0] ( interpolation = "constant" )
    def Mesh "Skinned" {)" + kMesh + R"( float[] primvars:skel:jointWeights = [1] ( interpolation = "constant" )
    }
    def Mesh "Inheriting" {)" + kMesh + R"( }
}
)");
    CHECK(stage, "the stage reads");
    if (!stage) return;
    PrimvarSet set = infoAt(stage, "/Mesh");
    CHECK(set.groups.size() == 1 && set.groups.front().id == "display" && set.groups.front().problems.empty(),
          "color and opacity go together, of different interpolations: %s", messages(set).c_str());
    CHECK(set.find("normals") && !set.find("normals")->derivedFrom.empty() && set.find("displayColor")->derivedFrom.empty(),
          "what normals are from");
    // One with its partner inherited is complete.
    CHECK(infoAt(stage, "/Rig/Skinned").allProblems().empty(), "an inherited partner counts: %s",
          messages(infoAt(stage, "/Rig/Skinned")).c_str());
    const PrimvarSet inheriting = infoAt(stage, "/Rig/Inheriting");
    CHECK(inheriting.allProblems().empty() && inheriting.groups.size() == 1 && inheriting.groups.front().missing.size() == 1,
          "nothing's wrong where nothing's authored");

    PrimvarRelation groom;
    groom.id = "studio.groom";
    groom.label = "Groom";
    groom.members = {"groom:*"};
    groom.sameInterpolation = true;
    groom.check = [](const UsdPrim&, UsdTimeCode, const PrimvarSet& primvars, std::vector<PrimvarProblem>& problems) {
        if (primvars.find("groom:length")) problems.push_back({PrimvarProblem::Severity::Info, "Studio", "seen", {"groom:length"}});
    };
    relations.set(groom);
    relations.set(groom);
    CHECK(relations.relations().size() == builtIn + 1, "the same id replaces");
    set = infoAt(stage, "/Mesh");
    const auto found = std::find_if(set.groups.begin(), set.groups.end(),
                                    [](const PrimvarGroup& group) { return group.id == "studio.groom"; });
    CHECK(found != set.groups.end() && found->members.size() == 2 && codes(found->problems) == "InterpolationDiffers,Studio",
          "a plugin's, by pattern");
    CHECK(relations.remove("studio.groom") && !relations.remove("studio.groom"), "removed");
    relations.remove("skel");
    CHECK(relations.relations().size() == builtIn - 1, "a built-in one too");
    relations.reset();
    CHECK(relations.relations().size() == builtIn, "and back");
}

USDCC_TEST(primvar_validation_rules) {
    using namespace usdcc::usd::validation;
    const UsdStageRefPtr stage = brokenStage();
    if (!stage) return;
    std::vector<std::string> names;
    for (const Rule& rule : rules()) {
        if (rule.group() == "usdccValidators") names.push_back(rule.name);
    }
    CHECK((names == std::vector<std::string>{"usdccValidators:PrimvarGroups", "usdccValidators:Primvars"}), "the rules are listed");
    if (names.size() != 2) return;
    Rule groups;
    groups.name = names.front();
    CHECK(ProfileLibrary::instance().find("General")->runs(groups), "and run by default");
    ValidationRun run(stage, names);
    run.runAll();
    std::set<std::string> prims;
    for (const Finding& finding : run.findings()) {
        prims.insert(finding.prim().GetName());
        CHECK(finding.rule.rfind("usdccValidators:", 0) == 0, "%s: %s", finding.rule.c_str(), finding.message.c_str());
        if (finding.name == "ElementCount") {
            CHECK(finding.severity == Severity::Error && finding.sites.size() == 1 && finding.sites.front().path.IsPropertyPath(),
                  "at the primvar: %s", finding.message.c_str());
        }
    }
    CHECK(prims.size() == 15 && !prims.count("Good"), "every broken prim and no other (%zu)", prims.size());
    const auto incomplete = std::find_if(run.findings().begin(), run.findings().end(),
                                         [](const Finding& finding) { return finding.name == "Incomplete"; });
    CHECK(incomplete != run.findings().end() && incomplete->rule == "usdccValidators:PrimvarGroups", "the groups' rule");
}

USDCC_TEST(primvar_statistics) {
    const PrimvarStatistics floats = primvarStatistics(VtValue(VtFloatArray{1, 2, 6, std::numeric_limits<float>::quiet_NaN()}));
    CHECK(floats.count == 4 && floats.components == 1 && floats.nonFinite == 1 && floats.min == std::vector<double>{1} &&
              floats.max == std::vector<double>{6} && floats.mean == std::vector<double>{3} && !floats.distinct,
          "floats: min, max and mean of the finite ones");
    const PrimvarStatistics uvs = primvarStatistics(VtValue(VtVec2fArray{GfVec2f(0, 1), GfVec2f(2, -1)}));
    CHECK(uvs.components == 2 && uvs.min == (std::vector<double>{0, -1}) && uvs.max == (std::vector<double>{2, 1}), "per component");
    const PrimvarStatistics ids = primvarStatistics(VtValue(VtIntArray{3, 1, 3, 3, 2, 1}));
    CHECK(ids.distinct == 3u && ids.commonest.size() == 3 && ids.commonest.front() == std::make_pair(std::string("3"), std::size_t(3)) &&
              ids.commonest[1].first == "1", "ints: distinct and the commonest first");
    const PrimvarStatistics names = primvarStatistics(VtValue(VtStringArray{"a", "b", "a"}));
    CHECK(names.components == 0 && names.distinct == 2u && names.min.empty() && names.count == 3, "strings");
    const PrimvarStatistics scalar = primvarStatistics(VtValue(GfVec3f(1, 2, 3)));
    CHECK(scalar.count == 1 && scalar.components == 3 && scalar.mean == (std::vector<double>{1, 2, 3}), "a scalar");
    const PrimvarStatistics nothing = primvarStatistics(VtValue(VtFloatArray{std::numeric_limits<float>::infinity()}));
    CHECK(nothing.nonFinite == 1 && nothing.min.empty() && nothing.mean.empty(), "no finite value");
    CHECK(primvarStatistics(VtValue()).count == 0 && primvarStatistics(VtValue(GfMatrix4d(1))).components == 0, "nothing to say");

    VtValue values(VtFloatArray{10, 20});
    CHECK(flattenPrimvar(values, VtIntArray{1, 1, 0}, 1) && values.Get<VtFloatArray>() == (VtFloatArray{20, 20, 10}), "flattened");
    VtValue pairs(VtFloatArray{1, 2, 3, 4});
    CHECK(flattenPrimvar(pairs, VtIntArray{1, 0}, 2) && pairs.Get<VtFloatArray>() == (VtFloatArray{3, 4, 1, 2}), "with an element size");
    VtValue bad(VtFloatArray{1});
    CHECK(!flattenPrimvar(bad, VtIntArray{0, 4}, 1) && bad.Get<VtFloatArray>().size() == 1, "indices outside: left as it was");
}
