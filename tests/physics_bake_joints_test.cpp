// Phases 3 and 4 of docs/PHYSICS.md, with Jolt: Bake Simulation (the cache's frames as
// local transform samples in the edit target, one undo step), joints with limits, drives
// and break forces, filtered pairs and collision groups, and Check Physics.
#include "test_harness.h"

#include "jolt_backend.h"
#include "usdcc/usd/physics_backend.h"
#include "usdcc/usd/physics_bake.h"
#include "usdcc/usd/physics_check.h"
#include "usdcc/usd/physics_menu.h"
#include "usdcc/usd/physics_overlay.h"
#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdPhysics/collisionAPI.h>
#include <pxr/usd/usdPhysics/collisionGroup.h>
#include <pxr/usd/usdPhysics/distanceJoint.h>
#include <pxr/usd/usdPhysics/driveAPI.h>
#include <pxr/usd/usdPhysics/filteredPairsAPI.h>
#include <pxr/usd/usdPhysics/fixedJoint.h>
#include <pxr/usd/usdPhysics/massAPI.h>
#include <pxr/usd/usdPhysics/revoluteJoint.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/scene.h>

#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace usdcc::usd;
using namespace usdcc::usd::physics;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Jolt as the only backend while it lives.
struct WithJolt {
    WithJolt() : m_before(PhysicsBackends::instance().all()) {
        for (const auto& backend : m_before) PhysicsBackends::instance().remove(backend->id());
        PhysicsBackends::instance().add(usdcc::physics_jolt::makeJoltBackend());
    }
    ~WithJolt() {
        PhysicsBackends::instance().remove("org.usdcc.physics-jolt");
        for (const auto& backend : m_before) PhysicsBackends::instance().add(backend);
    }
    std::vector<std::shared_ptr<PhysicsBackend>> m_before;
};

void ground(const UsdStageRefPtr& stage) {
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
    UsdGeomPlane plane = UsdGeomPlane::Define(stage, SdfPath("/Ground"));
    plane.CreateAxisAttr().Set(UsdGeomTokens->y);
    UsdPhysicsCollisionAPI::Apply(plane.GetPrim());
}

UsdPrim box(const UsdStageRefPtr& stage, const char* path, const GfVec3d& at, const GfVec3f& scale = GfVec3f(1.0f),
            bool moving = true) {
    UsdGeomCube cube = UsdGeomCube::Define(stage, SdfPath(path));
    cube.CreateSizeAttr().Set(1.0);
    cube.AddTranslateOp().Set(at);
    if (scale != GfVec3f(1.0f)) cube.AddScaleOp().Set(scale);
    if (moving) UsdPhysicsRigidBodyAPI::Apply(cube.GetPrim());
    UsdPhysicsCollisionAPI::Apply(cube.GetPrim());
    return cube.GetPrim();
}

GfMatrix4d world(const UsdStageRefPtr& stage, const char* path, double time) {
    return UsdGeomXformCache(UsdTimeCode(time)).GetLocalToWorldTransform(stage->GetPrimAtPath(SdfPath(path)));
}

// The bar's turn about Z, in degrees: where its own X axis points.
double turnAboutZ(const GfMatrix4d& m) {
    const GfVec3d x = m.TransformDir(GfVec3d(1, 0, 0)).GetNormalized();
    return std::atan2(x[1], x[0]) * 180.0 / 3.14159265358979323846;
}

bool reported(const PhysicsSession& session, const std::string& part) {
    const auto& reports = session.reports();
    return std::any_of(reports.begin(), reports.end(), [&part](const PhysicsReport& r) { return r.message.find(part) != std::string::npos; });
}

