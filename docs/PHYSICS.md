# Physics — Plan

Status: **phases 0 to 4 built** (2026-11-16): rigid bodies and joints simulate with Jolt, from
the timeline's Simulate, the Physics menu or `usdcc.physics`, into a cache that is never saved;
Bake Simulation keeps a result as time samples; Check Physics and the viewport's Show Colliders
say what the simulation sees. Phase 5's items are what's left. Written 2026-11-16, revised the same day after a
review against OpenUSD's parser source; phase 0's findings are in §7. §2 describes the app as it
was before phase 0. The choices are in §4, what was decided and what is still open in §8.

To try it: open `test_data/physics_drop.usda` or `test_data/physics_playground.usda`, turn on
Simulate (the ball-and-ground button in the timeline, or Physics > Simulate) and play.

usdcc can already *author* physics: the Create menu makes `PhysicsScene`, collision groups and
every joint type ([CREATE_MENU.md](CREATE_MENU.md)), Applied Schemas applies the physics APIs
([APPLIED_SCHEMAS.md](APPLIED_SCHEMAS.md)), and the relationship rules cover `physics:body0/1`,
`filteredGroups` and `simulationOwner` ([RELATIONSHIPS.md](RELATIONSHIPS.md)). Nothing *runs* it:
[ANIMATION.md](ANIMATION.md) §3 lists simulation as not offered. This plan adds basic rigid-body
simulation: a solver, a place for its results, and a way to turn them into animation.

## 1. What OpenUSD offers

Checked against the build the app links (`USD_INSTALL`, which reports 26.08).

