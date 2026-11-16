#include "usdcc/usd/physics_menu.h"

#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdPhysics/collisionAPI.h>
#include <pxr/usd/usdPhysics/meshCollisionAPI.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/scene.h>
#include <pxr/usd/usdPhysics/tokens.h>

#include <QAction>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace physics_actions {

namespace {

// The gprims at or under `prim`, outside instances.
std::vector<UsdPrim> geometryOf(const UsdPrim& prim) {
    std::vector<UsdPrim> found;
    for (const UsdPrim& each : UsdPrimRange(prim)) {
        if (each.IsInstanceProxy()) continue;
        if (each.IsA<UsdGeomGprim>()) found.push_back(each);
    }
    return found;
}

// The prims that can be edited: there, not inside an instance, reachable by the edit target.
std::vector<UsdPrim> editable(const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    std::vector<UsdPrim> prims;
    for (const SdfPath& path : paths) {
        const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
        if (prim && !prim.IsInstanceProxy() && !prim.IsPseudoRoot() && editTargetReaches(prim)) prims.push_back(prim);
    }
    return prims;
}

void addColliders(const UsdPrim& prim, bool moving) {
    for (const UsdPrim& geometry : geometryOf(prim)) {
        if (!geometry.HasAPI<UsdPhysicsCollisionAPI>()) UsdPhysicsCollisionAPI::Apply(geometry);
        if (moving && geometry.IsA<UsdGeomMesh>()) {
            UsdPhysicsMeshCollisionAPI api = UsdPhysicsMeshCollisionAPI::Apply(geometry);
            if (!api.GetApproximationAttr().HasAuthoredValue()) api.CreateApproximationAttr().Set(UsdPhysicsTokens->convexHull);
        }
    }
}

// A free name for a prim under the root: `base`, then base1, base2, ...
SdfPath freePath(const UsdStageRefPtr& stage, const std::string& base) {
    SdfPath path = SdfPath::AbsoluteRootPath().AppendChild(TfToken(base));
    for (int i = 1; stage->GetPrimAtPath(path); ++i) {
        path = SdfPath::AbsoluteRootPath().AppendChild(TfToken(base + std::to_string(i)));
    }
    return path;
}

}  // namespace

bool makeRigidBody(UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    if (!stage) return false;
    const std::vector<UsdPrim> prims = editable(stage, paths);
    if (prims.empty()) return false;
    return undo.apply(stage, QObject::tr("Make Rigid Body"), [&prims]() {
        for (const UsdPrim& prim : prims) {
            if (!prim.HasAPI<UsdPhysicsRigidBodyAPI>()) UsdPhysicsRigidBodyAPI::Apply(prim);
            addColliders(prim, true);
        }
        return true;
    });
}

bool makeStaticCollider(UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    if (!stage) return false;
    const std::vector<UsdPrim> prims = editable(stage, paths);
    if (prims.empty()) return false;
    return undo.apply(stage, QObject::tr("Make Static Collider"), [&prims]() {
        for (const UsdPrim& prim : prims) addColliders(prim, false);
        return true;
    });
}

SdfPath addGroundPlane(UndoManager& undo, const StageRefPtr& stage) {
    if (!stage) return {};
    const UsdStageRefPtr& usd = stage->usdStage();
    const SdfPath path = freePath(usd, "GroundPlane");
    const double size = 20.0 / UsdGeomGetStageMetersPerUnit(usd);  // 20 m across
    const bool done = undo.apply(stage, QObject::tr("Add Ground Plane"), [&]() {
        UsdGeomPlane plane = UsdGeomPlane::Define(usd, path);
        if (!plane) return false;
        plane.CreateAxisAttr().Set(UsdGeomGetStageUpAxis(usd));
        plane.CreateWidthAttr().Set(size);
        plane.CreateLengthAttr().Set(size);
        UsdPhysicsCollisionAPI::Apply(plane.GetPrim());
        return true;
    });
    if (!done) return {};
    stage->primCreated(path);
    return path;
}

