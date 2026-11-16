#pragma once

// A stage's UsdPhysics content as plain, owned data in metres, kilograms and seconds
// (docs/PHYSICS.md §5.1): what a physics backend is handed. OpenUSD's parser
// (UsdPhysicsLoadStageFromPrimRange) decides which collider belongs to which body; this
// reads every pose again at the simulation's start frame, in double, since the parser
// reads at Default() and keeps floats (§4.3, §7 item 7).
//
// Qt-free, solver-free.

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/timeCode.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::usd::physics {

// What a backend or the parsing couldn't take as it is, with the prim it is about
// (§4.8). Goes to the Log.
struct PhysicsReport {
    PXR_NS::SdfPath prim;
    std::string message;
};

// A pose in metres, relative to PhysicsSceneDesc::origin.
struct Pose {
    PXR_NS::GfVec3d position{0.0};
    PXR_NS::GfQuatd rotation{1.0};
};

enum class ShapeKind { Sphere, Box, Capsule, Cylinder, Cone, Plane, ConvexHull, TriangleMesh };

// Along which local axis a capsule, cylinder, cone or plane lies (a plane: its normal).
enum class Axis { X, Y, Z };

struct MaterialDesc {
    double staticFriction = 0.5;
    double dynamicFriction = 0.5;
    double restitution = 0.0;
};

// One collider, in its body's frame (a static one: in the scene's).
struct ShapeDesc {
    PXR_NS::SdfPath prim;
    ShapeKind kind = ShapeKind::Box;
    Pose local;
    Axis axis = Axis::Y;
    PXR_NS::GfVec3d halfExtents{0.0};  // Box
    double radius = 0.0;               // Sphere, Capsule, Cylinder, Cone (its base)
    double halfHeight = 0.0;           // Capsule (of the cylinder part), Cylinder, Cone
    // ConvexHull and TriangleMesh: points in the shape's frame, scaled; TriangleMesh's
    // triangles as three indices each.
    std::vector<PXR_NS::GfVec3f> points;
    std::vector<int> triangles;
    MaterialDesc material;
};

using BodyId = std::size_t;  // an index into PhysicsSceneDesc::bodies

struct BodyDesc {
    PXR_NS::SdfPath prim;
    Pose pose;  // at the start frame
    // The prim's world matrix at the start frame less its rotation and translation: its
    // scale, and any shear a rotated parent with non-uniform scale gives it. The cache
    // writes it back unchanged (§4.6): as a scale op, or inside one matrix when sheared.
    PXR_NS::GfMatrix4d scaleShear{1.0};
    PXR_NS::GfVec3d scale{1.0};
    bool sheared = false;
    bool kinematic = false;
    bool startsAsleep = false;
    PXR_NS::GfVec3d linearVelocity{0.0};   // m/s, world
    PXR_NS::GfVec3d angularVelocity{0.0};  // rad/s, world
    // From UsdPhysicsRigidBodyAPI::ComputeMassProperties() (physics_mass.h). Zero mass:
    // the backend works it out.
    double mass = 0.0;                          // kg
    PXR_NS::GfVec3d inertia{0.0};               // kg m², diagonal, about the centre of mass
    PXR_NS::GfQuatd principalAxes{1.0};         // of `inertia`, in the body's frame
    PXR_NS::GfVec3d centerOfMass{0.0};          // m, in the body's frame
    std::vector<ShapeDesc> shapes;
};

// Joints (docs/PHYSICS.md §5.3). Angles in radians, distances in metres, forces in N,
// torques in N m: converted from the stage's units.
enum class JointKind { Fixed, Revolute, Prismatic, Spherical, Distance, Generic };

// The six degrees of freedom of a generic joint, in its frame.
enum class Dof { TransX, TransY, TransZ, RotX, RotY, RotZ };

struct JointLimitDesc {
    bool enabled = false;
    // Revolute, prismatic, a generic joint's axis: lower and upper, and lower > upper
    // locks the axis. Spherical: the cone's two half angles. Distance: min and max.
    double lower = 0.0;
    double upper = 0.0;
};

struct JointDriveDesc {
    bool enabled = false;
    double targetPosition = 0.0;
    double targetVelocity = 0.0;
    double forceLimit = 0.0;  // 0: none
    double stiffness = 0.0;   // N/m or N m/rad; with `acceleration`, 1/s²
    double damping = 0.0;     // N s/m or N m s/rad; with `acceleration`, 1/s
    bool acceleration = false;
};

struct JointDesc {
    PXR_NS::SdfPath prim;
    JointKind kind = JointKind::Fixed;
    // The bodies it joins; none for the world (or a static collider, which doesn't move).
    std::optional<BodyId> body0;
    std::optional<BodyId> body1;
    // Its frame on each side, in that body's frame, or in the scene's for the world.
    Pose frame0;
    Pose frame1;
    Axis axis = Axis::X;     // revolute, prismatic, spherical
    JointLimitDesc limit;    // revolute, prismatic, spherical; distance: lower = min, upper = max
    bool minEnabled = false;  // distance
    bool maxEnabled = false;
    JointDriveDesc drive;  // revolute, prismatic
    std::vector<std::pair<Dof, JointLimitDesc>> limits;  // generic; a DOF not here is free
    std::vector<std::pair<Dof, JointDriveDesc>> drives;
    double breakForce = 0.0;   // 0: unbreakable
    double breakTorque = 0.0;
    bool collisionEnabled = false;  // between the two bodies
};

// Which colliders may touch (docs/PHYSICS.md §5.3). An "owner" is a body (0 to bodies - 1)
// or a static collider (bodies + its index in statics): what a solver filters by.
struct CollisionFilterDesc {
    std::vector<std::vector<int>> ownerGroups;    // per owner, its collision groups
    std::vector<std::vector<bool>> groupsCollide;  // per pair of groups
    std::vector<std::pair<std::size_t, std::size_t>> pairs;  // owners that never touch
    bool empty() const { return pairs.empty() && groupsCollide.empty(); }
};

struct PhysicsSceneDesc {
    PXR_NS::SdfPath scene;  // the PhysicsScene used; empty when the stage has none
    PXR_NS::GfVec3d gravity{0.0, -9.81, 0.0};  // m/s²
    double metersPerUnit = 1.0;
    // Where poses are measured from, in metres in the stage's world: the dynamic
    // bodies' centre at the start frame, so single precision holds far from the stage's
    // origin (§7 item 7).
    PXR_NS::GfVec3d origin{0.0};
    std::vector<BodyDesc> bodies;     // dynamic and kinematic, in a stable order
    std::vector<ShapeDesc> statics;   // colliders without a body
    std::vector<JointDesc> joints;
    CollisionFilterDesc filter;
};

// Parses `stage` at `startTime`. Reports what it leaves out or changes. The stage must be
// seen as authored: a simulation cache composed in it would be parsed as the bodies' pose
// (§4.4), so the caller clears that first.
PhysicsSceneDesc parseScene(const PXR_NS::UsdStageRefPtr& stage, PXR_NS::UsdTimeCode startTime,
                            std::vector<PhysicsReport>& reports);

// A world pose of the stage (stage units) from a scene's pose, and back.
PXR_NS::GfVec3d toStagePosition(const PhysicsSceneDesc& scene, const PXR_NS::GfVec3d& position);
Pose poseFromWorld(const PhysicsSceneDesc& scene, const PXR_NS::GfMatrix4d& world);

}  // namespace usdcc::usd::physics
