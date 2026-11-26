// Color By (docs/PRIMVAR_VIEWER.md §17): the colorizers, the stage's range and categories, and
// the scene index filter that hands the colours to Hydra. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/primvar_colors.h"

#include "../src/cpp/usd/src/primvar_color_filter.h"

#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>

#include <cmath>
#include <limits>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::usd;

namespace {
bool close(const GfVec3f& a, const GfVec3f& b) { return (a - b).GetLength() < 1e-4f; }

VtVec3fArray colorsOf(const char* id, const VtValue& values, PrimvarColorRange range = {}, int component = -1) {
    const PrimvarColorizer* colorizer = PrimvarColorizers::instance().find(id);
    return colorizer ? colorizer->colors(values, {range, component}) : VtVec3fArray();
}

std::string chosen(const SdfValueTypeName& type) {
    const PrimvarColorizer* colorizer = PrimvarColorizers::instance().choose({}, type);
    return colorizer ? colorizer->id : "none";
}

// Counts the prims an observer is told are dirty.
struct Counter : HdSceneIndexObserver {
    std::size_t dirtied = 0;
    void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries&) override {}
    void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries&) override {}
    void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries& entries) override { dirtied += entries.size(); }
    void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries&) override {}
};
}  // namespace

USDCC_TEST(primvar_colorizers) {
    const PrimvarColorRange range{0, 10};
    const VtVec3fArray ramp = colorsOf("ramp", VtValue(VtFloatArray{0, 5, 10, 20, std::numeric_limits<float>::quiet_NaN()}), range);
    CHECK(ramp.size() == 5 && close(ramp[0], primvarRampStops().front()) && close(ramp[2], primvarRampStops().back()) &&
              close(ramp[3], ramp[2]) && close(ramp[1], primvarRampColor(0.5)),
          "the ramp: its ends, clamped beyond them");
    CHECK(close(ramp[4], GfVec3f(1, 0, 1)), "not a number: magenta");
    const VtVec3fArray lengths = colorsOf("ramp", VtValue(VtVec3fArray{GfVec3f(3, 4, 0)}), range);
    const VtVec3fArray ys = colorsOf("ramp", VtValue(VtVec3fArray{GfVec3f(3, 4, 0)}), range, 1);
    CHECK(close(lengths[0], primvarRampColor(0.5)) && close(ys[0], primvarRampColor(0.4)), "a vector's length, or a component");
    CHECK(colorsOf("ramp", VtValue(2.5f), range).size() == 1, "a scalar: one colour");

    const VtVec3fArray categories = colorsOf("categories", VtValue(VtIntArray{7, 3, 7}));
    CHECK(categories.size() == 3 && categories[0] == categories[2] && categories[0] != categories[1] &&
              categories[0] == primvarCategoryColor("7"),
          "categories: the same value the same colour, as its text's");
    CHECK(colorsOf("categories", VtValue(VtStringArray{"a"}))[0] == colorsOf("categories", VtValue(VtTokenArray{TfToken("a")}))[0],
          "strings and tokens alike");
    CHECK(close(colorsOf("color", VtValue(VtVec3fArray{GfVec3f(0.2f, 2, -1)}))[0], GfVec3f(0.2f, 1, 0)), "colours, clamped");
    CHECK(close(colorsOf("uv", VtValue(VtVec2fArray{GfVec2f(0.25f, 0.75f)}))[0], GfVec3f(0.25f, 0.75f, 0)), "U red, V green");
    CHECK(close(colorsOf("direction", VtValue(VtVec3fArray{GfVec3f(0, 0, 4)}))[0], GfVec3f(0.5f, 0.5f, 1)), "a direction");
    CHECK(colorsOf("ramp", VtValue(VtStringArray{"a"})).empty(), "a colorizer that can't read them gives nothing");

    const auto& t = *SdfValueTypeNames;
    CHECK(chosen(t.Color3fArray) == "color" && chosen(t.TexCoord2fArray) == "uv" && chosen(t.Float2Array) == "uv" &&
              chosen(t.Normal3fArray) == "direction" && chosen(t.IntArray) == "categories" && chosen(t.StringArray) == "categories" &&
              chosen(t.FloatArray) == "ramp" && chosen(t.Point3fArray) == "ramp" && chosen(t.Float) == "ramp",
          "the one for each type");
    CHECK(chosen(t.Matrix4dArray) == "none" && chosen(t.Asset) == "none", "none for what has no numbers or words");
    CHECK(PrimvarColorizers::instance().choose("categories", t.FloatArray)->id == "ramp", "one asked for that doesn't apply: the type's");
    CHECK(PrimvarColorizers::instance().choose("ramp", t.Color3fArray)->id == "ramp", "one asked for that does");

    PrimvarColorizers::instance().add({"test.grey", "Grey", [](const SdfValueTypeName&) { return true; },
                                       [](const VtValue& values, const PrimvarColorizerInput&) {
                                           return VtVec3fArray(values.IsArrayValued() ? values.GetArraySize() : 1, GfVec3f(0.5f));
                                       }});
    CHECK(PrimvarColorizers::instance().choose("test.grey", t.FloatArray)->id == "test.grey", "a plugin's");
    CHECK(PrimvarColorizers::instance().remove("test.grey") && !PrimvarColorizers::instance().find("test.grey"), "removed");
}

