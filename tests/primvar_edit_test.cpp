// Editing a primvar as one thing (docs/PRIMVAR_VIEWER.md §7 level 1, §19): each command on a stage
// whose primvars come from a weaker sublayer, written to the root layer; refusals for a stronger
// layer, time samples, instance proxies, a locked layer and broken results. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/primvar_edit.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <algorithm>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::usd;

namespace {
// Two quads sharing the edge between points 1 and 4; the primvars are in the weaker layer.
const char* const kBase = R"(#usda 1.0
def Mesh "Grid"
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
    texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (
        interpolation = "faceVarying"
    )
    int[] primvars:st:indices = [0, 1, 4, 3, 1, 2, 5, 4]
    texCoord2f[] primvars:seam = [(0, 0), (0.5, 0), (0.5, 1), (0, 1), (0.6, 0), (1, 0), (1, 1), (0.6, 1)] (
        interpolation = "faceVarying"
    )
    float[] primvars:dirt = [0, 1, 2, 3, 4, 5] (
        interpolation = "vertex"
    )
    float[] primvars:same = [2, 2, 2, 2, 2, 2] (
        interpolation = "vertex"
    )
    int[] primvars:faceId = [7, 9] (
        interpolation = "uniform"
    )
    color3f[] primvars:tint = [(1, 0, 0)] (
        interpolation = "constant"
    )
    float[] primvars:timed (
        interpolation = "vertex"
    )
    float[] primvars:timed.timeSamples = {
        1: [0, 0, 0, 0, 0, 0],
    }
}

def Mesh "Other"
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 1), (1, 0, 1), (2, 0, 1), (0, 1, 1), (1, 1, 1), (2, 1, 1)]
}

def Mesh "Tri"
{
    int[] faceVertexCounts = [3, 3, 2]
    int[] faceVertexIndices = [0, 1, 2, 0, 2, 3, 0, 1]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
}

def Xform "Group"
{
}

def Xform "Proto"
{
    def Mesh "Shape"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        float[] primvars:w = [1, 2, 3] (
            interpolation = "vertex"
        )
    }
}

def Xform "Instance" (
    instanceable = true
    references = </Proto>
)
{
}
)";

struct Fixture {
    SdfLayerRefPtr base = SdfLayer::CreateAnonymous("base.usda");
    SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    UsdStageRefPtr stage;
    UsdPrim grid;
    Fixture() {
        if (!base->ImportFromString(kBase)) return;
        root->SetSubLayerPaths({base->GetIdentifier()});
        stage = UsdStage::Open(root);
        stage->SetEditTarget(UsdEditTarget(root));
        grid = stage->GetPrimAtPath(SdfPath("/Grid"));
    }
    UsdGeomPrimvar primvar(const std::string& name, const char* prim = "/Grid") const {
        return UsdGeomPrimvarsAPI(stage->GetPrimAtPath(SdfPath(prim))).GetPrimvar(TfToken(name));
    }
    template <typename T>
    VtArray<T> flat(const std::string& name, const char* prim = "/Grid") const {
        VtArray<T> values;
        primvar(name, prim).ComputeFlattened(&values, UsdTimeCode::Default());
        return values;
    }
    bool rootHas(const std::string& property, const char* prim = "/Grid") const {
        return bool(root->GetPropertyAtPath(SdfPath(prim).AppendProperty(TfToken(property))));
    }
};

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool anyNote(const PrimvarEdit& edit, const std::string& part) {
    return std::any_of(edit.notes.begin(), edit.notes.end(), [&](const std::string& note) { return contains(note, part); });
}
}  // namespace