| Piece | What it is |
|---|---|
| **`usdPhysics` schemas** | `PhysicsScene` (gravity), `RigidBodyAPI` (velocities, kinematic, starts asleep), `CollisionAPI`, `MeshCollisionAPI` (approximation: none, convex hull, convex decomposition, simplified mesh, bounding cube, bounding sphere), `MassAPI`, `MaterialAPI` (friction, restitution, density), `CollisionGroup`, `FilteredPairsAPI`, the joints (fixed, revolute, prismatic, spherical, distance, and the generic one with `LimitAPI` and `DriveAPI` per degree of freedom), `ArticulationRootAPI` |
| **Parsing** (`usdPhysics/parseUtils.h`, `parseDesc.h`) | `UsdPhysicsLoadStageFromPrimRange()` walks a stage and reports plain descriptors: scenes, materials, collision groups, shapes (sphere, cube, capsule, cylinder, cone, plane, mesh, sphere points), rigid bodies, joints with limits and drives, articulations. It resolves the rules a solver would otherwise have to re-implement: which collider belongs to which body, nested bodies, simulation owners, filtering. Its output is deterministic. It reads every value at `UsdTimeCode::Default()`, never at a frame (§4.3) |
| **Mass** | `UsdPhysicsRigidBodyAPI::ComputeMassProperties()`: the precedence rules (explicit mass, then density on the mass API, then the material's density, then 1000 kg/m³) with a callback that supplies each collider's volume and inertia |
| **Units** | Distance in the stage's `metersPerUnit`, mass in `kilogramsPerUnit` (`metrics.h`), time in seconds, angles in degrees. Gravity defaults to the up axis and Earth's magnitude |
| **Validators** | `usdPhysicsValidators` is in the build: rules for bodies, colliders and articulations |

Two things the schema does **not** say, which §4 has to decide:

- **Where results go.** Only that they are written back in the stage's units. Nothing about which
  layer, or whether as defaults or time samples.
- **Anything beyond rigid bodies.** No cloth, soft bodies, fluids or vehicles. A deformables
  proposal is in the AOUSD physics working group; nothing here depends on it.

## 2. What there is today

| Where | State |
|---|---|
| Create menu, Applied Schemas, relationships, icons | Physics prims and APIs can be made, applied, removed and connected |
| Attribute Editor | Shows physics attributes in "Physics …" sections like any other schema |
| `Stage` ([stage.h](../src/cpp/usd/include/usdcc/usd/stage.h)) | Has a time and a playback range per stage. No simulation state |
| `PlaybackController` | One clock on the UI thread, on the current stage, Every Frame or Real Time ([ANIMATION.md](ANIMATION.md) §4.7) |
| Session layer | Used by variant edit mode, saved as text with the app session (`session_stages.cpp`), listed by the Layer Editor |
| Undo | Records the edit target's layer only ([undo_manager.h](../src/cpp/usd/include/usdcc/usd/undo_manager.h)). Edits to other layers are not steps |
| Plugins | The C++ SDK loads libraries that share the app's classes ([PLUGIN_SDK.md](PLUGIN_SDK.md)). `Host` has no notion of a solver |
| Dependencies | Jolt 5.6.0 from vcpkg since phase 0, linked only by the headless tests so far. With the port's `rtti` feature: the backend subclasses `JPH::GroupFilter`, and on Linux GCC finds that class's typeinfo only in a Jolt built with RTTI. vcpkg's pinned baseline also has PhysX 5.5.0, Bullet 3.25 and MuJoCo 3.10.0 |

## 3. Engines

Licences as read on 2026-11-16; this is not legal advice. "Standard artifact" is the BSD-3-Clause
distribution of [RELEASE.md](RELEASE.md) §6.

| Engine | Licence | In the standard artifact? | Fit |
|---|---|---|---|
| **Jolt Physics 5.6.0** | MIT | Yes: the licence text, which `tools/deploy.py` already gathers for vcpkg libraries | C++17, the standard library its only dependency, no RTTI or exceptions. Has every UsdPhysics shape and joint type, with motors for drives. Deterministic, multi-core. Windows, Linux, macOS. No reduced-coordinate articulations |
| **PhysX SDK 5.5** (vcpkg; upstream is further on) | BSD-3-Clause in the repository's `LICENSE.md`; its README now also names Apache-2.0 for the source. Permissive either way; read it again when pinning | Yes | The reference implementation of UsdPhysics, with real articulations. A heavier build. GPU dynamics need CUDA and an NVIDIA GPU. The USD glue is ours to write: the SDK itself knows nothing of USD |
| **ovphysx** (PhysX with its own USD reader) | Source Apache-2.0. Prebuilt SDKs and wheels under the NVIDIA Omniverse License | No, for the reasons ovrtx isn't ([RTX_RENDER.md](RTX_RENDER.md), "ship the plugin, not the runtime") | Reads USD itself, `PhysxSchema` included, through ovstage, which has to match exactly and carries its own OpenUSD. Results come back as tensors, never written to USD. Pre-release, the API still changing. CPU works, AVX required |
| **MuJoCo 3.10** | Apache-2.0 | Yes | A C library with robotics-grade articulations and a soft contact model. Its OpenUSD support is experimental and needs MuJoCo built against our OpenUSD. A poor match for props falling on a floor |
| **Newton** | Apache-2.0 (on NVIDIA Warp) | Yes, but large | Python. Solvers for rigid bodies (MuJoCo Warp), cloth, soft bodies and granular material; reads UsdPhysics with its own resolver. Could be a Python plugin in the embedded interpreter. Not checked: whether it installs its own `usd-core`, which must not meet the app's `pxr` in one process |
| **Bullet 3.25** | zlib | Yes | Mature and barely maintained. Nothing it does better than Jolt here |
| **Box3D 0.1** | MIT | Yes | From Box2D's author, released June 2026, alpha. Worth another look in a year |
| **Havok** | Commercial: about $50,000 per title under the 2025 terms | No | Licensed per game title, not redistributable in an open-source app |
| **Chaos** | Unreal Engine EULA (from memory, not read again) | No | Not usable outside Unreal |

Also in vcpkg and not looked at further: ODE (BSD-3 or LGPL), ReactPhysics3D (zlib).

**Jolt is the one that is permissive, small, already in vcpkg, and complete for what UsdPhysics
describes.** No UsdPhysics-to-Jolt bridge was found to reuse, so the mapping is ours to write;
OpenUSD's parser (§1) makes that a table of descriptor to shape, not a traversal.

PhysX is where to go for content authored for Omniverse or Isaac (`PhysxSchema`) or for robot
articulations. It fits behind the same interface (§5.2), as its own plugin.

## 4. Directions

### 4.1 The solver sits behind an interface, in a plugin

**The app owns everything about USD; a backend owns only the solving.**

- The app parses the stage into plain, unit-normalised descriptors, hands them to a backend,
  asks it to step, and reads poses back. No backend touches a `UsdStage`.
- So a second backend costs a mapping, tests run against a fake one, and a solver's types never
  appear in usdcc's headers.
- The Jolt backend is a bundled C++ plugin in `src/plugins/physics_jolt`, built and enabled in
  every build like the mesh tools. Its licence fits the standard artifact, so unlike those it
  needs no second distribution.

### 4.2 Results are a cache in the session layer, not edits

**Stepping writes time samples into an anonymous sublayer of the session layer. Nothing reaches
the user's layers until they bake.**

| | Cache in a session sublayer | Authoring into the edit target |
|---|---|---|
| The user's files | Untouched | Filled with solver output on every Play |
| Undo | Nothing recorded: the undo manager only watches the edit target | A step per run, or per frame |
| Scrubbing back | Plain USD playback of the samples | The same |
| Throwing a run away | Clear one layer | Undo, if nothing else happened since |
| Seeing the scene un-simulated | Mute the layer in the Layer Editor | Not possible |

A simulation is tried many times before it is kept. Each try must cost nothing and leave nothing.

### 4.3 Simulation is driven by the timeline

**No second clock. With Simulate on, playing forward past the cached frames steps the solver;
inside them it is ordinary playback.**

- It starts from the stage as authored at the **simulation start frame**: the playback range's
  first frame. OpenUSD's parser reads everything at `UsdTimeCode::Default()` (every
  `ComputeLocalToWorldTransform` and `Get` in `parseUtils.cpp`), so `physics_desc` reads bodies'
  and shapes' world transforms again at the start frame with a `UsdGeomXformCache`, and mesh
  points there too. Otherwise an animated body, or one with only time samples, starts from its
  default value or from identity.