USDCC_TEST(primvar_color_stats) {
    const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    layer->ImportFromString(R"(#usda 1.0
def Mesh "A" {
    float[] primvars:heat = [2, 7, -1] ( interpolation = "vertex" )
    int[] primvars:id = [3, 1, 3] ( interpolation = "uniform" )
    vector3f[] primvars:wind = [(3, 4, 0)] ( interpolation = "constant" )
}
def Mesh "B" {
    float[] primvars:heat.timeSamples = { 1: [10], 2: [20] }
    string[] primvars:id = ["x"]
}
def Mesh "C" {}
)");
    const UsdStageRefPtr stage = UsdStage::Open(layer);
    const PrimvarColorStats heat = primvarColorStats(stage, "heat", UsdTimeCode(1));
    CHECK(heat.prims == 2 && heat.type == SdfValueTypeNames->FloatArray && heat.range == (PrimvarColorRange{-1, 10}) && heat.timeVarying,
          "the range over every prim, at the time");
    CHECK(primvarColorStats(stage, "heat", UsdTimeCode(2)).range == (PrimvarColorRange{-1, 20}), "another time");
    const PrimvarColorStats ids = primvarColorStats(stage, "id", UsdTimeCode::Default());
    CHECK((ids.categories == std::vector<std::string>{"1", "3", "x"}) && !ids.moreCategories, "distinct values, sorted, of any kind");
    CHECK(primvarColorStats(stage, "wind", UsdTimeCode::Default()).range == (PrimvarColorRange{5, 5}), "a vector's length");
    CHECK(primvarColorStats(stage, "wind", UsdTimeCode::Default(), 1).range == (PrimvarColorRange{4, 4}), "or a component");
    const PrimvarColorStats none = primvarColorStats(stage, "nothing", UsdTimeCode::Default());
    CHECK(none.prims == 0 && !none.type && !none.range, "nothing has it");
}

