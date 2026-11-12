// What the viewport HUD counts (docs/VIEWPORT_HUD.md §2.3): the prims, meshes, faces and
// points of the stage or the selection, as the viewport draws them.
#include "test_harness.h"

#include "usdcc/usd/orbit_camera.h"
#include "usdcc/usd/scene_counts.h"

#include <pxr/base/gf/math.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <cmath>
#include <string>

using namespace usdcc::usd;
using PXR_NS::SdfPath;
using PXR_NS::TfToken;
using PXR_NS::UsdStage;
using PXR_NS::UsdStageRefPtr;

namespace {
// A quad mesh with 6 faces and 8 points (a cube), and what refers to it.
const char* kScene = R"(#usda 1.0
def Xform "World"
{
    def Mesh "Box"
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
        point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
    }
    def Xform "Group"
    {
        def Mesh "Tri"
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        }
        def Points "Cloud"
        {
            point3f[] points = [(0, 0, 0), (1, 1, 1), (2, 2, 2), (3, 3, 3), (4, 4, 4)]
        }
    }
    def Mesh "Hidden"
    {
        token visibility = "invisible"
        int[] faceVertexCounts = [3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
    def Mesh "Off" (
        active = false
    )
    {
        int[] faceVertexCounts = [3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
    def Mesh "Guide"
    {
        uniform token purpose = "guide"
        int[] faceVertexCounts = [3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}

def Xform "Proto"
{
    def Mesh "Tri"
    {
        int[] faceVertexCounts = [3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}

def Xform "Instances"
{
    def "A" (
        instanceable = true
        references = </Proto>
    )
    {
    }
    def "B" (
        instanceable = true
        references = </Proto>
    )
    {
    }
}

def PointInstancer "Scatter"
{
    rel prototypes = [</Scatter/Prototypes/Box>, </Scatter/Prototypes/Tri>]
    int[] protoIndices = [0, 0, 0, 1]
    point3f[] positions = [(0, 0, 0), (2, 0, 0), (4, 0, 0), (6, 0, 0)]

    def Scope "Prototypes"
    {
        def Mesh "Box"
        {
            int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
            point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
        }
        def Mesh "Tri"
        {
            int[] faceVertexCounts = [3]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        }
    }
}
)";

UsdStageRefPtr makeStage() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->GetRootLayer()->ImportFromString(kScene);
    return stage;
}

const PXR_NS::TfTokenVector kDefaultPurposes = {PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->proxy};

std::string str(const SceneCounts& c) {
    return std::to_string(c.prims) + " prims, " + std::to_string(c.meshes) + " meshes, " + std::to_string(c.faces) +
           " faces, " + std::to_string(c.points) + " points";
}
}  // namespace

USDCC_TEST(scene_counts_roots) {
    const std::vector<SdfPath> roots =
        countRoots({SdfPath("/World/Group/Tri"), SdfPath("/World/Group"), SdfPath("/Proto"), SdfPath("/World/Group"), SdfPath()});
    CHECK(roots == std::vector<SdfPath>({SdfPath("/Proto"), SdfPath("/World/Group")}),
          "a path below another selected one, a duplicate and an empty path are left out");
    CHECK(countRoots({SdfPath::AbsoluteRootPath(), SdfPath("/World")}) == std::vector<SdfPath>({SdfPath::AbsoluteRootPath()}),
          "everything is below the root");
}

USDCC_TEST(scene_counts_selection) {
    const UsdStageRefPtr stage = makeStage();
    SceneCounts box = countScene(stage, {SdfPath("/World/Box")}, kDefaultPurposes);
    CHECK(box == SceneCounts({1, 1, 6, 8}), "a mesh: its faces and points: %s", str(box).c_str());

    const SceneCounts group = countScene(stage, {SdfPath("/World/Group")}, kDefaultPurposes);
    CHECK(group == SceneCounts({3, 1, 1, 8}), "a group with all below it, a Points prim's points too: %s", str(group).c_str());

    const SceneCounts both = countScene(stage, countRoots({SdfPath("/World/Group"), SdfPath("/World/Group/Tri")}), kDefaultPurposes);
    CHECK(both == group, "a prim selected with its parent is counted once: %s", str(both).c_str());

    CHECK(countScene(stage, {SdfPath("/World/Hidden")}, kDefaultPurposes) == SceneCounts{}, "an invisible prim: nothing");
    CHECK(countScene(stage, {SdfPath("/World/Off")}, kDefaultPurposes) == SceneCounts{}, "an inactive prim: nothing");
    CHECK(countScene(stage, {SdfPath("/World/Guide")}, kDefaultPurposes) == SceneCounts{}, "a guide while guides are off");
    const PXR_NS::TfTokenVector withGuides = {PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->guide};
    CHECK(countScene(stage, {SdfPath("/World/Guide")}, withGuides) == SceneCounts({1, 1, 1, 3}), "with guides on, it counts");
    CHECK(countScene(stage, {SdfPath("/Nowhere")}, kDefaultPurposes) == SceneCounts{}, "a path with no prim: nothing");

    const SceneCounts instance = countScene(stage, {SdfPath("/Instances/A")}, kDefaultPurposes);
    CHECK(instance == SceneCounts({2, 1, 1, 3}), "a native instance counts its prototype: %s", str(instance).c_str());
    const SceneCounts below = countScene(stage, {SdfPath("/Instances/A/Tri")}, kDefaultPurposes);
    CHECK(below == SceneCounts({1, 1, 1, 3}), "a prim inside an instance (an instance proxy): %s", str(below).c_str());

    const SceneCounts scatter = countScene(stage, {SdfPath("/Scatter")}, kDefaultPurposes);
    // The instancer itself, then 3 boxes and 1 triangle; its Prototypes scope isn't a prim of its own.
    CHECK(scatter == SceneCounts({1 + 3 + 1, 4, 3 * 6 + 1, 3 * 8 + 3}), "a point instancer, each prototype as often as used: %s",
          str(scatter).c_str());
}

USDCC_TEST(scene_counts_stage) {
    const UsdStageRefPtr stage = makeStage();
    const SceneCounts all = countScene(stage, {}, kDefaultPurposes);
    // World, Box, Group, Tri, Cloud | Proto, Proto/Tri | Instances, A, A/Tri, B, B/Tri | Scatter (5)
    const SceneCounts expected = SceneCounts({5, 2, 7, 16}) + SceneCounts({2, 1, 1, 3}) + SceneCounts({5, 2, 2, 6}) +
                                 SceneCounts({5, 4, 19, 27});
    CHECK(all == expected, "the whole stage: %s, expected %s", str(all).c_str(), str(expected).c_str());

    // A slice at a time comes to the same.
    SceneCountJob job(stage, {}, kDefaultPurposes);
    int steps = 0;
    while (!job.step(1)) ++steps;
    CHECK(job.counts() == all, "one prim a step: %s", str(job.counts()).c_str());
    CHECK(steps > 10, "it did take several steps (%d)", steps);
    CHECK(job.done() && job.step(1), "done stays done");

    // Making a prim invisible takes it and all below it out.
    stage->GetPrimAtPath(SdfPath("/World/Group")).GetAttribute(TfToken("visibility")).Set(PXR_NS::UsdGeomTokens->invisible);
    const SceneCounts hidden = countScene(stage, {}, kDefaultPurposes);
    CHECK(hidden.prims == all.prims - 3 && hidden.faces == all.faces - 1, "an invisible group: %s", str(hidden).c_str());
    CHECK(countScene(stage, {SdfPath("/World/Group/Tri")}, kDefaultPurposes) == SceneCounts{},
          "selected below an invisible prim: nothing");

    CHECK(countScene(nullptr, {}, kDefaultPurposes) == SceneCounts{}, "no stage: nothing");
}

// The axis triad's snap (docs/VIEWPORT_HUD.md §2.2): the camera ends up on the side the
// axis points to, looking back.
USDCC_TEST(orbit_angles_looking_from) {
    using PXR_NS::GfVec3d;
    const struct {
        GfVec3d direction;
        double yaw, pitch;
    } cases[] = {
        {{1, 0, 0}, 90, 0}, {{-1, 0, 0}, -90, 0}, {{0, 0, 1}, 0, 0}, {{0, 0, -1}, 180, 0}, {{0, 1, 0}, 33, 90}, {{0, -2, 0}, 33, -90},
    };
    for (const auto& c : cases) {
        const OrbitAngles angles = orbitAnglesLookingFrom(c.direction, 33.0);
        CHECK(std::abs(angles.yaw - c.yaw) < 1e-9 && std::abs(angles.pitch - c.pitch) < 1e-9,
              "(%g, %g, %g): yaw %g pitch %g, expected %g %g", c.direction[0], c.direction[1], c.direction[2], angles.yaw,
              angles.pitch, c.yaw, c.pitch);
        const OrbitCameraAxes axes = orbitCameraAxes(GfVec3d(0.0), 4.0, angles.yaw, angles.pitch);
        CHECK((axes.eye - c.direction.GetNormalized() * 4.0).GetLength() < 1e-9, "the eye is on that side");
    }
}