- Frames are solved in order, never skipped. In Real Time mode playback waits for the solver
  instead of jumping over frames it hasn't made.
- Going back, or scrubbing inside the cached range, reads the cache. Scrubbing past its end
  stays on the last cached frame.
- One frame is `1 / timeCodesPerSecond` seconds, cut into substeps (a setting; 4 to start with).

This is the "global tool acts on the current stage" rule of [CURRENT_STAGE.md](CURRENT_STAGE.md):
the cache and the solver's world belong to the stage, the transport to the app.

### 4.4 An edit outside the cache makes it stale

Any `Stage::changed` that isn't the cache layer's own marks the cache stale: the ruler shows it,
and the next Play solves anew. The cache isn't cleared at once, so a look at an attribute doesn't
cost the run. **Reset** clears it and returns to the start frame.

- **Telling the cache's own writes apart.** `StageChanges` is built from
  `UsdNotice::ObjectsChanged`, which doesn't say which layer changed. `SimCache` sets a guard on
  the `PhysicsSession` around each frame's `SdfChangeBlock`, and changes that arrive under it
  don't mark the cache stale. Muting or unmuting the cache layer is the session's own act too.
- **Solving anew clears the cache first.** The parser must see the stage as authored: with the
  cache composed, its `xformOpOrder` points at `:sim` ops that have only time samples, and at
  `Default()` those read as identity, so every body would start at the origin. The cache is
  cleared (not muted, which would recompose the stage twice) before `physics_desc` runs.
- **Play while stale** solves from the start frame, whatever the current time: the timeline
  jumps there first. A stale cache is never extended, as its frames no longer follow from the
  stage.

Selection, time and view state don't touch it: they aren't stage changes.

### 4.5 Keeping a result means baking it

**Bake Simulation copies the cache's samples into the edit target, as one undo step.**

- What it writes are plain time samples on transform ops, which every reader understands: Storm,
  RTX, V-Ray, other DCCs, usdz. This is the choice [ANIMATION.md](ANIMATION.md) §4.4 made for keys.
- All bodies or the selected ones, over the cached range.
- With the cache stale, Bake solves anew from the start frame first (§4.7), and its dialog says
  so. It never bakes frames that no longer follow from the stage.
- An option marks baked bodies `physics:kinematicEnabled`, so a later run treats them as moving
  colliders instead of solving them again on top of their own animation.
- Baking goes through `anim_edit` once [ANIMATION.md](ANIMATION.md) phase 2 exists, so a weaker
  layer's animation is handled by the same rule as everywhere else.

### 4.6 How a pose is written

A body's pose from the solver is a world transform, and **the cache writes it as one**. In the
cache layer the prim gets:

- `xformOpOrder = [!resetXformStack!, xformOp:translate:sim, xformOp:orient:sim,
  xformOp:scale:sim]`, with the scale constant (the body's world scale at the start frame) and the
  other two time-sampled. USD interpolates the orientation by slerp, so sub-frame reads (motion
  blur) stay rigid.
- `xformOp:transform:sim`, one matrix, in place of the three when the world scale has shear
  (a rotated parent with non-uniform scale above the body).

Why world space, not the parent's space:

- UsdPhysics simulates a body under another body on its own. A local transform would need the
  parent's *simulated* pose at every frame, so frames would have to be written parents first.
- An animated parent would have to be evaluated at every frame, only to be divided out again.
- The prim's children still inherit from it; `!resetXformStack!` only cuts the body off from its
  ancestors, which is what the solver already did.

The stronger `xformOpOrder` hides the prim's own stack without changing it. **Baking converts**:
it writes local `:sim` ops relative to the parent as composed after the bake, ancestors first, so
the user's layer doesn't gain a `!resetXformStack!` it didn't have.
`XformStack::setLocalTransform()` already chooses between three ops and one matrix. Converting to
the prim's own translate/rotate/scale ops is a later refinement.

### 4.7 The first version solves on the UI thread

Jolt runs its own worker threads inside a step, so the UI thread only waits for a step and then
authors the poses. Hydra is driven from the UI thread everywhere in this app
([ANIMATION.md](ANIMATION.md) §4.7), and a solver thread that authored USD would have to fight
that. Bake runs the remaining frames the same way, behind a progress dialog that can cancel.

### 4.8 What can't be simulated is said, not skipped silently

A backend reports what it didn't take: an approximation it doesn't have, a dynamic body with a
triangle-mesh collider, an articulation it solves as plain joints. The reports go to the Log and
to **Check Physics**, with the prim's path. This is the mesh tools' rule: reject or report, never
relax quietly ([BLENDER_MESH_TOOLS.md](BLENDER_MESH_TOOLS.md) §2).

The app reports what it can't write back, too. **A body inside an instance** is parsed (the parser
walks instance proxies) but its pose can't be written: opinions at an instance proxy's path are
ignored by composition. Phase 1 leaves such bodies out of the world and reports each instance once.
Later, the cache layer can set `instanceable = false` on the instance's root, which makes the
bodies ordinary prims for as long as the cache exists and leaves the user's layers as they were.

