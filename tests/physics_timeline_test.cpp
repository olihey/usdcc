// Phase 2 of docs/PHYSICS.md: the timeline drives the simulation (Simulate, Reset, the
// cached band, stale marking, the playback rule of §4.3), the Physics menu's actions, the
// session file and the Layer Editor's entry. Runs in usdcc_ui_tests, with Jolt.
#include "test_harness.h"
#include "test_settings.h"

#include "jolt_backend.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/layer_stack.h"
#include "usdcc/usd/physics_backend.h"
#include "usdcc/usd/physics_menu.h"
#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/sim_cache.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/timeline_bar.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdPhysics/collisionAPI.h>
#include <pxr/usd/usdPhysics/meshCollisionAPI.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/scene.h>
#include <pxr/usd/usdPhysics/tokens.h>

#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <memory>
#include <vector>

using namespace usdcc::usd;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Jolt as the app's backend while it lives, as the bundled plugin makes it.
struct WithJolt {
    WithJolt() { physics::PhysicsBackends::instance().add(usdcc::physics_jolt::makeJoltBackend()); }
    ~WithJolt() { physics::PhysicsBackends::instance().remove("org.usdcc.physics-jolt"); }
};

// A 1 m cube five metres over a ground plane, in the stage manager, as the current stage.
StageRefPtr cubeOverPlane(StageManager& stages) {
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& usd = stage->usdStage();
    UsdGeomSetStageUpAxis(usd, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(usd, 1.0);
    UsdPhysicsScene::Define(usd, SdfPath("/PhysicsScene"));
    UsdGeomPlane ground = UsdGeomPlane::Define(usd, SdfPath("/Ground"));
    ground.CreateAxisAttr().Set(UsdGeomTokens->y);
    UsdPhysicsCollisionAPI::Apply(ground.GetPrim());
    UsdGeomCube cube = UsdGeomCube::Define(usd, SdfPath("/Cube"));
    cube.CreateSizeAttr().Set(1.0);
    cube.AddTranslateOp().Set(GfVec3d(0, 5, 0));
    UsdPhysicsRigidBodyAPI::Apply(cube.GetPrim());
    UsdPhysicsCollisionAPI::Apply(cube.GetPrim());
    stages.setCurrentStage(stage);
    return stage;
}

double height(const StageRefPtr& stage, double time) {
    return UsdGeomXformCache(UsdTimeCode(time))
        .GetLocalToWorldTransform(stage->usdStage()->GetPrimAtPath(SdfPath("/Cube")))
        .ExtractTranslation()[1];
}

QAction* actionNamed(const QMenu& menu, const char* name) {
    for (QAction* action : menu.actions()) {
        if (action->objectName() == QLatin1String(name)) return action;
    }
    return nullptr;
}

}  // namespace

// §4.3: with Simulate on, playing past the cache solves the next frame first; inside it,
// plain playback; Play while stale solves anew from the first frame.
USDCC_TEST(physics_timeline_playback) {
    TestSettings settings;
    WithJolt jolt;
    StageManager stages;
    const StageRefPtr stage = cubeOverPlane(stages);
    stage->setPlaybackRange({1.0, 48.0});
    PlaybackController playback(&stages);
    CHECK(!playback.simulates(), "Simulate is off to start with");

    // Off: plain playback, nothing solved.
    playback.play();
    playback.advance(0.0);
    CHECK(stage->time() == 2.0 && !stage->physicsIfStarted(), "off: playing solves nothing");
    playback.stop();

    stage->setTime(10.0);
    playback.setSimulates(true);
    playback.play();
    CHECK(stage->time() == 1.0, "Play with nothing cached: from the first frame (%g)", stage->time());
    physics::PhysicsSession& session = stage->physics();
    CHECK(session.hasWorld() && session.cachedRange() && session.cachedRange()->end == 1.0, "solving from it");
    std::vector<double> shown;
    for (int i = 0; i < 5; ++i) {
        playback.advance(0.0);
        shown.push_back(stage->time());
    }
    CHECK(shown == (std::vector<double>{2, 3, 4, 5, 6}) && session.cachedRange()->end == 6.0,
          "every frame, each solved as it's reached (cached to %g)", session.cachedRange()->end);
    CHECK(height(stage, 6.0) < height(stage, 1.0) && std::abs(height(stage, 1.0) - 5.0) < 1e-9,
          "and the cube falls (%g to %g)", height(stage, 1.0), height(stage, 6.0));

    // Real Time never skips a frame the solver hasn't made.
    playback.setMode(PlaybackMode::RealTime);
    playback.advance(1.0);  // 24 frames' worth
    CHECK(stage->time() == 7.0 && session.cachedRange()->end == 7.0, "Real Time waits for the solver (%g)", stage->time());
    playback.setMode(PlaybackMode::EveryFrame);
    playback.stop();

    // Scrubbing reads the cache, and past its end holds the last frame.
    const double at4 = height(stage, 4.0);
    playback.setTime(4.0);
    CHECK(session.cachedRange()->end == 7.0 && height(stage, 4.0) == at4, "scrubbing back reads the cache");
    CHECK(height(stage, 30.0) == height(stage, 7.0), "past its end: the last cached frame");

    // Playing on from inside the cache plays it, then solves on.
    playback.play();
    CHECK(stage->time() == 4.0, "Play from inside the cache goes on from there");
    for (int i = 0; i < 4; ++i) playback.advance(0.0);
    CHECK(stage->time() == 8.0 && session.cachedRange()->end == 8.0, "through it, and one past (%g)", session.cachedRange()->end);
    playback.stop();

    // An edit: stale, kept; Play solves anew from the first frame.
    UsdGeomPlane(stage->usdStage()->GetPrimAtPath(SdfPath("/Ground"))).AddTranslateOp().Set(GfVec3d(0, -1, 0));
    CHECK(session.isStale() && session.cachedRange(), "an edit makes it stale, and it stays");
    playback.play();
    CHECK(stage->time() == 1.0 && !session.isStale() && session.cachedRange()->end == 1.0, "Play: anew, from the first frame");
    playback.stop();

    // Reset clears it and goes to the first frame.
    playback.advance(0.0);
    stage->setTime(5.0);
    playback.resetSimulation();
    CHECK(!session.cachedRange() && stage->time() == 1.0 && std::abs(height(stage, 1.0) - 5.0) < 1e-9,
          "Reset: nothing cached, at the first frame, the cube where it's authored");
}