SdfPath addPhysicsScene(UndoManager& undo, const StageRefPtr& stage) {
    if (!stage) return {};
    const UsdStageRefPtr& usd = stage->usdStage();
    const SdfPath path = freePath(usd, "PhysicsScene");
    const bool done = undo.apply(stage, QObject::tr("Add Physics Scene"),
                                 [&]() { return static_cast<bool>(UsdPhysicsScene::Define(usd, path)); });
    if (!done) return {};
    stage->primCreated(path);
    return path;
}

}  // namespace physics_actions

PhysicsMenu::PhysicsMenu(StageManager* stageManager, std::shared_ptr<UndoManager> undo, PlaybackController* playback,
                         QWidget* parent)
    : QMenu(tr("&Physics"), parent), m_stageManager(stageManager), m_undo(std::move(undo)), m_playback(playback) {
    setObjectName(QStringLiteral("physicsMenu"));
    setToolTipsVisible(true);
    connect(this, &QMenu::aboutToShow, this, &PhysicsMenu::rebuild);
    rebuild();
}

void PhysicsMenu::rebuild() {
    clear();
    const StageRefPtr stage = m_stageManager->currentStage();
    const std::vector<SdfPath> selection = stage ? m_stageManager->selectedPaths(stage) : std::vector<SdfPath>{};
    const bool canEdit = stage && m_undo;

    auto select = [this, stage](const SdfPath& path) {
        if (!path.IsEmpty()) m_stageManager->setSelectedPaths(stage, {path});
    };
    QAction* body = addAction(tr("Make Rigid Body"), this, [this, stage, selection]() {
        physics_actions::makeRigidBody(*m_undo, stage, selection);
    });
    body->setObjectName(QStringLiteral("physicsMakeRigidBody"));
    body->setToolTip(tr("The selection moves under gravity and collides: RigidBodyAPI on it, CollisionAPI on its "
                        "geometry, and a convex hull for its meshes"));
    body->setEnabled(canEdit && !selection.empty());
    QAction* collider = addAction(tr("Make Static Collider"), this, [this, stage, selection]() {
        physics_actions::makeStaticCollider(*m_undo, stage, selection);
    });
    collider->setObjectName(QStringLiteral("physicsMakeStaticCollider"));
    collider->setToolTip(tr("The selection stays where it is, and bodies land on it: CollisionAPI on its geometry"));
    collider->setEnabled(canEdit && !selection.empty());
    QAction* ground = addAction(tr("Add Ground Plane"), this,
                                [this, stage, select]() { select(physics_actions::addGroundPlane(*m_undo, stage)); });
    ground->setObjectName(QStringLiteral("physicsAddGroundPlane"));
    ground->setToolTip(tr("A plane at the up axis' zero that bodies land on"));
    ground->setEnabled(canEdit);
    QAction* scene = addAction(tr("Add Physics Scene"), this,
                               [this, stage, select]() { select(physics_actions::addPhysicsScene(*m_undo, stage)); });
    scene->setObjectName(QStringLiteral("physicsAddScene"));
    scene->setToolTip(tr("Gravity for the stage's simulation; without one, it uses Earth's down the up axis"));
    scene->setEnabled(canEdit);

    addSeparator();
    QAction* simulate = addAction(tr("Simulate"));
    simulate->setObjectName(QStringLiteral("physicsSimulate"));
    simulate->setCheckable(true);
    simulate->setChecked(m_playback && m_playback->simulates());
    simulate->setEnabled(m_playback != nullptr);
    simulate->setToolTip(tr("Playing the timeline solves the stage's physics, frame by frame"));
    connect(simulate, &QAction::triggered, this, [this](bool on) {
        if (m_playback) m_playback->setSimulates(on);
    });
    QAction* reset = addAction(tr("Reset Simulation"), this, [this]() {
        if (m_playback) m_playback->resetSimulation();
    });
    reset->setObjectName(QStringLiteral("physicsResetSimulation"));
    reset->setToolTip(tr("Clear the cached frames and go to the first frame"));
    physics::PhysicsSession* session = stage ? stage->physicsIfStarted() : nullptr;
    reset->setEnabled(m_playback && session && session->cachedRange());
}

}  // namespace usdcc::usd