## 5. Design

### 5.1 New pieces

| Piece | Target | What it does |
|---|---|---|
| `physics_desc.h` | `usdcc_usd`, Qt-free | Calls `UsdPhysicsLoadStageFromPrimRange()` and turns its descriptors into owned structs in metres, kilograms and seconds: world poses and mesh points read again at the start frame (§4.3), shape geometry (points and indices copied), mass properties from `ComputeMassProperties()`, materials, joints in body-local frames, filtering. Leaves out and reports bodies inside instances (§4.8) |
| `physics_mass.h` | `usdcc_usd`, Qt-free | The `MassInformationFn` that `ComputeMassProperties()` calls for each collider: volume, inertia tensor and centre of mass for the primitives in closed form, and for a convex hull or closed mesh by summing signed tetrahedra. No solver is linked here to borrow this from |
| `physics_backend.h` | `usdcc_usd`, Qt-free | §5.2, and the registry of backends |
| `SimCache` | `usdcc_usd`, Qt-free | The cache layer: makes and removes it, pre-creates the world-space ops for every body (§4.6), writes a frame's poses in one `SdfChangeBlock` under the session's guard (§4.4), knows the cached range, clears |
| `PhysicsSession` | `usdcc_usd`, Qt-free | One per stage that has simulated: the backend's world, the cache, the start frame, stale or not. `start()` clears the cache and then parses (§4.4), `stepTo(frame)`, `reset()`. Reads kinematic bodies' animated transforms and hands them to the backend before each step. Ignores the stage changes its own writes cause |
| `physics_bake.h` | `usdcc_usd`, Qt-free | §4.5: `cacheRange()` fills the cache over a range, `bakeFromCache()` writes it. Callers wrap it in an `UndoManager` edit and clear the cache |
| `physics_check.h` | `usdcc_usd`, Qt-free | Check Physics: OpenUSD's `usdPhysicsValidators` through a `UsdValidationContext`, then the parsing's and the backend's reports for a world built and never stepped |
| `Stage::physics()` | `usdcc_usd` | The stage's session, made on first use; `PhysicsSession::changed` for the cached range, the stale mark and the reports. `Stage::setEditTargetLayer()` refuses the cache |
| `PlaybackController` | `usdcc_usd_ui` | With Simulate on, a tick asks the session for the next frame before setting the time (§4.3) |
| `TimelineBar`, `TimeRuler` | `usdcc_usd_ui` | The Simulate toggle and Reset; the cached range as a band on the ruler, dimmed when stale |
| `PhysicsMenu`, `physics_actions` (`physics_menu.h`) | `usdcc_usd_ui` | §5.4 |
| Collider overlay (`physics_overlay.h`) | `usdcc_usd_ui` | Wireframes of the parsed shapes and joint frames, from `physics_desc`, drawn as the gizmos' `GizmoGeometry` lines; rebuilt when the stage or the time changes. Show Colliders in a viewport's options menu and Display dialog, with a Settings default (`Viewport/showColliders`, off) ([VIEWPORT_DISPLAY.md](VIEWPORT_DISPLAY.md)) |
| `Host::addPhysicsBackend()` | `usdcc_plugin_sdk` | Registers a backend and tells the Plugin Manager |
| `physics_jolt` | `src/plugins` | The Jolt backend: `usdcc_physics_jolt_backend`, a static library the tests link too, and the plugin that registers it. Jolt's library and licence go in the plugin's folder; `tools/deploy.py` ships it in `plugins/` |

### 5.2 The backend interface

As built ([physics_backend.h](../src/cpp/usd/include/usdcc/usd/physics_backend.h)):

```cpp
struct PhysicsReport { PXR_NS::SdfPath prim; std::string message; };   // §4.8

class PhysicsWorld {                      // one simulation of one stage
public:
    virtual ~PhysicsWorld() = default;
    // Where a kinematic body has to be at the end of the next step.
    virtual void setKinematicTarget(BodyId, const Pose&, double seconds) = 0;
    virtual void step(double seconds, int substeps) = 0;
    virtual void readPoses(std::vector<Pose>& out) const = 0;    // every body, in BodyId order
    // A joint that broke in the steps since the last call, and the like (§4.8).
    virtual void takeEvents(std::vector<PhysicsReport>& out) {}
};

class PhysicsBackend {
public:
    virtual ~PhysicsBackend() = default;
    virtual std::string id() const = 0;       // "org.usdcc.physics-jolt"
    virtual std::string name() const = 0;     // "Jolt"
    virtual std::unique_ptr<PhysicsWorld> build(const PhysicsSceneDesc&, std::vector<PhysicsReport>& reports) = 0;
};
```