// The timeline's Simulate button and menu, and the cached band on the ruler.
USDCC_TEST(physics_timeline_bar) {
    TestSettings settings;
    WithJolt jolt;
    StageManager stages;
    const StageRefPtr stage = cubeOverPlane(stages);
    stage->setPlaybackRange({1.0, 24.0});
    PlaybackController playback(&stages);
    TimelineBar bar(&playback, std::make_shared<UndoManager>());
    CHECK(bar.simulateButton() && !bar.simulateButton()->isChecked(), "a Simulate button, off");
    bar.simulateButton()->click();
    CHECK(playback.simulates() && bar.simulateButton()->isChecked(), "clicking it turns Simulate on");
    CHECK(!bar.ruler()->cachedRange(), "nothing cached: no band");
    playback.play();
    for (int i = 0; i < 3; ++i) playback.advance(0.0);
    playback.stop();
    CHECK(bar.ruler()->cachedRange() && bar.ruler()->cachedRange()->end == 4.0 && !bar.ruler()->cacheStale(),
          "the ruler shows frames 1 to 4 cached");
    UsdGeomCube(stage->usdStage()->GetPrimAtPath(SdfPath("/Cube"))).GetSizeAttr().Set(0.5);
    CHECK(bar.ruler()->cacheStale(), "dimmed when an edit made it stale");

    QMenu menu;
    bar.populateMenu(&menu);
    QAction* simulate = actionNamed(menu, "timelineSimulate");
    QAction* reset = actionNamed(menu, "timelineResetSimulation");
    CHECK(simulate && simulate->isChecked() && reset && reset->isEnabled(), "the menu has Simulate, on, and Reset Simulation");
    if (reset) reset->trigger();
    CHECK(!bar.ruler()->cachedRange() && stage->time() == 1.0, "Reset: the band is gone");
}

// §5.5: the session file doesn't keep the cache, nor its mute; the Layer Editor lists it
// as the simulation cache, which can't be the edit target.
USDCC_TEST(physics_cache_in_session_and_layer_stack) {
    TestSettings settings;
    WithJolt jolt;
    StageManager stages;
    const StageRefPtr stage = cubeOverPlane(stages);
    CHECK(stage->physics().start(0.0) && stage->physics().stepTo(5.0), "five frames simulated");
    const std::string cache = stage->physics().cache().layer()->GetIdentifier();
    stage->usdStage()->MuteLayer(cache);

    const QJsonObject state = session_stages::capture(stages, stage);
    CHECK(!state.value("sessionLayer").toString().contains(QLatin1String(physics::SimCache::kTag)),
          "the saved session layer doesn't name the cache");
    bool mutedListed = false;
    for (const QJsonValue& muted : state.value("mutedLayers").toArray()) {
        mutedListed = mutedListed || muted.toString().contains(QLatin1String(physics::SimCache::kTag));
    }
    CHECK(!mutedListed, "nor its mute");
    stage->usdStage()->UnmuteLayer(cache);

    const std::vector<LayerEntry> tree = layerTree(stage->usdStage());
    const LayerEntry* entry = nullptr;
    for (const LayerEntry& child : tree.front().children) {
        if (child.simulationCache) entry = &child;
    }
    CHECK(tree.front().session && entry && entry->identifier == cache, "listed under the session layer, marked as the cache");
    if (entry) {
        CHECK(!layer_actions::canTarget(*entry) && !layer_actions::canRemove(*entry) && !layer_actions::canAddTo(*entry) &&
                  layer_actions::canMute(*entry),
              "it can be muted, and nothing else");
    }
}