// A 2 m bar on a hinge at its left end, at (0, 5, 0), turning about Z; gravity pulls it down.
UsdPhysicsRevoluteJoint hingedBar(const UsdStageRefPtr& stage) {
    UsdPrim bar = box(stage, "/Bar", GfVec3d(1, 5, 0), GfVec3f(2.0f, 0.2f, 0.2f));
    UsdPhysicsRevoluteJoint hinge = UsdPhysicsRevoluteJoint::Define(stage, SdfPath("/Hinge"));
    hinge.CreateAxisAttr().Set(TfToken("Z"));
    hinge.CreateBody1Rel().SetTargets({bar.GetPath()});
    hinge.CreateLocalPos0Attr().Set(GfVec3f(0, 5, 0));
    hinge.CreateLocalPos1Attr().Set(GfVec3f(-0.5f, 0, 0));  // in the bar's own, scaled, frame
    return hinge;
}

}  // namespace

// §4.5, phase 3: baked, with the cache cleared, the stage shows the same transforms at every
// frame, a body under another body included; no !resetXformStack! in the user's layer; one
// undo step, which undo takes back.
USDCC_TEST(physics_bake_keeps_every_frame) {
    WithJolt jolt;
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& usd = stage->usdStage();
    ground(usd);
    box(usd, "/Crate", GfVec3d(0, 3, 0));
    box(usd, "/Crate/Lid", GfVec3d(0.2, 1.5, 0));  // a body under a body: simulated on its own
    box(usd, "/Other", GfVec3d(3, 2, 0));
    stage->setPlaybackRange({1.0, 30.0});

    PhysicsSession& session = stage->physics();
    CHECK(session.start(1.0) && session.stepTo(30.0), "thirty frames simulated");
    std::map<std::string, std::vector<GfMatrix4d>> simulated;
    for (const char* path : {"/Crate", "/Crate/Lid", "/Other"}) {
        for (int f = 1; f <= 30; ++f) simulated[path].push_back(world(usd, path, f));
    }

    UndoManager undo;
    const int before = undo.stack(stage)->count();
    const BakeResult result = physics_actions::bakeSimulation(undo, stage, {});
    CHECK(result.baked.size() == 3 && result.reports.empty(), "three bodies baked (%zu), nothing reported (%zu)",
          result.baked.size(), result.reports.size());
    CHECK(!session.cachedRange(), "the cache is cleared");
    CHECK(undo.stack(stage)->count() == before + 1, "one undo step");
    double worst = 0.0;
    for (const auto& [path, frames] : simulated) {
        for (int f = 1; f <= 30; ++f) {
            const GfMatrix4d baked = world(usd, path.c_str(), f);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::abs(baked[r][c] - frames[f - 1][r][c]));
        }
    }
    CHECK(worst < 1e-4, "every frame of every body as simulated, the nested one too (worst %.2g)", worst);
    const VtTokenArray order = usd->GetRootLayer()
                                   ->GetAttributeAtPath(SdfPath("/Crate/Lid.xformOpOrder"))
                                   ->GetDefaultValue()
                                   .GetWithDefault<VtTokenArray>();
    CHECK(std::find(order.begin(), order.end(), TfToken("!resetXformStack!")) == order.end() && !order.empty(),
          "the user's layer has plain local ops, no !resetXformStack!");

    undo.stack(stage)->undo();
    CHECK(GfIsClose(world(usd, "/Crate", 20).ExtractTranslation(), GfVec3d(0, 3, 0), 1e-9), "undone: as authored again");
}

