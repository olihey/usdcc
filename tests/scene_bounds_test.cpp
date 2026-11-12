// What Fit to View frames (scene_bounds.h): the world-space box around the selected
// prims, or the whole stage, as the viewport draws them. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/scene_bounds.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <cstdio>
#include <optional>
#include <string>

using usdcc::usd::sceneBounds;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const char* kScene = R"(#usda 1.0
def Xform "World"
{
    double3 xformOp:translate = (10, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]

    def Cube "Box"
    {
        double size = 2
    }

    def Sphere "Ball"
    {
        double radius = 1
        double3 xformOp:translate = (0, 5, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Cube "Hidden"
    {
        token visibility = "invisible"
        double3 xformOp:translate = (100, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Cube "Guide"
    {
        uniform token purpose = "guide"
        double3 xformOp:translate = (0, -100, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}

def Scope "Looks"
{
    def Material "Mat"
    {
    }
}
)";

UsdStageRefPtr makeStage() {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->GetRootLayer()->ImportFromString(kScene);
    return stage;
}

bool closeTo(const GfRange3d& a, const GfRange3d& b) {
    return (a.GetMin() - b.GetMin()).GetLength() < 1e-6 && (a.GetMax() - b.GetMax()).GetLength() < 1e-6;
}

std::string text(const std::optional<GfRange3d>& range) {
    if (!range) return "none";
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "(%g %g %g)-(%g %g %g)", range->GetMin()[0], range->GetMin()[1],
                  range->GetMin()[2], range->GetMax()[0], range->GetMax()[1], range->GetMax()[2]);
    return buffer;
}
}  // namespace

USDCC_TEST(scene_bounds_of_selected_prims) {
    const UsdStageRefPtr stage = makeStage();
    const GfRange3d box(GfVec3d(9, -1, -1), GfVec3d(11, 1, 1));
    const GfRange3d ball(GfVec3d(9, 4, -1), GfVec3d(11, 6, 1));

    auto bounds = sceneBounds(stage, {SdfPath("/World/Box")});
    CHECK(bounds && closeTo(*bounds, box), "a cube, in world space (its extent computed): %s", text(bounds).c_str());
    bounds = sceneBounds(stage, {SdfPath("/World/Box"), SdfPath("/World/Ball")});
    CHECK(bounds && closeTo(*bounds, GfRange3d(GfVec3d(9, -1, -1), GfVec3d(11, 6, 1))), "two prims: around both: %s",
          text(bounds).c_str());
    bounds = sceneBounds(stage, {SdfPath("/World")});
    CHECK(bounds && closeTo(*bounds, GfRange3d(GfVec3d(9, -1, -1), GfVec3d(11, 6, 1))),
          "a parent: its visible, drawn children, not the hidden or guide ones: %s", text(bounds).c_str());
    bounds = sceneBounds(stage, {SdfPath("/World/Box"), SdfPath("/Looks/Mat")});
    CHECK(bounds && closeTo(*bounds, box), "a prim without bounds adds nothing: %s", text(bounds).c_str());
    bounds = sceneBounds(stage, {SdfPath("/World/Box.size")});
    CHECK(bounds && closeTo(*bounds, box), "a property path stands for its prim");

    CHECK(!sceneBounds(stage, {SdfPath("/Looks/Mat")}), "a material has none");
    CHECK(!sceneBounds(stage, {SdfPath("/Looks"), SdfPath("/World/Hidden"), SdfPath("/World/Guide")}),
          "nor a scope, a hidden prim, or a guide: %s",
          text(sceneBounds(stage, {SdfPath("/Looks"), SdfPath("/World/Hidden"), SdfPath("/World/Guide")})).c_str());
    CHECK(!sceneBounds(stage, {SdfPath("/Missing")}), "nor a path with no prim");
    UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World"))).MakeInvisible();
    CHECK(!sceneBounds(stage, {SdfPath("/World/Box")}), "nor a prim under a hidden parent");
    CHECK(!sceneBounds(stage, {}), "and with that hidden, the stage has nothing to show");
    CHECK(!sceneBounds(nullptr, {}), "nor no stage");
}

USDCC_TEST(scene_bounds_of_the_whole_stage) {
    const UsdStageRefPtr stage = makeStage();
    const auto bounds = sceneBounds(stage, {});
    CHECK(bounds && closeTo(*bounds, GfRange3d(GfVec3d(9, -1, -1), GfVec3d(11, 6, 1))),
          "nothing selected: everything drawn: %s", text(bounds).c_str());

    // With guides shown (docs/VIEWPORT_DISPLAY.md): the guide cube counts too.
    const auto withGuides = sceneBounds(stage, {}, UsdTimeCode::Default(),
                                        {UsdGeomTokens->default_, UsdGeomTokens->guide, UsdGeomTokens->proxy});
    CHECK(withGuides && closeTo(*withGuides, GfRange3d(GfVec3d(9, -101, -1), GfVec3d(11, 6, 1))),
          "guides shown: the guide adds its box: %s", text(withGuides).c_str());
    CHECK(sceneBounds(stage, {SdfPath("/World/Guide")}, UsdTimeCode::Default(),
                      {UsdGeomTokens->default_, UsdGeomTokens->guide}),
          "and a selected guide has bounds");

    const UsdStageRefPtr empty = UsdStage::CreateInMemory();
    empty->DefinePrim(SdfPath("/Empty"), TfToken("Xform"));
    CHECK(!sceneBounds(empty, {}), "a stage with no geometry has none");
}