USDCC_TEST(primvar_edit_create_remove_block) {
    Fixture f;
    CHECK(f.grid, "the stage opens");
    if (!f.grid) return;
    PrimvarEdit edit = createPrimvar(f.grid, "weight", SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex);
    CHECK(edit && (f.flat<float>("weight") == VtFloatArray(6, 0.0f)) && f.rootHas("primvars:weight"), "a vertex float, a zero a point: %s",
          edit.error.c_str());
    CHECK(contains(createPrimvar(f.grid, "weight", SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).error, "has weight already"), "twice");
    CHECK(contains(createPrimvar(f.grid, "a b", SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).error, "can't be a primvar's name"), "a bad name");
    CHECK(createPrimvar(f.grid, "perFace", SdfValueTypeNames->Color3fArray, UsdGeomTokens->uniform) && f.flat<GfVec3f>("perFace").size() == 2,
          "a colour a face");
    CHECK(createPrimvar(f.grid, "one", SdfValueTypeNames->Float, UsdGeomTokens->constant) && f.primvar("one").GetTypeName() == SdfValueTypeNames->Float,
          "a single constant value");
    CHECK(contains(createPrimvar(f.stage->GetPrimAtPath(SdfPath("/Group")), "w", SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).error,
                   "USD doesn't say"),
          "an Xform has no points");

    edit = removePrimvar(f.grid, "weight");
    CHECK(edit && !f.primvar("weight") && !f.rootHas("primvars:weight"), "removed: %s", edit.error.c_str());
    edit = removePrimvar(f.grid, "dirt");
    CHECK(!edit && contains(edit.error, "has nothing of dirt to remove") && contains(edit.error, "Block hides it"), "only the root's: %s",
          edit.error.c_str());

    edit = blockPrimvar(f.grid, "dirt");
    VtValue value;
    CHECK(edit && !f.primvar("dirt").Get(&value) && f.rootHas("primvars:dirt") && f.base->GetPropertyAtPath(SdfPath("/Grid.primvars:dirt")),
          "blocked here, the base untouched: %s", edit.error.c_str());
    CHECK(anyNote(edit, "Overrides dirt from"), "and it says what it overrides");
    edit = removePrimvar(f.grid, "dirt");
    CHECK(edit && f.flat<float>("dirt").size() == 6 && anyNote(edit, "still has dirt"), "removing the block shows the base's again");
}

USDCC_TEST(primvar_edit_duplicate_rename_type) {
    Fixture f;
    if (!f.grid) return;
    PrimvarEdit edit = duplicatePrimvar(f.grid, "st", "st1");
    CHECK(edit && f.primvar("st1").IsIndexed() && f.flat<GfVec2f>("st1") == f.flat<GfVec2f>("st") &&
              f.primvar("st1").GetInterpolation() == UsdGeomTokens->faceVarying && f.primvar("st1").GetTypeName() == SdfValueTypeNames->TexCoord2fArray,
          "a copy, indexed, faceVarying, texCoord2f: %s", edit.error.c_str());
    CHECK(contains(duplicatePrimvar(f.grid, "st", "st1").error, "has st1 already"), "not over one that's there");

    const VtFloatArray dirt = f.flat<float>("dirt");
    edit = renamePrimvar(f.grid, "dirt", "grime");
    VtValue value;
    CHECK(edit && f.flat<float>("grime") == dirt && !f.primvar("dirt").Get(&value), "renamed, the old one blocked: %s", edit.error.c_str());
    CHECK(anyNote(edit, "dirt is blocked here"), "which it says");
    edit = renamePrimvar(f.grid, "st1", "uv");
    CHECK(edit && f.primvar("uv") && !f.primvar("st1") && !f.rootHas("primvars:st1") && !f.rootHas("primvars:st1:indices"),
          "one only here goes altogether: %s", edit.error.c_str());
    CHECK(contains(renamePrimvar(f.grid, "uv", "st").error, "has st already"), "not onto another");

    const std::vector<SdfValueTypeName> like = primvarTypesLike(SdfValueTypeNames->TexCoord2fArray);
    CHECK(std::find(like.begin(), like.end(), SdfValueTypeNames->Float2Array) != like.end() &&
              std::find(like.begin(), like.end(), SdfValueTypeNames->Float3Array) == like.end(),
          "float2[] has texCoord2f[]'s data, float3[] hasn't");
    edit = setPrimvarType(f.grid, "st", SdfValueTypeNames->Float2Array);
    CHECK(edit && f.primvar("st").GetTypeName() == SdfValueTypeNames->Float2Array && f.primvar("st").IsIndexed(), "another role: %s",
          edit.error.c_str());
    CHECK(anyNote(edit, "Overrides st from"), "over the base's");
    CHECK(contains(setPrimvarType(f.grid, "st", SdfValueTypeNames->Float3Array).error, "holds other data"), "not other data");
}