// Phase 3: baking the selection leaves the rest; marking kinematic; a stale cache is solved
// anew first.
USDCC_TEST(physics_bake_selection_and_stale) {
    WithJolt jolt;
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& usd = stage->usdStage();
    ground(usd);
    box(usd, "/A", GfVec3d(0, 3, 0));
    box(usd, "/B", GfVec3d(3, 3, 0));
    stage->setPlaybackRange({1.0, 12.0});
    PhysicsSession& session = stage->physics();
    CHECK(session.start(1.0) && session.stepTo(6.0), "half the range simulated");
    // An edit: the cache is stale, and a bake solves anew before it writes.
    UsdGeomCube(usd->GetPrimAtPath(SdfPath("/B"))).GetSizeAttr().Set(0.5);
    CHECK(session.isStale(), "stale");

    UndoManager undo;
    BakeOptions options;
    options.prims = {SdfPath("/A")};
    options.markKinematic = true;
    const BakeResult result = physics_actions::bakeSimulation(undo, stage, options);
    CHECK(result.baked == std::vector<SdfPath>{SdfPath("/A")}, "only the selection's body is baked");
    CHECK(result.frames && result.frames->start == 1.0 && result.frames->end == 12.0, "over the whole playback range");
    CHECK(world(usd, "/A", 12).ExtractTranslation()[1] < 2.9, "it falls in the baked frames");
    CHECK(GfIsClose(world(usd, "/B", 12).ExtractTranslation(), GfVec3d(3, 3, 0), 1e-9), "the other is as authored");
    bool kinematic = false;
    UsdPhysicsRigidBodyAPI(usd->GetPrimAtPath(SdfPath("/A"))).GetKinematicEnabledAttr().Get(&kinematic);
    CHECK(kinematic, "the baked body is marked kinematic");
}

// §5.3, phase 4: a hinge stays within its limits; without them, it swings further.
USDCC_TEST(physics_joint_hinge_limits) {
    WithJolt jolt;
    for (const bool limited : {false, true}) {
        UsdStageRefPtr usd = UsdStage::CreateInMemory();
        ground(usd);
        UsdPhysicsRevoluteJoint hinge = hingedBar(usd);
        if (limited) {
            hinge.CreateLowerLimitAttr().Set(-30.0f);
            hinge.CreateUpperLimitAttr().Set(30.0f);
        }
        Stage stage(usd);
        CHECK(stage.physics().start(0.0) && stage.physics().stepTo(48.0), "two seconds simulated");
        double lowest = 0.0;
        for (int f = 0; f <= 48; ++f) lowest = std::min(lowest, turnAboutZ(world(usd, "/Bar", f)));
        const GfVec3d pivotEnd = GfVec3d(0, 5, 0);
        const GfVec3d end = world(usd, "/Bar", 48).Transform(GfVec3d(-0.5, 0, 0));
        CHECK((end - pivotEnd).GetLength() < 0.02, "the bar's end stays on the hinge (%.4f m off)", (end - pivotEnd).GetLength());
        if (limited) {
            CHECK(lowest > -31.5, "limited to 30 degrees, it stops there (%.1f)", lowest);
        } else {
            CHECK(lowest < -60.0, "unlimited, it swings well past that (%.1f)", lowest);
        }
    }
}

// §5.3: a drive turns the hinge to its target, with no gravity to fight.
USDCC_TEST(physics_joint_drive) {
    WithJolt jolt;
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    ground(usd);
    UsdPhysicsScene::Define(usd, SdfPath("/PhysicsScene")).CreateGravityMagnitudeAttr().Set(0.0f);
    UsdPhysicsRevoluteJoint hinge = hingedBar(usd);
    UsdPhysicsDriveAPI drive = UsdPhysicsDriveAPI::Apply(hinge.GetPrim(), TfToken("angular"));
    drive.CreateTargetPositionAttr().Set(45.0f);
    drive.CreateStiffnessAttr().Set(100.0f);  // N m per degree
    drive.CreateDampingAttr().Set(20.0f);
    Stage stage(usd);
    CHECK(stage.physics().start(0.0) && stage.physics().stepTo(96.0), "four seconds simulated");
    const double angle = turnAboutZ(world(usd, "/Bar", 96));
    CHECK(std::abs(angle - 45.0) < 2.0, "the drive brought it to 45 degrees (%.2f)", angle);
}