Plain structs in, plain arrays out. A world is rebuilt from the start frame whenever the cache
is stale; nothing is patched in place. `PhysicsBackends` holds the backends the app has; the first
one added is used. Velocities aren't read back yet: nothing needs them before baking.

### 5.3 The Jolt mapping

| UsdPhysics | Jolt | Notes |
|---|---|---|
| Sphere, cube, capsule, cylinder | `SphereShape`, `BoxShape`, `CapsuleShape`, `CylinderShape` | Sizes come from the parser with the world scale in them. A collider off its body's origin, or turned to its axis, goes in a `StaticCompoundShape` |
| Cone | `TaperedCylinderShape` with a top radius of zero | Its apex up its axis, as `UsdGeomCone` has it |
| Capsule or cylinder with two radii | The larger one | Reported |
| Plane | `PlaneShape` | Static or kinematic only, as the schema says |
| Mesh, `convexHull` | `ConvexHullShape` | |
| Mesh, `boundingCube` / `boundingSphere` | `BoxShape` / `SphereShape` from the points' bounds | |
| Mesh, `none` | `MeshShape` | Static and kinematic bodies. On a dynamic body: reported, and a convex hull used instead |
| Mesh, `convexDecomposition`, `meshSimplification` | Phase 5 | Until then: reported, and a convex hull used |
| Several colliders under one body | `StaticCompoundShape` | |
| A body without a collider | `EmptyShape` | Reported: it falls, and hits nothing |
| Material | `Body::SetFriction`, `SetRestitution` | Jolt has one friction per body; UsdPhysics has static and dynamic friction per collider. Dynamic friction is used. A body whose colliders have different materials gets the first collider's, and is reported. The schema's "average" combine rule through `PhysicsSystem::SetCombineFriction` and `SetCombineRestitution` |
| Mass, centre of mass, inertia | `MassProperties` override, and an `OffsetCenterOfMassShape` where UsdPhysics' centre of mass isn't the shapes' | From `ComputeMassProperties()` and `physics_mass`, not Jolt's own |
| Kinematic body | `EMotionType::Kinematic`, `MoveKinematic()` each step | |
| Fixed, revolute, prismatic, spherical, distance joints | `FixedConstraint`, `HingeConstraint`, `SliderConstraint`, `SwingTwistConstraint`, `DistanceConstraint`, in world space from the start frame's poses | A joint to nothing, or to a static collider, is to the world. A revolute or prismatic limit that excludes where the joint starts is widened to include it (Jolt's limits keep 0 inside), and reported. A spherical joint's two cone angles go to the swing-twist cone; its twist is free |
| Drives | The constraint's motor: a position spring with the drive's stiffness and damping (as a frequency and damping ratio for an acceleration drive), or a velocity motor without stiffness; the force limit as the motor's | Angular stiffness and damping per degree in UsdPhysics, per radian in Jolt; forces and torques in the stage's units, converted |
| Break force and torque | Checked after each step from the impulses the constraint applied in the last substep; over either, the constraint is disabled and its bodies woken | Reported, with the frame |
| Generic joint with limits and drives | `SixDOFConstraint` with limits and motors; a DOF without a limit is free, lower > upper locks it | Swing limits as a pyramid. A distance limit on a generic joint is reported, not simulated |
| A joint's collisions | `collisionEnabled` false (the default) filters the two bodies' pair | |
| Collision groups, filtered pairs | A `GroupFilter` of our own | Not `GroupFilterTable`: a collider can be in several UsdPhysics groups, with `mergedGroups` and `invertFilteredGroups`, and Jolt gives a body one group and subgroup. `physics_desc` works out, solver-neutrally, each owner's groups (a body, or a static collider), a group-by-group table, and the filtered pairs; the filter answers each pair from that. Two owners don't touch when any group of one filters any group of the other. Filtering is per body in Jolt, per collider in UsdPhysics: colliders of one body in different groups are reported |
| Articulation root | Ignored | Reported once per articulation: solved as plain joints |
| Scene gravity | `PhysicsSystem::SetGravity` | One scene per world; bodies owned by another scene are left out and reported |

### 5.4 What the user gets

```
Physics
  Make Rigid Body            RigidBodyAPI on the selection, CollisionAPI on its geometry,
                             MeshCollisionAPI = convexHull on meshes
  Make Static Collider       CollisionAPI on the selection's geometry
  Add Ground Plane           a Plane with CollisionAPI at the up axis' zero, 20 m across
  Add Physics Scene
  ──────────────
  Check Physics…             the USD validators and the simulation's reports
  ──────────────
  Simulate                   the timeline's toggle
  Reset Simulation
  Bake Simulation…           all bodies or the selection's; mark them kinematic
```

- Each of the first four is one undo step: UsdPhysics' own `Apply()` inside one `UndoManager`
  edit, as `prim_actions::applySchema()` would make one step per schema. Prims inside instances
  are left alone.