USDCC_TEST(primvar_edit_interpolation) {
    Fixture f;
    if (!f.grid) return;
    // More elements: the same values, indexed.
    PrimvarEdit edit = setPrimvarInterpolation(f.grid, "dirt", UsdGeomTokens->faceVarying);
    CHECK(edit && f.primvar("dirt").IsIndexed() && (f.flat<float>("dirt") == VtFloatArray{0, 1, 4, 3, 1, 2, 5, 4}), "vertex to faceVarying: %s",
          edit.error.c_str());
    edit = setPrimvarInterpolation(f.grid, "faceId", UsdGeomTokens->faceVarying);
    CHECK(edit && (f.flat<int>("faceId") == VtIntArray{7, 7, 7, 7, 9, 9, 9, 9}), "uniform to faceVarying: %s", edit.error.c_str());
    edit = setPrimvarInterpolation(f.grid, "tint", UsdGeomTokens->vertex);
    CHECK(edit && f.flat<GfVec3f>("tint") == VtVec3fArray(6, GfVec3f(1, 0, 0)), "constant to vertex: %s", edit.error.c_str());

    // Fewer: only where they agree.
    edit = setPrimvarInterpolation(f.grid, "st", UsdGeomTokens->vertex);
    CHECK(edit && f.primvar("st").GetInterpolation() == UsdGeomTokens->vertex && f.flat<GfVec2f>("st").size() == 6 &&
              f.flat<GfVec2f>("st")[4] == GfVec2f(0.5f, 1.0f),
          "faceVarying to vertex where each point's corners agree: %s", edit.error.c_str());
    edit = setPrimvarInterpolation(f.grid, "seam", UsdGeomTokens->vertex);
    CHECK(!edit && contains(edit.error, "2 of the 6 points") && f.primvar("seam").GetInterpolation() == UsdGeomTokens->faceVarying,
          "refused across a seam, nothing written: %s", edit.error.c_str());
    edit = setPrimvarInterpolation(f.grid, "same", UsdGeomTokens->constant);
    CHECK(edit && !f.primvar("same").IsIndexed() && (f.flat<float>("same") == VtFloatArray{2}), "all equal to constant: %s", edit.error.c_str());
    CHECK(contains(setPrimvarInterpolation(f.grid, "same", UsdGeomTokens->constant).error, "constant already"), "the same again");
    CHECK(contains(setPrimvarInterpolation(f.grid, "timed", UsdGeomTokens->faceVarying).error, "time samples"), "not with time samples");
}

USDCC_TEST(primvar_edit_indices_and_element_size) {
    Fixture f;
    if (!f.grid) return;
    PrimvarEdit edit = flattenPrimvarIndices(f.grid, "st");
    const VtVec2fArray st = f.flat<GfVec2f>("st");
    CHECK(edit && !f.primvar("st").IsIndexed() && st.size() == 8 && st[2] == GfVec2f(0.5f, 1.0f), "flattened, the base's indices blocked: %s",
          edit.error.c_str());
    CHECK(f.base->GetPropertyAtPath(SdfPath("/Grid.primvars:st:indices")), "the base keeps them");
    edit = indexPrimvar(f.grid, "st");
    CHECK(edit && f.primvar("st").IsIndexed() && f.flat<GfVec2f>("st") == st && anyNote(edit, "8 values merged into 6"), "and indexed again: %s",
          edit.error.c_str());
    CHECK(contains(indexPrimvar(f.grid, "dirt").error, "indices would save nothing"), "nothing to merge");
    CHECK(contains(flattenPrimvarIndices(f.grid, "dirt").error, "isn't indexed"), "nothing to flatten");

    PrimvarData pair;
    pair.type = SdfValueTypeNames->FloatArray;
    pair.interpolation = UsdGeomTokens->constant;
    pair.elementSize = 2;
    pair.values = VtValue(VtFloatArray{1, 2});
    edit = authorPrimvar(f.grid, "pair", pair);
    CHECK(edit && f.primvar("pair").GetElementSize() == 2, "a constant pair: %s", edit.error.c_str());
    edit = setPrimvarElementSize(f.grid, "pair", 1);
    CHECK(!edit && contains(edit.error, "would have 2 elements") && contains(edit.error, "needs 1"), "two values aren't one element: %s",
          edit.error.c_str());
    CHECK(contains(setPrimvarElementSize(f.grid, "st", 2).error, "Flatten it first"), "not indexed ones");
}

