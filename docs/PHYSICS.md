# Physics — Plan

Status: **plan, nothing built.** Written 2026-11-16, revised the same day after a review against
OpenUSD's parser source. The choices are in §4, what has to be checked
before building on them in §7, and what was decided and what is still open in §8.

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
| Dependencies | No physics library. vcpkg's pinned baseline has ports for Jolt 5.6.0, PhysX 5.5.0, Bullet 3.25 and MuJoCo 3.10.0 |

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
| `physics_bake.h` | `usdcc_usd`, Qt-free | §4.5. Callers wrap it in an `UndoManager` edit |
| `Stage::physics()` | `usdcc_usd` | The stage's session, made on first use; `physicsChanged` for the cached range and the stale mark |
| `PlaybackController` | `usdcc_usd_ui` | With Simulate on, a tick asks the session for the next frame before setting the time (§4.3) |
| `TimelineBar`, `TimeRuler` | `usdcc_usd_ui` | The Simulate toggle and Reset; the cached range as a band on the ruler, dimmed when stale |
| Physics menu | `usdcc_usd_ui` | §5.4 |
| Collider overlay | `usdcc_usd_ui` | Wireframes of the parsed shapes and joint frames, from `physics_desc`, drawn as the gizmos' `GizmoGeometry` lines. A Display menu entry ([VIEWPORT_DISPLAY.md](VIEWPORT_DISPLAY.md)) |
| `Host::addPhysicsBackend()` | `usdcc_plugin_sdk` | Registers a backend and tells the Plugin Manager |
| `physics_jolt` | `src/plugins` | The Jolt backend |

### 5.2 The backend interface

```cpp
struct PhysicsReport { PXR_NS::SdfPath prim; std::string message; };   // §4.8

class PhysicsWorld {                      // one simulation of one stage
public:
    virtual ~PhysicsWorld() = default;
    virtual void setKinematicPose(BodyId, const Pose&) = 0;      // before a step
    virtual void step(double seconds, int substeps) = 0;
    virtual void readPoses(std::vector<Pose>& out) const = 0;    // dynamic bodies, in BodyId order
    virtual void readVelocities(std::vector<Velocity>& out) const = 0;
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
is stale; nothing is patched in place.

### 5.3 The Jolt mapping

| UsdPhysics | Jolt | Notes |
|---|---|---|
| Sphere, cube, capsule, cylinder | `SphereShape`, `BoxShape`, `CapsuleShape`, `CylinderShape`, in a `ScaledShape` or `RotatedTranslatedShape` as needed | A sphere under non-uniform scale is reported |
| Cone | `TaperedCylinderShape` | |
| Plane | `PlaneShape` | Static or kinematic only, as the schema says |
| Mesh, `convexHull` | `ConvexHullShape` | |
| Mesh, `boundingCube` / `boundingSphere` | `BoxShape` / `SphereShape` from the points' bounds | |
| Mesh, `none` | `MeshShape` | Static and kinematic bodies. On a dynamic body: reported, and a convex hull used instead |
| Mesh, `convexDecomposition`, `meshSimplification` | Phase 5 | Until then: reported, and a convex hull used |
| Several colliders under one body | `StaticCompoundShape` | |
| Material | `Body::SetFriction`, `SetRestitution` | Jolt has one friction per body; UsdPhysics has static and dynamic friction per collider. Dynamic friction is used. A body whose colliders have different materials gets the first collider's, and is reported. The schema's "average" combine rule through `PhysicsSystem::SetCombineFriction` and `SetCombineRestitution` |
| Mass, centre of mass, inertia | `MassProperties` override | From `ComputeMassProperties()` and `physics_mass`, not Jolt's own |
| Kinematic body | `EMotionType::Kinematic`, `MoveKinematic()` each step | |
| Fixed, revolute, prismatic, spherical, distance joints | `FixedConstraint`, `HingeConstraint`, `SliderConstraint`, `SwingTwistConstraint`, `DistanceConstraint` | Break force: phase 4 |
| Generic joint with limits and drives | `SixDOFConstraint` with limits and motors | |
| Collision groups, filtered pairs | A `GroupFilter` of our own | Not `GroupFilterTable`: a collider can be in several UsdPhysics groups, with `mergedGroups` and `invertFilteredGroups`, and Jolt gives a body one group and subgroup. The filter keeps, per body, the set of groups and filtered pairs, and answers each pair from that. Filtering is per body in Jolt, per collider in UsdPhysics: colliders of one body in different groups are reported |
| Articulation root | Ignored | Reported once per articulation: solved as plain joints |
| Scene gravity | `PhysicsSystem::SetGravity` | One scene per world; bodies owned by another scene are left out and reported |

### 5.4 What the user gets

```
Physics
  Make Rigid Body            RigidBodyAPI on the selection, CollisionAPI on its geometry,
                             MeshCollisionAPI = convexHull on meshes
  Make Static Collider       CollisionAPI on the selection's geometry
  Add Ground Plane           a Plane with CollisionAPI at the up axis' zero
  Add Physics Scene
  ──────────────
  Check Physics…             the USD validators and the backend's reports
  Bake Simulation…
  Reset Simulation
