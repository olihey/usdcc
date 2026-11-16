#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/physics_bake.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/sdf/path.h>

#include <QMenu>

#include <functional>
#include <memory>
#include <vector>

namespace usdcc::usd {

class PlaybackController;
class StageManager;
class UndoManager;

// What the Physics menu does to a stage (docs/PHYSICS.md §5.4), each one undo step in the
// edit target, through UsdPhysics' own Apply().
namespace physics_actions {

// RigidBodyAPI on each prim, CollisionAPI on its geometry (it and the gprims under it),
// and MeshCollisionAPI with a convex hull on its meshes: a moving body can't collide as a
// triangle mesh. Prims inside an instance are left alone. False if nothing changed.
bool makeRigidBody(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& prims);
// CollisionAPI on the prims' geometry, no body: it stays where it is, and things land on it.
bool makeStaticCollider(UndoManager& undo, const StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& prims);
// A Plane with CollisionAPI at the up axis' zero, 20 m across. Its path; empty if refused.
PXR_NS::SdfPath addGroundPlane(UndoManager& undo, const StageRefPtr& stage);
// A PhysicsScene: Earth's gravity down the up axis. Its path; empty if refused.
PXR_NS::SdfPath addPhysicsScene(UndoManager& undo, const StageRefPtr& stage);

// Bake Simulation (docs/PHYSICS.md §4.5), over the stage's playback range: solves what the
// cache lacks first (anew if it's stale), with `progress(done, total)` after each frame,
// which cancels by returning false; then writes the chosen bodies' frames into the edit
// target as one undo step, and clears the cache. Nothing is written when cancelled.
physics::BakeResult bakeSimulation(UndoManager& undo, const StageRefPtr& stage, const physics::BakeOptions& options,
                                   const std::function<bool(int, int)>& progress = {});

}  // namespace physics_actions

// The menu bar's Physics menu (docs/PHYSICS.md §5.4): making bodies and colliders, a
// ground plane and a scene, on the current stage and its selection; then Simulate and
// Reset Simulation, which are the timeline's. Its entries' states are worked out each
// time it opens.
class USDCC_USD_UI_API PhysicsMenu : public QMenu {
    Q_OBJECT

public:
    PhysicsMenu(StageManager* stageManager, std::shared_ptr<UndoManager> undo, PlaybackController* playback, QWidget* parent);

    // Fills the menu now, as opening it does.
    void rebuild();

    // Bake Simulation...: asks which bodies and whether to mark them kinematic, then bakes
    // behind a progress dialog that can cancel.
    void bakeInteractively();
    // Check Physics...: the validators' findings and the simulation's reports, in a list
    // whose rows select their prim.
    void checkInteractively();

private:
    StageManager* m_stageManager;
    std::shared_ptr<UndoManager> m_undo;
    PlaybackController* m_playback;
};

}  // namespace usdcc::usd