USDCC_TEST(primvar_edit_refusals) {
    Fixture f;
    if (!f.grid) return;
    PrimvarData data;
    data.type = SdfValueTypeNames->FloatArray;
    data.interpolation = UsdGeomTokens->vertex;
    data.values = VtValue(VtFloatArray{1, 2, 3});
    PrimvarEdit edit = authorPrimvar(f.grid, "short", data);
    CHECK(!edit && contains(edit.error, "would have 3 elements") && contains(edit.error, "needs 6") && !f.rootHas("primvars:short"),
          "too few for vertex, nothing written: %s", edit.error.c_str());
    data.values = VtValue(VtFloatArray{1, 2});
    data.indices = VtIntArray{0, 1, 0, 1, 0, 2};
    CHECK(contains(authorPrimvar(f.grid, "short", data).error, "Index 2 (at 5) is outside the 2 values"), "an index past the values");
    data.indices.reset();
    data.values = VtValue(VtIntArray{1, 2, 3, 4, 5, 6});
    CHECK(contains(authorPrimvar(f.grid, "short", data).error, "not float[]"), "values of another type");

    // A stronger layer's opinion would hide the edit.
    f.stage->GetSessionLayer()->ImportFromString(R"(#usda 1.0
over "Grid"
{
    float[] primvars:dirt = [9, 9, 9, 9, 9, 9]
}
)");
    edit = setPrimvarInterpolation(f.grid, "dirt", UsdGeomTokens->faceVarying);
    CHECK(!edit && contains(edit.error, "A stronger layer") && contains(edit.error, "wouldn't show"), "a stronger layer: %s", edit.error.c_str());
    f.stage->GetSessionLayer()->Clear();

    const UsdPrim proxy = f.stage->GetPrimAtPath(SdfPath("/Instance/Shape"));
    CHECK(proxy && proxy.IsInstanceProxy() && contains(blockPrimvar(proxy, "w").error, "is in an instance"), "an instance proxy");

    f.root->SetPermissionToEdit(false);
    CHECK(contains(createPrimvar(f.grid, "x", SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).error, "is locked"), "a locked layer");
    f.root->SetPermissionToEdit(true);
}

USDCC_TEST(primvar_edit_copy) {
    Fixture f;
    if (!f.grid) return;
    const UsdPrim other = f.stage->GetPrimAtPath(SdfPath("/Other"));
    PrimvarEdit edit = copyPrimvar(f.grid, other, "st");
    CHECK(edit && f.flat<GfVec2f>("st", "/Other") == f.flat<GfVec2f>("st") && f.primvar("st", "/Other").IsIndexed(), "the same faces: %s",
          edit.error.c_str());
    edit = copyPrimvar(f.grid, f.stage->GetPrimAtPath(SdfPath("/Tri")), "st");
    CHECK(!edit && contains(edit.error, "faces aren't"), "other faces: %s", edit.error.c_str());
    edit = copyPrimvar(f.grid, f.stage->GetPrimAtPath(SdfPath("/Tri")), "dirt");
    CHECK(!edit && contains(edit.error, "needs 4"), "other points: %s", edit.error.c_str());
}