USDCC_TEST(primvar_color_filter) {
    // A mesh with a primvar and a material, one without the primvar, a cube, and a material.
    const auto primvar = [](VtValue values, const TfToken& interpolation) {
        return HdPrimvarSchema::Builder()
            .SetPrimvarValue(HdRetainedSampledDataSource::New(values))
            .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
            .Build();
    };
    const auto mesh = [&](bool withHeat) {
        std::vector<TfToken> names{HdMaterialBindingsSchema::GetSchemaToken()};
        std::vector<HdDataSourceBaseHandle> sources{HdRetainedTypedSampledDataSource<SdfPath>::New(SdfPath("/Look"))};
        if (withHeat) {
            names.push_back(HdPrimvarsSchema::GetSchemaToken());
            sources.push_back(HdRetainedContainerDataSource::New(TfToken("heat"), primvar(VtValue(VtFloatArray{0, 10}), HdPrimvarSchemaTokens->vertex)));
        }
        return HdRetainedContainerDataSource::New(names.size(), names.data(), sources.data());
    };
    const HdRetainedSceneIndexRefPtr input = HdRetainedSceneIndex::New();
    input->AddPrims({{SdfPath("/Hot"), HdPrimTypeTokens->mesh, mesh(true)},
                     {SdfPath("/Plain"), HdPrimTypeTokens->mesh, mesh(false)},
                     {SdfPath("/Box"), HdPrimTypeTokens->cube, mesh(false)},
                     {SdfPath("/Look"), HdPrimTypeTokens->material, HdRetainedContainerDataSource::New()}});
    const PrimvarColorSceneIndex::RefPtr filter = PrimvarColorSceneIndex::New(input);
    CHECK(filter->geometryCount() == 3, "what's there already: meshes and the cube, not the material (%zu)", filter->geometryCount());
    Counter counter;
    filter->AddObserver(HdSceneIndexObserverPtr(&counter));

    const auto displayColor = [&filter](const char* path) -> VtValue {
        const HdPrimvarSchema color = HdPrimvarsSchema::GetFromParent(filter->GetPrim(SdfPath(path)).dataSource).GetPrimvar(HdTokens->displayColor);
        return color && color.GetPrimvarValue() ? color.GetPrimvarValue()->GetValue(0) : VtValue();
    };
    const auto bound = [&filter](const char* path) {
        return bool(HdContainerDataSource::Cast(filter->GetPrim(SdfPath(path)).dataSource)->Get(HdMaterialBindingsSchema::GetSchemaToken()));
    };
    CHECK(displayColor("/Hot").IsEmpty() && bound("/Hot"), "off: the scene as it is");

    auto state = std::make_shared<PrimvarColorSceneIndex::State>();
    state->primvar = TfToken("heat");
    state->colors = PrimvarColorizers::instance().find("ramp")->colors;
    state->input.range = {0, 10};
    filter->setState(state);
    CHECK(counter.dirtied == 3, "every piece of geometry is dirtied (%zu)", counter.dirtied);
    const VtValue hot = displayColor("/Hot");
    CHECK(hot.IsHolding<VtVec3fArray>() && hot.UncheckedGet<VtVec3fArray>().size() == 2 &&
              close(hot.UncheckedGet<VtVec3fArray>()[1], primvarRampStops().back()),
          "its colours");
    CHECK(!bound("/Hot") && !bound("/Plain"), "and no material");
    const VtValue plain = displayColor("/Plain");
    CHECK(plain.IsHolding<VtVec3fArray>() && close(plain.UncheckedGet<VtVec3fArray>()[0], primvarMissingColor()), "grey without it");
    CHECK(!displayColor("/Box").IsEmpty(), "a cube too");
    CHECK(displayColor("/Look").IsEmpty(), "not a material");

    // Its source's edits reach its colours.
    counter.dirtied = 0;
    input->DirtyPrims({{SdfPath("/Hot"), HdDataSourceLocatorSet{HdPrimvarsSchema::GetDefaultLocator().Append(TfToken("heat"))}}});
    CHECK(counter.dirtied == 1, "dirtied with the primvar");

    input->RemovePrims({SdfPath("/Plain")});
    input->AddPrims({{SdfPath("/New"), HdPrimTypeTokens->basisCurves, mesh(false)}});
    CHECK(filter->geometryCount() == 3, "follows what comes and goes");
    filter->setState(nullptr);
    CHECK(displayColor("/Hot").IsEmpty() && bound("/Hot"), "and off again");
    filter->RemoveObserver(HdSceneIndexObserverPtr(&counter));
}