// §5.3: a joint with a break force lets go under the load; a strong one holds.
USDCC_TEST(physics_joint_breaks) {
    WithJolt jolt;
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    ground(usd);
    for (const auto& [path, at, force] : {std::tuple{"/Weak", 0.0, 100.0f}, std::tuple{"/Strong", 3.0, 1.0e9f}}) {
        const std::string body = std::string(path) + "Box";
        box(usd, body.c_str(), GfVec3d(at, 4, 0));  // 1000 kg: about 9800 N of weight
        UsdPhysicsFixedJoint joint = UsdPhysicsFixedJoint::Define(usd, SdfPath(path));
        joint.CreateBody1Rel().SetTargets({SdfPath(body)});
        joint.CreateLocalPos0Attr().Set(GfVec3f(float(at), 4, 0));
        joint.CreateBreakForceAttr().Set(force);
    }
    Stage stage(usd);
    CHECK(stage.physics().start(0.0) && stage.physics().stepTo(48.0), "two seconds simulated");
    CHECK(world(usd, "/WeakBox", 48).ExtractTranslation()[1] < 1.0, "the weak joint broke and its box fell (%.2f)",
          world(usd, "/WeakBox", 48).ExtractTranslation()[1]);
    CHECK(std::abs(world(usd, "/StrongBox", 48).ExtractTranslation()[1] - 4.0) < 0.01, "the strong one holds (%.3f)",
          world(usd, "/StrongBox", 48).ExtractTranslation()[1]);
    CHECK(reported(stage.physics(), "broke"), "the break is reported");
}

// §5.3: a distance joint keeps a ball within its reach.
USDCC_TEST(physics_joint_distance) {
    WithJolt jolt;
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    ground(usd);
    UsdGeomSphere ball = UsdGeomSphere::Define(usd, SdfPath("/Ball"));
    ball.CreateRadiusAttr().Set(0.2);
    ball.AddTranslateOp().Set(GfVec3d(0.5, 4, 0));
    UsdPhysicsRigidBodyAPI::Apply(ball.GetPrim());
    UsdPhysicsCollisionAPI::Apply(ball.GetPrim());
    UsdPhysicsDistanceJoint rope = UsdPhysicsDistanceJoint::Define(usd, SdfPath("/Rope"));
    rope.CreateBody1Rel().SetTargets({ball.GetPath()});
    rope.CreateLocalPos0Attr().Set(GfVec3f(0, 4, 0));
    rope.CreateMaxDistanceAttr().Set(1.0f);
    Stage stage(usd);
    CHECK(stage.physics().start(0.0) && stage.physics().stepTo(48.0), "two seconds simulated");
    double furthest = 0.0;
    for (int f = 0; f <= 48; ++f) furthest = std::max(furthest, (world(usd, "/Ball", f).ExtractTranslation() - GfVec3d(0, 4, 0)).GetLength());
    CHECK(furthest < 1.03, "never further than its rope (%.3f m)", furthest);
    CHECK(furthest > 0.95, "and it does hang on it (%.3f m)", furthest);
}