```

- Each of the first four is one undo step, and uses the Applied Schemas code that exists.
- Without a `PhysicsScene`, simulation runs with the schema's default gravity and the Log says so.
  Nothing is authored for the user.
- The timeline's Simulate toggle is app-wide and off at start, like auto-key.
- Python: `usdcc.physics.simulate(start, end)`, `reset()`, `bake()`, `cached_range()`,
  `is_stale()`, `reports()`, and a `physics_changed` event.

### 5.5 What gets saved

- **The cache is not saved.** The session file writes the session layer as text
  (`session_stages.cpp`); the cache's sublayer path is left out of it, as an anonymous layer
  can't be found again.
- **The Layer Editor lists the cache** under the session layer as "Simulation cache": it can be
  muted to see the scene as authored, and not made the edit target. Variant edit mode can make
  the session layer itself the edit target (`session_stages.cpp`); that is a different layer from
  the cache, and the rule is enforced in `Stage::setEditTargetLayer()`, not only in the Layer
  Editor's menu, so no path reaches the cache.
- Simulate, the substep count and the collider overlay are settings in `usdcc.ini`.

## 6. Phases

Each phase ships on its own and leaves the app consistent.

| Phase | What | Main files | Tests |
|---|---|---|---|
| **0. Spike** | `joltphysics` in `vcpkg.json` (first confirm the version the pinned baseline has); its imported target mapped to Release in every configuration, as Qt is; a headless test that parses a stage and drops a cube on a plane; the measurements of §7 | `vcpkg.json`, `CMakeLists.txt`, a test | The cube comes to rest on the plane; two runs give the same poses |
| **1. Core** | `physics_desc`, `physics_mass`, the backend interface and registry, `SimCache`, `PhysicsSession`, `Host::addPhysicsBackend()`, the Jolt plugin: rigid bodies, primitives, convex hulls, static meshes, compounds, materials, mass, gravity, kinematic bodies | the new headers, `plugin.h`, `src/plugins/physics_jolt`, `test_data/physics_*.usda` | Units: a stage in centimetres falls as one in metres does. Mass precedence case by case; `physics_mass` against closed-form box and sphere. Restitution and friction. A kinematic body pushes a dynamic one. A body animated only by time samples starts where the start frame puts it. Solving twice gives the same first frame (the cache was cleared before parsing). A body under a rotated, simulated parent lands where it would alone. A body inside an instance is reported, not moved. The cache: range, clear, the user's layers untouched and not dirty, its own writes don't make it stale. A fake backend drives the session |
| **2. Timeline** | Simulate, Reset, the cached band, stale marking, the playback rule of §4.3, the Physics menu's first five entries, settings, session save, the Layer Editor entry, Python | `playback_controller.cpp`, `timeline_bar.cpp`, `time_ruler.cpp`, `session_stages.cpp`, `layer_stack.cpp`, bindings | The controller against a fake clock: frames in order in both modes; an edit makes the cache stale; Play while stale starts from the start frame; scrubbing reads the cache; the session file has no cache path; the cache can't become the edit target, variant edit mode included |
| **3. Bake** | Bake Simulation with its dialog and progress; mark kinematic; world poses converted to local ops, ancestors first | `physics_bake`, a dialog | Baked, with the cache cleared, the stage shows the same transforms at every frame, nested bodies included; no `!resetXformStack!` in the user's layer; one undo step; baking the selection leaves the rest; baking a stale cache solves anew first |
| **4. Constraints** | Joints, limits, drives, break force, collision groups, filtered pairs, the collider overlay, Check Physics | the Jolt plugin, the viewport overlay | A hinge stays within its limits; a drive reaches its target; filtered pairs pass through each other; a broken joint lets go |
| **5. Later** | Convex decomposition (V-HACD or CoACD: licences to read then), point instancers, grabbing a body with the mouse while it plays, a cache on disk as value clips, baking to the prim's own ops, a second backend | — | — |

Phase 0 goes first because §4.2 rests on something not yet measured. Phase 3 depends only on
phase 1, and can move before phase 2 if results in a file matter more than playing them.

## 7. To verify while building

Believed, not tested here.

1. **What Hydra does when a frame's samples arrive.** Adding a time sample to an attribute that
   had one or none may make UsdImaging resync the prim instead of only dirtying its transform,
   which for a mesh is expensive. `SimCache` pre-creates the ops so their *existence* never
   changes while playing; whether the samples themselves cost a resync is phase 0's main
   measurement, at 1,000 bodies. **If they do:** while playing, write each frame as *default*
   values in the cache layer (an info-only change) and keep the samples in a second layer that is
   only composed once playback stops.
2. A session layer's sublayer may hold a stronger `xformOpOrder` than the root layer's, with
   `!resetXformStack!`, and clearing the sublayer brings the prim's own stack back. Storm, the
   Outliner's transform display and the gizmos all honour the reset.
3. Jolt's exported compile definitions reach the plugin through vcpkg's CMake config, and its
   Release library links in a Debug build of usdcc (the release CRT and
   `_ITERATOR_DEBUG_LEVEL=0`, as [CMakeLists.txt](../CMakeLists.txt) sets for everything).
4. The same build on the same machine gives the same poses on every run. Jolt promises
   cross-platform determinism only with a build option; whether to turn it on is a question once
   there are two platforms to compare.
5. `ComputeMassProperties()` with our volume callback matches Jolt's own mass for a box and a
   sphere of uniform density.
6. `MoveKinematic()` from sampled transforms gives a kinematic body the velocity that pushes
   dynamic bodies correctly, with no step of lag at the first frame.
7. Single precision is enough for stages a few kilometres from the origin, solving relative to
   the bodies' common centre. If not: Jolt's double-precision build.
8. vcpkg's pinned baseline has `joltphysics` at 5.6.0 (§2): not read from the baseline itself.
9. A `TaperedCylinderShape` takes a top radius of zero, for a cone. Jolt's capsule and cylinder
   accept the non-uniform scales a `ScaledShape` would give them, or those are reported as the
   sphere's are.

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
| 1 | How many bodies should play at frame rate? | A few thousand; it decides how much §7 item 1 matters |
| 2 | Without a `PhysicsScene`: simulate with defaults, or refuse? | Defaults, and say so in the Log (§5.4) |
| 3 | Should Simulate be per stage or app-wide? | App-wide, off at start, as auto-key |

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