- Without a `PhysicsScene`, simulation runs with Earth's gravity down the up axis and the Log says
  so. Nothing is authored for the user.
- The timeline has a Simulate button beside Loop, and Simulate and Reset Simulation in its menu.
  The ruler shows the cached frames as a band along its bottom, dimmed when stale; the button's
  tooltip says which frames.
- Simulate is app-wide and off at every start, like auto-key. Turning it on while playing starts
  a simulation at once.
- **Check Physics…** lists, in a dialog whose rows select their prim on a double-click, what
  OpenUSD's physics validators say (a negative mass, a joint to a prim that isn't xformable) and
  what a simulation would leave out or change, from a world built at the current time.
- **Bake Simulation…** asks for all bodies or those at or under the selection, and whether to
  mark them kinematic; solves the playback range where the cache lacks frames (anew if it's
  stale) behind a progress dialog that cancels with nothing written; writes the frames as one
  undo step and clears the cache, so the stage then shows the bake. A body whose
  `xformOpOrder` a stronger layer than the edit target sets is reported: the bake wouldn't show.
- Python: `usdcc.physics.simulate(start, end)`, `reset()`, `cached_range()`, `is_stale()`,
  `reports()`, `bake(prims, mark_kinematic)`, `check()`; `usdcc.app.is_simulating()`,
  `set_simulating()`; and a `physics_changed` event. A bake from a script is part of the
  script run's undo step.

### 5.5 What gets saved

- **The cache is not saved.** The session file writes the session layer as text
  (`session_stages.cpp`); the cache's sublayer path is left out of it, as an anonymous layer
  can't be found again.
- **The Layer Editor lists the cache** under the session layer as "Simulation cache": it can be
  muted to see the scene as authored, and not made the edit target. Variant edit mode can make
  the session layer itself the edit target (`session_stages.cpp`); that is a different layer from
  the cache, and the rule is enforced in `Stage::setEditTargetLayer()`, not only in the Layer
  Editor's menu, so no path reaches the cache.
- **Settings:** the substep count (Settings > Physics, `Physics/substeps`, 4) is in
  `usdcc.ini`. Simulate isn't saved: it is off at every start. Show Colliders is a viewport
  display switch: a Settings default, and each viewport's own choice saved with the session.

## 6. Phases

Each phase ships on its own and leaves the app consistent.

| Phase | What | Main files | Tests |
|---|---|---|---|
| **0. Spike** (done) | `joltphysics` in `vcpkg.json`; its imported target mapped to Release in every configuration, as Qt is, with the two Debug fixes of §7 item 3; a headless test that parses a stage and drops a cube on a plane; the measurements of §7 | `vcpkg.json`, `CMakeLists.txt`, `tests/physics_spike_test.cpp` | The cube comes to rest on the plane; two runs give the same poses. Both pass, and so do the §7 checks |
| **1. Core** (done) | `physics_desc`, `physics_mass`, the backend interface and registry, `SimCache`, `PhysicsSession`, `Host::addPhysicsBackend()`, the Jolt plugin: rigid bodies, primitives, convex hulls, static meshes, compounds, materials, mass, gravity, kinematic bodies | the new headers, `plugin.h`, `src/plugins/physics_jolt`, `test_data/physics_drop.usda`, `tests/physics_test.cpp` | Units: a stage in centimetres falls as one in metres does. Mass precedence case by case; `physics_mass` against closed-form box and sphere. Restitution and friction. A kinematic body pushes a dynamic one. A body animated only by time samples starts where the start frame puts it. Solving twice gives the same first frame (the cache was cleared before parsing). A body under a rotated, simulated parent lands where it would alone. A body inside an instance is reported, not moved. The cache: range, clear, the user's layers untouched and not dirty, its own writes don't make it stale. A fake backend drives the session |
| **2. Timeline** (done) | Simulate, Reset, the cached band, stale marking, the playback rule of §4.3, the Physics menu's first four entries with Simulate and Reset, settings, session save, the Layer Editor entry, Python | `playback_controller.cpp`, `timeline_bar.cpp`, `time_ruler.cpp`, `physics_menu.cpp`, `session_stages.cpp`, `layer_stack.cpp`, bindings, `tests/physics_timeline_test.cpp` | The controller against a fake clock: frames in order in both modes; an edit makes the cache stale; Play while stale starts from the start frame; scrubbing reads the cache; the session file has no cache path; the cache can't become the edit target, variant edit mode included |
| **3. Bake** (done) | Bake Simulation with its dialog and progress; mark kinematic; world poses converted to local ops, ancestors first | `physics_bake`, `physics_menu.cpp`, `tests/physics_bake_joints_test.cpp` | Baked, with the cache cleared, the stage shows the same transforms at every frame, nested bodies included; no `!resetXformStack!` in the user's layer; one undo step; baking the selection leaves the rest; baking a stale cache solves anew first |
| **4. Constraints** (done) | Joints, limits, drives, break force, collision groups, filtered pairs, the collider overlay, Check Physics | `physics_desc`, the Jolt plugin, `physics_check`, `physics_overlay`, `tests/physics_bake_joints_test.cpp` | A hinge stays within its limits; a drive reaches its target; filtered pairs pass through each other; a broken joint lets go |
| **5. Later** | Convex decomposition (V-HACD or CoACD: licences to read then), point instancers, grabbing a body with the mouse while it plays, a cache on disk as value clips, baking to the prim's own ops, a second backend | — | — |

