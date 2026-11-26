// Viewport overlays (docs/PRIMVAR_VIEWER.md §9.2 point 7): the registry, and the built-in one of
// the primvar elements picked, for each kind of element; and the elements on the stage. Runs in
// usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/viewport_overlays.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::usd;

namespace {
// Two quads side by side, moved 10 along x; a curve of 3 points; an instancer of 3.
UsdStageRefPtr overlayStage() {
    const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    layer->ImportFromString(R"(#usda 1.0
def Mesh "Mesh" {
    double3 xformOp:translate = (10, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
}
def BasisCurves "Curve" {
    uniform token type = "linear"
    int[] curveVertexCounts = [3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0)]
}
def PointInstancer "Instancer" {
    int[] protoIndices = [0, 0, 0]
    point3f[] positions = [(0, 0, 0), (5, 0, 0), (9, 0, 0)]
}
)");
    return UsdStage::Open(layer);
}
}  // namespace

USDCC_TEST(viewport_overlay_primvar_elements) {
    const UsdStageRefPtr stage = overlayStage();
    const UsdTimeCode time = UsdTimeCode::Default();
    const auto build = [&](const char* prim, const char* domain, std::vector<std::size_t> indices) {
        return buildPrimvarElementsOverlay(stage, {SdfPath(prim), domain, std::move(indices)}, {}, time);
    };
    const usdcc::tools::GizmoGeometry points = build("/Mesh", "points", {0, 5, 99});
    CHECK(points.lines.size() == 6 && points.triangles.empty(), "a cross of three lines at each point, none beyond the points");
    const GfVec3d middle = (points.lines[0].a + points.lines[0].b) / 2;
    CHECK(GfIsClose(middle, GfVec3d(10, 0, 0), 1e-9), "in world space: the prim is moved 10 along x");
    const usdcc::tools::GizmoGeometry faces = build("/Mesh", "faces", {1});
    CHECK(faces.lines.size() == 4 && faces.triangles.size() == 2 && faces.triangles[0].color[3] < 1.0f, "a face's outline and fill");
    CHECK(build("/Mesh", "face corners", {5}).lines.size() == 4, "a corner: a line into its face and a cross");
    CHECK(build("/Curve", "curves", {0}).lines.size() == 2, "a curve's line");
    CHECK(build("/Instancer", "instances", {2}).lines.size() == 3, "an instance's cross");
    CHECK(build("/Mesh", "segment ends", {0}).lines.empty() && build("/Gone", "points", {0}).lines.empty(), "what it can't draw");

    const usdcc::tools::GizmoGeometry both =
        buildPrimvarElementsOverlay(stage, {SdfPath("/Mesh"), "points", {0}}, {SdfPath("/Mesh"), "points", {1}}, time);
    CHECK(both.lines.size() == 6 && both.lines.front().color != both.lines.back().color, "the hovered one in another colour");
}

USDCC_TEST(viewport_overlay_registry) {
    ViewportOverlays& overlays = ViewportOverlays::instance();
    CHECK(overlays.find(kPrimvarElementsOverlay) && overlays.overlays().front().id == kPrimvarElementsOverlay, "the built-in one first");
    const std::size_t count = overlays.overlays().size();
    int changes = 0;
    QObject::connect(&overlays, &ViewportOverlays::changed, [&changes]() { ++changes; });
    overlays.add({"test.overlay", "Test", [](const ViewportOverlayContext&) { return usdcc::tools::GizmoGeometry(); }});
    overlays.add({"test.overlay", "Test again", [](const ViewportOverlayContext&) { return usdcc::tools::GizmoGeometry(); }});
    CHECK(overlays.overlays().size() == count + 1 && overlays.find("test.overlay")->label == "Test again", "the same id replaces");
    CHECK(overlays.remove("test.overlay") && !overlays.remove("test.overlay") && changes == 3, "removed, saying so each time");
}

USDCC_TEST(stage_primvar_elements) {
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    stage->usdStage()->DefinePrim(SdfPath("/A"));
    stage->usdStage()->DefinePrim(SdfPath("/B"));
    int changes = 0;
    const auto connection = stage->primvarElementsChanged.connect([&changes]() { ++changes; });
    stages.setSelectedPaths(stage, {SdfPath("/A"), SdfPath("/B")});
    stage->setPrimvarElements({SdfPath("/A"), "points", {5, 1, 5, 3}});
    CHECK((stage->primvarElements().indices == std::vector<std::size_t>{1, 3, 5}) && changes == 1, "in order, each once");
    stage->setPrimvarElements({SdfPath("/A"), "points", {3, 1, 5}});
    CHECK(changes == 1, "the same again says nothing");
    stage->setPrimvarElementHover({SdfPath("/A"), "points", {2}});
    stages.setSelectedPaths(stage, {SdfPath("/A")});
    CHECK(!stage->primvarElements().empty() && !stage->primvarElementHover().empty(), "kept while their prim is selected");
    stages.setSelectedPaths(stage, {SdfPath("/B")});
    CHECK(stage->primvarElements().empty() && stage->primvarElementHover().empty() && changes == 2, "dropped when it isn't");
    stage->setPrimvarElements({SdfPath("/B"), "faces", {}});
    CHECK(stage->primvarElements() == PrimvarElements() && changes == 2, "none picked is nothing");
}