// §5.4: the Physics menu's actions, each one undo step.
USDCC_TEST(physics_menu_actions) {
    TestSettings settings;
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    stages.setCurrentStage(stage);
    const UsdStageRefPtr& usd = stage->usdStage();
    UsdGeomSetStageUpAxis(usd, UsdGeomTokens->z);
    UsdGeomXform crate = UsdGeomXform::Define(usd, SdfPath("/Crate"));
    UsdGeomCube::Define(usd, SdfPath("/Crate/Box"));
    UsdGeomMesh::Define(usd, SdfPath("/Crate/Lid"));
    UsdGeomCube::Define(usd, SdfPath("/Floor"));
    auto undo = std::make_shared<UndoManager>();
    PlaybackController playback(&stages);
    PhysicsMenu menu(&stages, undo, &playback, nullptr);

    QAction* body = actionNamed(menu, "physicsMakeRigidBody");
    CHECK(body && !body->isEnabled(), "Make Rigid Body needs a selection");
    stages.setSelectedPaths(stage, {SdfPath("/Crate")});
    menu.rebuild();
    body = actionNamed(menu, "physicsMakeRigidBody");
    CHECK(body && body->isEnabled(), "and has one");
    const int before = undo->stack(stage)->count();
    if (body) body->trigger();
    CHECK(usd->GetPrimAtPath(SdfPath("/Crate")).HasAPI<UsdPhysicsRigidBodyAPI>(), "the selection is a rigid body");
    CHECK(usd->GetPrimAtPath(SdfPath("/Crate/Box")).HasAPI<UsdPhysicsCollisionAPI>() &&
              usd->GetPrimAtPath(SdfPath("/Crate/Lid")).HasAPI<UsdPhysicsCollisionAPI>(),
          "its geometry collides");
    TfToken approximation;
    UsdPhysicsMeshCollisionAPI(usd->GetPrimAtPath(SdfPath("/Crate/Lid"))).GetApproximationAttr().Get(&approximation);
    CHECK(approximation == UsdPhysicsTokens->convexHull, "its mesh as a convex hull (%s)", approximation.GetText());
    CHECK(undo->stack(stage)->count() == before + 1, "one undo step");
    undo->stack(stage)->undo();
    CHECK(!usd->GetPrimAtPath(SdfPath("/Crate")).HasAPI<UsdPhysicsRigidBodyAPI>() &&
              !usd->GetPrimAtPath(SdfPath("/Crate/Box")).HasAPI<UsdPhysicsCollisionAPI>(),
          "undone, all of it");

    CHECK(physics_actions::makeStaticCollider(*undo, stage, {SdfPath("/Floor")}) &&
              usd->GetPrimAtPath(SdfPath("/Floor")).HasAPI<UsdPhysicsCollisionAPI>() &&
              !usd->GetPrimAtPath(SdfPath("/Floor")).HasAPI<UsdPhysicsRigidBodyAPI>(),
          "a static collider: collision, no body");

    QAction* ground = actionNamed(menu, "physicsAddGroundPlane");
    if (ground) ground->trigger();
    const UsdGeomPlane plane(usd->GetPrimAtPath(SdfPath("/GroundPlane")));
    TfToken axis;
    plane.GetAxisAttr().Get(&axis);
    CHECK(plane && axis == UsdGeomTokens->z && plane.GetPrim().HasAPI<UsdPhysicsCollisionAPI>(),
          "a ground plane facing up the stage's up axis, colliding");
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>{SdfPath("/GroundPlane")}, "and selected");
    QAction* scene = actionNamed(menu, "physicsAddScene");
    if (scene) scene->trigger();
    CHECK(UsdPhysicsScene(usd->GetPrimAtPath(SdfPath("/PhysicsScene"))), "a physics scene");
    CHECK(physics_actions::addPhysicsScene(*undo, stage) == SdfPath("/PhysicsScene1"), "a second one gets a free name");
}