Phase 0 went first because §4.2 rested on something not yet measured (§7 item 1: it holds).
Phase 3 depends only on phase 1, and can move before phase 2 if results in a file matter more
than playing them.

`tests/physics_spike_test.cpp` drives Jolt directly, with just enough of a UsdPhysics mapping for
cubes, spheres and planes; it stays, for §7's measurements. `tests/physics_test.cpp` checks
phase 1 (with a fake backend and with Jolt), `tests/physics_timeline_test.cpp` phase 2, and
`python_physics` in `tests/python_app_test.cpp` the Python API.

`tests/physics_bake_joints_test.cpp` checks phases 3 and 4. `test_data/physics_playground.usda`
has a group for each case (bounce, a knocked-over stack, every shape, friction on a ramp, a
kinematic pusher, a static triangle mesh, mass, a pendulum on a hinge); simulated in the app, each
behaves as its description says.

Phases 1 to 4 as built, where they differ from the plan above:

- The cache is the session layer's *first* sublayer, so the session layer itself (where variant
  edit mode writes) stays stronger than it.
- In Real Time, playback that has run ahead of the solver shows the next solved frame and counts
  on from there, so no frame is skipped (§4.3).
- Muting or unmuting the cache doesn't make it stale: the session tells a mute from an edit by
  the layer's muted state.
- Static colliders inside an instance stay: they never move, so nothing has to be written for
  them. Only moving bodies inside instances are left out (§4.8).
- Baking reads every frame's local transforms with the cache still composed, then writes: a
  parent that is itself a body is where the cache has it, which is where it is once baked too,
  so no ancestors-first order is needed. Each body gets `[translate:sim, orient:sim, scale:sim]`
  in the edit target, or one `transform:sim` matrix where its local transform has shear; its
  own ops stay, unused.
- `anim_edit` ([ANIMATION.md](ANIMATION.md) phase 2) doesn't exist yet, so a bake writes to the
  edit target directly and reports when a stronger layer hides it, rather than following
  `anim_edit`'s rule for a weaker layer's animation.
- Articulations are simulated as plain joints between their bodies, and each is reported once.

## 7. Checked in phase 0

What the plan believed, and what `tests/physics_spike_test.cpp` found (2026-11-16, Release build,
Windows, one machine). Items still open are marked.

1. **Hydra and a frame's samples: no resync.** For 1,000 mesh bodies whose cache ops exist,
   writing one more sample per body (in one `SdfChangeBlock`) gives UsdImaging's scene index 1,000
   dirtied prims with only the transform locator, and none removed or added. Creating the ops and
   the `xformOpOrder` in the first place doesn't resync either. Writing defaults instead gives the
   same notices, so §7's fallback (defaults while playing, samples in a second layer) isn't
   needed. Measured on the scene indices the viewport's engine uses by default in 26.08
   (`USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX`). A frame cost about 1.6 ms to write and 6 ms to sync
   and pull every transform. **Open:** Storm's own cost per dirtied transform, in the app.
2. **The cache layer composes as planned.** An anonymous sublayer of the session layer holding
   `[!resetXformStack!, translate:sim, orient:sim, scale:sim]` puts a body exactly at the solver's
   world pose under a rotated, non-uniformly scaled parent, at every sampled frame. The user's own
   `xformOpOrder` is untouched, and `SdfLayer::Clear()` on the cache brings the prim's own stack
   back. Parsing through the cache puts the body at the origin, as §4.4 said. **Open:** the
   Outliner's transform display and the gizmos with `!resetXformStack!` (Storm gets it through the
   scene index, which honours it).
3. **Jolt in a Debug build needs two fixes**, now in [CMakeLists.txt](../CMakeLists.txt):
   - Jolt's target adds `NDEBUG` for `$<CONFIG:Release,...>`. With Debug mapped to Release that
     matches a Debug build too, so `assert()` would be off in everything that links Jolt. The
     definition is dropped.
   - Without `NDEBUG`, Jolt's headers switch its asserts on, which changes its classes' layout
     (`Mutex` 80 to 88 bytes, `PhysicsSystem` 1,344 to 1,440) from the Release DLL's. Jolt's own
     check, `VerifyJoltVersionID()`, still returns true: it is computed before the asserts are
     switched on. `JPH_NO_DEBUG` keeps the headers as the DLL was built.

   Found with a separate two-file CMake project set up as usdcc is (release CRT,
   `_ITERATOR_DEBUG_LEVEL=0`, Debug mapped to Release). The spike checks that the asserts are off.
   **Open:** a Debug build of usdcc itself.