// §5.3: filtered pairs pass through each other; a collision group filtered from the floor's
// group falls through it.
USDCC_TEST(physics_filtering) {
    WithJolt jolt;
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    ground(usd);
    box(usd, "/Lower", GfVec3d(0, 0.5, 0));
    UsdPrim upper = box(usd, "/Upper", GfVec3d(0, 3, 0));
    UsdPhysicsFilteredPairsAPI::Apply(upper).CreateFilteredPairsRel().AddTarget(SdfPath("/Lower"));
    box(usd, "/Stacked", GfVec3d(3, 3, 0));
    box(usd, "/Base", GfVec3d(3, 0.5, 0));
    // A floor at y = 10 that a box of group A falls through, and a box of group B rests on.
    box(usd, "/Floor", GfVec3d(-6, 10, 0), GfVec3f(6.0f, 0.2f, 6.0f), false);
    box(usd, "/Ghost", GfVec3d(-7, 12, 0));
    box(usd, "/Solid", GfVec3d(-5, 12, 0));
    UsdPhysicsCollisionGroup groupA = UsdPhysicsCollisionGroup::Define(usd, SdfPath("/GroupA"));
    UsdPhysicsCollisionGroup groupB = UsdPhysicsCollisionGroup::Define(usd, SdfPath("/GroupB"));
    groupA.CreateFilteredGroupsRel().AddTarget(groupB.GetPath());
    groupA.GetCollidersCollectionAPI().CreateIncludesRel().AddTarget(SdfPath("/Ghost"));
    groupB.GetCollidersCollectionAPI().CreateIncludesRel().AddTarget(SdfPath("/Floor"));
    // Group C is inverted and lists only itself: it collides with no other group, so its
    // box falls through B's floor too. (Group A and B don't author invertFilteredGroups,
    // which OpenUSD's parser left uninitialized: on Linux, A came out inverted.)
    box(usd, "/Inverted", GfVec3d(-9, 12, 0));
    UsdPhysicsCollisionGroup groupC = UsdPhysicsCollisionGroup::Define(usd, SdfPath("/GroupC"));
    groupC.CreateInvertFilteredGroupsAttr(VtValue(true));
    groupC.CreateFilteredGroupsRel().AddTarget(groupC.GetPath());
    groupC.GetCollidersCollectionAPI().CreateIncludesRel().AddTarget(SdfPath("/Inverted"));
    Stage stage(usd);
    CHECK(stage.physics().start(0.0) && stage.physics().stepTo(72.0), "three seconds simulated");
    const double upperY = world(usd, "/Upper", 72).ExtractTranslation()[1];
    const double stackedY = world(usd, "/Stacked", 72).ExtractTranslation()[1];
    CHECK(std::abs(upperY - 0.5) < 0.05, "the filtered pair pass through each other: on the ground (%.3f)", upperY);
    CHECK(std::abs(stackedY - 1.5) < 0.05, "an unfiltered pair stack (%.3f)", stackedY);
    CHECK(world(usd, "/Ghost", 72).ExtractTranslation()[1] < 5.0, "group A falls through group B's floor (%.2f)",
          world(usd, "/Ghost", 72).ExtractTranslation()[1]);
    CHECK(std::abs(world(usd, "/Solid", 72).ExtractTranslation()[1] - 10.6) < 0.05, "a box in no group rests on it (%.3f)",
          world(usd, "/Solid", 72).ExtractTranslation()[1]);
    CHECK(world(usd, "/Inverted", 72).ExtractTranslation()[1] < 5.0, "an inverted group falls through it too (%.2f)",
          world(usd, "/Inverted", 72).ExtractTranslation()[1]);
}

// §4.8, §5.4: Check Physics has the validators' findings and the simulation's reports.
USDCC_TEST(physics_check) {
    WithJolt jolt;
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    ground(usd);
    UsdPrim bad = box(usd, "/Bad", GfVec3d(0, 2, 0));
    UsdPhysicsMassAPI::Apply(bad).CreateMassAttr().Set(-1.0f);
    const std::vector<CheckFinding> findings = checkPhysics(usd, 0.0);
    const bool validator = std::any_of(findings.begin(), findings.end(), [](const CheckFinding& f) {
        return f.source == CheckFinding::Source::Validator && f.prim == SdfPath("/Bad");
    });
    const bool simulation = std::any_of(findings.begin(), findings.end(), [](const CheckFinding& f) {
        return f.source == CheckFinding::Source::Simulation && f.message.find("PhysicsScene") != std::string::npos;
    });
    CHECK(validator, "the validators flag the negative mass");
    CHECK(simulation, "and the simulation says it has no PhysicsScene");
}

// §5.1: the collider overlay draws every collider and joint, where the simulation has them.
USDCC_TEST(physics_overlay_lines) {
    WithJolt jolt;
    UsdStageRefPtr usd = UsdStage::CreateInMemory();
    ground(usd);
    hingedBar(usd);
    const usdcc::tools::GizmoGeometry lines = buildPhysicsOverlay(usd, UsdTimeCode(0.0));
    // A plane's square and normal (5), a box's twelve edges, two frames' three axes and the link.
    CHECK(lines.lines.size() >= 5 + 12 + 6, "lines for the ground, the bar and the hinge (%zu)", lines.lines.size());
    double highest = -1e9;
    for (const auto& line : lines.lines) highest = std::max({highest, line.a[1], line.b[1]});
    CHECK(std::abs(highest - 5.1) < 0.2, "the bar's box where the bar is (top at %.3f)", highest);
}