4. **Deterministic on one machine.** 200 cubes falling onto each other, with Jolt's worker
   threads, give bit-identical poses at every frame of two runs. Cross-platform determinism needs
   Jolt's `CROSS_PLATFORM_DETERMINISTIC`, which vcpkg's port turns off: a question once there are
   two platforms to compare.
5. **Mass matches.** `ComputeMassProperties()` with the spike's volume callback gives Jolt's own
   mass and inertia for a non-uniformly scaled box with an explicit density and for a sphere at the
   schema's default density, to 1e-4. The callback returns inertia at unit density;
   `ComputeMassProperties()` scales it.
6. **Kinematic bodies have no lag.** After one `MoveKinematic()` and one step, a kinematic body is
   at its animated position and moved at the animation's speed during that first frame. It pushes
   a dynamic box ahead of it.
7. **Far from the origin: recentre.** At 5 km, a cube at rest lands 0.02 mm from where it lands at
   the origin. A falling stack of 30 ends up to 16 cm apart, because a stack falling over is
   chaotic and half a millimetre of rounding sends it down another way. Solved relative to a centre
   near the bodies, both land exactly as at the origin. So `PhysicsSession` solves relative to the
   bodies' centre at the start frame, and Jolt's double-precision build isn't needed. The parser's
   own float positions are off by up to 0.2 mm at 5 km, one more reason `physics_desc` reads world
   transforms itself, in double (§4.3).
8. **Jolt 5.6.0** (port version 1) is what the pinned baseline has.
9. **Shapes.** A `TaperedCylinderShape` with a top radius of zero is a cone, with or without a
   convex radius. A capsule takes only uniform scale; a cylinder takes scales whose two radial
   components are equal. Other scales are reported, as a sphere's are (§5.3).

Also measured, for §8 question 1: columns of ten 1 m cubes falling onto a plane cost Jolt about
1.1 ms per frame on average for 1,000 bodies and 2.1 ms for 3,000, at four substeps (worst frame
3 ms). Writing the cache and syncing it (item 1) cost more than solving at those counts.

## 8. Open questions

Decided on 2026-11-16:

| Question | Decision |
|---|---|
| The backend as a bundled plugin, or compiled into the app? | **A bundled plugin** (§4.1) |
| A cache with an explicit bake, or results authored directly? | **A cache with an explicit bake** (§4.2, §4.5) |
| Is content authored for Omniverse or Isaac needed? | **No.** Jolt is the first backend; PhysX and `PhysxSchema` stay out (§9) |

Still open:

| # | Question | Recommended |
|---|---|---|
| 1 | How many bodies should play at frame rate? | A few thousand. Phase 0 (§7) found the solver and the cache cheap at 3,000; Storm's share is still to measure |
| 2 | Without a `PhysicsScene`: simulate with defaults, or refuse? | Built as recommended: Earth's gravity, said in the Log (§5.4) |
| 3 | Should Simulate be per stage or app-wide? | Built as recommended: app-wide, off at every start |

## 9. Out of scope

- **Cloth, soft bodies, fluids, particles, destruction, vehicles.** UsdPhysics doesn't describe
  them yet. Newton is the engine to look at when it does (§3).
- **`PhysxSchema` and other solver-specific schemas.** They are read by the backend that knows
  them; a PhysX plugin, not this plan.
- **Reduced-coordinate articulations**, and so robots that have to behave as their controllers
  expect.
- **A GPU solver**, a solver thread, a simulation that keeps running while the timeline is
  stopped.
- **Editing while it plays**, beyond the mouse grab of phase 5: an edit makes the cache stale
  (§4.4).

## 10. References

- [UsdPhysics overview](https://openusd.org/dev/api/usd_physics_page_front.html),
  [parseUtils.h](https://openusd.org/dev/api/parse_utils_8h_source.html)
- [Jolt Physics](https://github.com/jrouwe/JoltPhysics),
  [5.6.0](https://newreleases.io/project/github/jrouwe/JoltPhysics/release/v5.6.0)
- [NVIDIA PhysX](https://github.com/NVIDIA-Omniverse/PhysX), its
  [LICENSE.md](https://github.com/NVIDIA-Omniverse/PhysX/blob/main/LICENSE.md),
  [ovphysx](https://github.com/NVIDIA-Omniverse/PhysX/blob/main/ovphysx/README.md) and its
  [overview](https://nvidia-omniverse.github.io/PhysX/ovphysx/latest/ovphysx_overview.html)
- [Newton](https://github.com/newton-physics/newton) and
  [its USD parsing](https://newton-physics.github.io/newton/concepts/usd_parsing.html)
- [MuJoCo's OpenUSD support](https://mujoco.readthedocs.io/en/stable/OpenUSD/mjcPhysics.html)
- [Box3D](https://github.com/erincatto/box3d)
- [Havok's pricing](https://www.havok.com/blog/pricing-update/)
