#include "usdcc/usd/physics_desc.h"

#include "usdcc/usd/physics_mass.h"

#include <pxr/base/gf/matrix3f.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/tf/span.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdPhysics/collisionGroup.h>
#include <pxr/usd/usdPhysics/metrics.h>
#include <pxr/usd/usdPhysics/parseDesc.h>
#include <pxr/usd/usdPhysics/parseUtils.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/tokens.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::physics {

namespace {

constexpr double kDegrees = 3.14159265358979323846 / 180.0;
constexpr double kEarthGravity = 9.81;

// What the parser reported, by type. Its spans hold the derived type of the report.
struct Parsed {
    std::vector<UsdPhysicsSceneDesc> scenes;
    std::vector<UsdPhysicsRigidBodyDesc> bodies;
    std::vector<UsdPhysicsRigidBodyMaterialDesc> materials;
    std::vector<UsdPhysicsSphereShapeDesc> spheres;
    std::vector<UsdPhysicsCubeShapeDesc> cubes;
    std::vector<UsdPhysicsCapsuleShapeDesc> capsules;
    std::vector<UsdPhysicsCapsule1ShapeDesc> capsules1;
    std::vector<UsdPhysicsCylinderShapeDesc> cylinders;
    std::vector<UsdPhysicsCylinder1ShapeDesc> cylinders1;
    std::vector<UsdPhysicsConeShapeDesc> cones;
    std::vector<UsdPhysicsMeshShapeDesc> meshes;
    std::vector<UsdPhysicsPlaneShapeDesc> planes;
    std::vector<SdfPath> unsupportedShapes;  // sphere points, custom shapes
    std::vector<UsdPhysicsFixedJointDesc> fixedJoints;
    std::vector<UsdPhysicsRevoluteJointDesc> revoluteJoints;
    std::vector<UsdPhysicsPrismaticJointDesc> prismaticJoints;
    std::vector<UsdPhysicsSphericalJointDesc> sphericalJoints;
    std::vector<UsdPhysicsDistanceJointDesc> distanceJoints;
    std::vector<UsdPhysicsD6JointDesc> genericJoints;
    std::vector<SdfPath> customJoints;
    std::vector<UsdPhysicsCollisionGroupDesc> collisionGroups;
    std::vector<SdfPath> articulations;
};

template <class T>
void append(std::vector<T>& out, TfSpan<const UsdPhysicsObjectDesc> descs) {
    const T* first = static_cast<const T*>(descs.data());
    out.insert(out.end(), first, first + descs.size());
}

Parsed parse(const UsdStageRefPtr& stage) {
    Parsed parsed;
    UsdPhysicsLoadStageFromPrimRange(
        stage, {SdfPath::AbsoluteRootPath()},
        [&parsed](UsdPhysicsObjectType type, TfSpan<const SdfPath> paths, TfSpan<const UsdPhysicsObjectDesc> descs,
                  const VtValue&) {
            switch (type) {
                case UsdPhysicsObjectType::Scene: append(parsed.scenes, descs); break;
                case UsdPhysicsObjectType::RigidBody: append(parsed.bodies, descs); break;
                case UsdPhysicsObjectType::RigidBodyMaterial: append(parsed.materials, descs); break;
                case UsdPhysicsObjectType::SphereShape: append(parsed.spheres, descs); break;
                case UsdPhysicsObjectType::CubeShape: append(parsed.cubes, descs); break;
                case UsdPhysicsObjectType::CapsuleShape: append(parsed.capsules, descs); break;
                case UsdPhysicsObjectType::Capsule1Shape: append(parsed.capsules1, descs); break;
                case UsdPhysicsObjectType::CylinderShape: append(parsed.cylinders, descs); break;
                case UsdPhysicsObjectType::Cylinder1Shape: append(parsed.cylinders1, descs); break;
                case UsdPhysicsObjectType::ConeShape: append(parsed.cones, descs); break;
                case UsdPhysicsObjectType::MeshShape: append(parsed.meshes, descs); break;
                case UsdPhysicsObjectType::PlaneShape: append(parsed.planes, descs); break;
                case UsdPhysicsObjectType::SpherePointsShape:
                case UsdPhysicsObjectType::CustomShape:
                    parsed.unsupportedShapes.insert(parsed.unsupportedShapes.end(), paths.begin(), paths.end());
                    break;
                case UsdPhysicsObjectType::FixedJoint: append(parsed.fixedJoints, descs); break;
                case UsdPhysicsObjectType::RevoluteJoint: append(parsed.revoluteJoints, descs); break;
                case UsdPhysicsObjectType::PrismaticJoint: append(parsed.prismaticJoints, descs); break;
                case UsdPhysicsObjectType::SphericalJoint: append(parsed.sphericalJoints, descs); break;
                case UsdPhysicsObjectType::DistanceJoint: append(parsed.distanceJoints, descs); break;
                case UsdPhysicsObjectType::D6Joint: append(parsed.genericJoints, descs); break;
                case UsdPhysicsObjectType::CustomJoint:
                    parsed.customJoints.insert(parsed.customJoints.end(), paths.begin(), paths.end());
                    break;
                case UsdPhysicsObjectType::CollisionGroup: append(parsed.collisionGroups, descs); break;
                case UsdPhysicsObjectType::Articulation:
                    parsed.articulations.insert(parsed.articulations.end(), paths.begin(), paths.end());
                    break;
                default: break;
            }
        },
        VtValue());
    // UsdPhysicsCollisionGroupDesc leaves invertFilteredGroups uninitialized, and the
    // parser's Get() of it changes nothing when it isn't authored (the schema has no
    // fallback), so an ordinary group could come out inverted. On Linux it did.
    for (UsdPhysicsCollisionGroupDesc& group : parsed.collisionGroups) {
        bool invert = false;
        if (const UsdPhysicsCollisionGroup prim{stage->GetPrimAtPath(group.primPath)}) {
            prim.GetInvertFilteredGroupsAttr().Get(&invert);
        }
        group.invertFilteredGroups = invert;
    }
    return parsed;
}

Axis axisOf(UsdPhysicsAxis axis) {
    switch (axis) {
        case UsdPhysicsAxis::X: return Axis::X;
        case UsdPhysicsAxis::Z: return Axis::Z;
        default: return Axis::Y;
    }
}

// The instance a prim inside one belongs to: the nearest ancestor that is an instance.
SdfPath instanceOf(const UsdPrim& prim) {
    for (UsdPrim p = prim; p; p = p.GetParent()) {
        if (p.IsInstance()) return p.GetPath();
    }
    return prim.GetPath();
}

class Builder {
public:
    Builder(const UsdStageRefPtr& stage, UsdTimeCode start, std::vector<PhysicsReport>& reports)
        : m_stage(stage), m_xforms(start), m_start(start), m_reports(reports) {
        m_scene.metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
        if (m_scene.metersPerUnit <= 0.0) m_scene.metersPerUnit = 1.0;
    }

    PhysicsSceneDesc build() {
        const Parsed parsed = parse(m_stage);
        chooseScene(parsed);
        for (const UsdPhysicsRigidBodyMaterialDesc& material : parsed.materials) {
            m_materials[material.primPath] = {material.staticFriction, material.dynamicFriction, material.restitution};
        }
        addBodies(parsed);
        for (const auto& d : parsed.spheres) {
            ShapeDesc shape = shapeFor(d, ShapeKind::Sphere);
            shape.radius = d.radius * m_scene.metersPerUnit;
            place(d, std::move(shape));
        }
        for (const auto& d : parsed.cubes) {
            ShapeDesc shape = shapeFor(d, ShapeKind::Box);
            shape.halfExtents = GfVec3d(d.halfExtents) * m_scene.metersPerUnit;
            place(d, std::move(shape));
        }
        for (const auto& d : parsed.capsules) place(d, rounded(d, ShapeKind::Capsule, d.radius, d.halfHeight, d.axis));
        for (const auto& d : parsed.cylinders) place(d, rounded(d, ShapeKind::Cylinder, d.radius, d.halfHeight, d.axis));
        for (const auto& d : parsed.cones) place(d, rounded(d, ShapeKind::Cone, d.radius, d.halfHeight, d.axis));
        for (const auto& d : parsed.capsules1) {
            report(d.primPath, "a capsule with two radii: simulated with the larger");
            place(d, rounded(d, ShapeKind::Capsule, std::max(d.topRadius, d.bottomRadius), d.halfHeight, d.axis));
        }
        for (const auto& d : parsed.cylinders1) {
            report(d.primPath, "a cylinder with two radii: simulated with the larger");
            place(d, rounded(d, ShapeKind::Cylinder, std::max(d.topRadius, d.bottomRadius), d.halfHeight, d.axis));
        }
        for (const auto& d : parsed.meshes) addMesh(d);
        for (const auto& d : parsed.planes) {
            ShapeDesc shape = shapeFor(d, ShapeKind::Plane);
            shape.axis = axisOf(d.axis);
            if (!d.rigidBody.IsEmpty() && isMoving(d.rigidBody)) {
                report(d.primPath, "a plane can only be a static collider: left out");
                continue;
            }
            place(d, std::move(shape));
        }
        for (const SdfPath& path : parsed.unsupportedShapes) report(path, "a collider of a kind not simulated yet: left out");
        for (const SdfPath& path : parsed.articulations) {
            report(path, "an articulation: its joints are simulated as plain joints, not reduced coordinates");
        }

        // A body with nothing to collide with still falls, with the mass it says.
        for (BodyDesc& body : m_scene.bodies) computeMass(body);
        recentre();
        addJoints(parsed);
        buildFilter(parsed);
        return std::move(m_scene);
    }

private:
    void report(const SdfPath& prim, std::string message) { m_reports.push_back({prim, std::move(message)}); }

    void chooseScene(const Parsed& parsed) {
        const double m = m_scene.metersPerUnit;
        if (parsed.scenes.empty()) {
            const TfToken up = UsdGeomGetStageUpAxis(m_stage);
            m_scene.gravity = up == UsdGeomTokens->z ? GfVec3d(0, 0, -kEarthGravity) : GfVec3d(0, -kEarthGravity, 0);
            report(SdfPath::AbsoluteRootPath(),
                   std::string("no PhysicsScene: simulated with Earth's gravity down the ") + up.GetString() + " axis");
            return;
        }
        std::vector<UsdPhysicsSceneDesc> scenes = parsed.scenes;
        std::sort(scenes.begin(), scenes.end(), [](const auto& a, const auto& b) { return a.primPath < b.primPath; });
        const UsdPhysicsSceneDesc& scene = scenes.front();
        m_scene.scene = scene.primPath;
        m_scene.gravity = GfVec3d(scene.gravityDirection) * double(scene.gravityMagnitude) * m;
        if (scenes.size() > 1) {
            report(scenes[1].primPath, "a second PhysicsScene: only " + scene.primPath.GetString() + " is simulated");
        }
    }

    // Simulated by this scene: owned by nobody, or by it.
    bool ownedHere(const SdfPathVector& owners) const {
        if (owners.empty()) return true;
        return !m_scene.scene.IsEmpty() && std::find(owners.begin(), owners.end(), m_scene.scene) != owners.end();
    }

    void addBodies(const Parsed& parsed) {
        std::vector<UsdPhysicsRigidBodyDesc> bodies = parsed.bodies;
        std::sort(bodies.begin(), bodies.end(), [](const auto& a, const auto& b) { return a.primPath < b.primPath; });
        std::set<SdfPath> reportedInstances;
        for (const UsdPhysicsRigidBodyDesc& desc : bodies) {
            const UsdPrim prim = m_stage->GetPrimAtPath(desc.primPath);
            if (!prim) continue;
            if (!ownedHere(desc.simulationOwners)) {
                m_leftOut.insert(desc.primPath);
                report(desc.primPath, "simulated by another PhysicsScene: left out");
                continue;
            }
            if (prim.IsInstanceProxy()) {
                // Its pose can't be written: opinions inside an instance are ignored (§4.8).
                m_leftOut.insert(desc.primPath);
                const SdfPath instance = instanceOf(prim);
                if (reportedInstances.insert(instance).second) {
                    report(instance, "rigid bodies inside an instance: left out, as their poses can't be written");
                }
                continue;
            }
            if (!desc.rigidBodyEnabled) continue;  // its colliders are static ones
            BodyDesc body;
            body.prim = desc.primPath;
            const GfMatrix4d world = m_xforms.GetLocalToWorldTransform(prim);
            body.pose = worldPose(world);
            const GfTransform transform(world);
            body.scale = transform.GetScale();
            // The world matrix with its rotation and translation taken off: M = S R T.
            GfMatrix4d rotateTranslate(1.0);
            rotateTranslate.SetRotate(transform.GetRotation());
            rotateTranslate.SetTranslateOnly(transform.GetTranslation());
            body.scaleShear = world * rotateTranslate.GetInverse();
            // Anything off its diagonal is shear, which three ops can't say.
            const double size = std::max({std::abs(body.scale[0]), std::abs(body.scale[1]), std::abs(body.scale[2]), 1e-12});
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if (row != column && std::abs(body.scaleShear[row][column]) > 1e-9 * size) body.sheared = true;
                }
            }
            body.kinematic = desc.kinematicBody;
            body.startsAsleep = desc.startsAsleep;
            body.linearVelocity = GfVec3d(desc.linearVelocity) * m_scene.metersPerUnit;
            body.angularVelocity = GfVec3d(desc.angularVelocity) * kDegrees;
            for (const SdfPath& other : desc.filteredCollisions) m_filtered.emplace_back(m_scene.bodies.size(), other);
            m_bodyIndex[desc.primPath] = m_scene.bodies.size();
            m_bodyWorld[desc.primPath] = world;
            m_scene.bodies.push_back(std::move(body));
        }
    }

    bool isMoving(const SdfPath& body) const { return m_bodyIndex.count(body) > 0; }

    // The world pose in metres, not yet recentred.
    Pose worldPose(const GfMatrix4d& world) const {
        const GfTransform transform(world);
        Pose pose;
        pose.position = transform.GetTranslation() * m_scene.metersPerUnit;
        pose.rotation = transform.GetRotation().GetQuat().GetNormalized();
        return pose;
    }

    ShapeDesc shapeFor(const UsdPhysicsShapeDesc& desc, ShapeKind kind) {
        ShapeDesc shape;
        shape.prim = desc.primPath;
        shape.kind = kind;
        for (const SdfPath& material : desc.materials) {
            if (const auto it = m_materials.find(material); it != m_materials.end()) {
                shape.material = it->second;
                break;
            }
        }
        // Its pose in the frame it goes in: its body's, or the world's for a static one.
        const UsdPrim prim = m_stage->GetPrimAtPath(desc.primPath);
        const Pose world = worldPose(m_xforms.GetLocalToWorldTransform(prim));
        const auto body = m_bodyWorld.find(desc.rigidBody);
        if (body != m_bodyWorld.end()) {
            const Pose bodyPose = worldPose(body->second);
            const GfQuatd inverse = bodyPose.rotation.GetInverse();
            shape.local.position = GfRotation(inverse).TransformDir(world.position - bodyPose.position);
            shape.local.rotation = (inverse * world.rotation).GetNormalized();
        } else {
            shape.local = world;
        }
        return shape;
    }

    template <class Desc>
    ShapeDesc rounded(const Desc& d, ShapeKind kind, float radius, float halfHeight, UsdPhysicsAxis axis) {
        ShapeDesc shape = shapeFor(d, kind);
        shape.radius = radius * m_scene.metersPerUnit;
        shape.halfHeight = halfHeight * m_scene.metersPerUnit;
        shape.axis = axisOf(axis);
        return shape;
    }

    void addMesh(const UsdPhysicsMeshShapeDesc& d) {
        const UsdGeomMesh mesh(m_stage->GetPrimAtPath(d.primPath));
        VtVec3fArray points;
        VtIntArray counts, indices;
        mesh.GetPointsAttr().Get(&points, m_start);
        mesh.GetFaceVertexCountsAttr().Get(&counts, m_start);
        mesh.GetFaceVertexIndicesAttr().Get(&indices, m_start);
        if (points.empty()) {
            report(d.primPath, "a mesh collider without points: left out");
            return;
        }
        ShapeDesc shape = shapeFor(d, ShapeKind::ConvexHull);
        // The mesh's own scale, which its pose doesn't carry, goes into its points.
        const GfVec3d scale =
            GfTransform(m_xforms.GetLocalToWorldTransform(mesh.GetPrim())).GetScale() * m_scene.metersPerUnit;
        shape.points.reserve(points.size());
        for (const GfVec3f& p : points) shape.points.push_back(GfVec3f(GfCompMult(GfVec3d(p), scale)));

        const TfToken& approximation = d.approximation;
        const bool dynamic = !d.rigidBody.IsEmpty() && isMoving(d.rigidBody) &&
                             !m_scene.bodies[m_bodyIndex.at(d.rigidBody)].kinematic;
        if (approximation.IsEmpty() || approximation == UsdPhysicsTokens->none) {
            if (dynamic) {
                report(d.primPath, "a triangle mesh can't collide on a moving body: simulated as its convex hull");
            } else {
                shape.kind = ShapeKind::TriangleMesh;
                for (size_t face = 0, at = 0; face < counts.size(); at += counts[face], ++face) {
                    for (int k = 2; k < counts[face]; ++k) {  // a fan
                        const int a = indices[at], b = indices[at + k - 1], c = indices[at + k];
                        shape.triangles.insert(shape.triangles.end(), {a, b, c});
                    }
                }
            }
        } else if (approximation == UsdPhysicsTokens->boundingCube || approximation == UsdPhysicsTokens->boundingSphere) {
            GfVec3d low(shape.points.front()), high(shape.points.front());
            for (const GfVec3f& p : shape.points) {
                for (int k = 0; k < 3; ++k) {
                    low[k] = std::min(low[k], double(p[k]));
                    high[k] = std::max(high[k], double(p[k]));
                }
            }
            const GfVec3d centre = (low + high) / 2.0;
            if (approximation == UsdPhysicsTokens->boundingCube) {
                shape.kind = ShapeKind::Box;
                shape.halfExtents = (high - low) / 2.0;
            } else {
                shape.kind = ShapeKind::Sphere;
                for (const GfVec3f& p : shape.points) shape.radius = std::max(shape.radius, (GfVec3d(p) - centre).GetLength());
            }
            shape.local.position += GfRotation(shape.local.rotation).TransformDir(centre);
            shape.points.clear();
        } else if (approximation != UsdPhysicsTokens->convexHull) {
            report(d.primPath, "the '" + approximation.GetString() + "' approximation isn't simulated yet: its convex hull is");
        }
        place(d, std::move(shape));
    }

    void place(const UsdPhysicsShapeDesc& desc, ShapeDesc shape) {
        if (!desc.collisionEnabled) return;
        if (!ownedHere(desc.simulationOwners)) {
            report(desc.primPath, "simulated by another PhysicsScene: left out");
            return;
        }
        if (!desc.rigidBody.IsEmpty() && m_leftOut.count(desc.rigidBody)) return;
        // A static collider inside an instance never moves, so it can stay.
        std::size_t owner = 0;
        if (const auto it = m_bodyIndex.find(desc.rigidBody); it != m_bodyIndex.end()) {
            owner = it->second;
            m_scene.bodies[it->second].shapes.push_back(std::move(shape));
        } else {
            owner = m_scene.bodies.size() + m_scene.statics.size();
            m_scene.statics.push_back(std::move(shape));
        }
        m_colliderOwner[desc.primPath] = owner;
        // Its collision groups go to its owner, which Jolt and PhysX filter by.
        const std::set<SdfPath> groups(desc.collisionGroups.begin(), desc.collisionGroups.end());
        if (const auto known = m_ownerGroups.find(owner); known != m_ownerGroups.end()) {
            if (known->second != groups && m_mixedGroups.insert(owner).second) {
                report(desc.primPath, "a body's colliders in different collision groups: filtered as if all were in all");
            }
            known->second.insert(groups.begin(), groups.end());
        } else {
            m_ownerGroups[owner] = groups;
        }
        for (const SdfPath& other : desc.filteredCollisions) m_filtered.emplace_back(owner, other);
    }

    // The owner (a body, or a static collider) a path names: a body, or a collider.
    std::optional<std::size_t> ownerOf(const SdfPath& path) const {
        if (const auto body = m_bodyIndex.find(path); body != m_bodyIndex.end()) return body->second;
        if (const auto collider = m_colliderOwner.find(path); collider != m_colliderOwner.end()) return collider->second;
        return std::nullopt;
    }

    // Collision groups and filtered pairs (§5.3), between owners.
    void buildFilter(const Parsed& parsed) {
        CollisionFilterDesc& filter = m_scene.filter;
        for (const auto& [owner, other] : m_filtered) {
            if (const std::optional<std::size_t> second = ownerOf(other); second && *second != owner) {
                filter.pairs.emplace_back(std::min(owner, *second), std::max(owner, *second));
            }
        }
        const std::size_t groupCount = parsed.collisionGroups.size();
        if (groupCount == 0) return;
        std::map<SdfPath, int> groupIndex;
        for (std::size_t g = 0; g < groupCount; ++g) groupIndex[parsed.collisionGroups[g].primPath] = int(g);
        // Groups merged under one name filter as one.
        std::map<std::string, std::set<int>> merged;
        for (std::size_t g = 0; g < groupCount; ++g) {
            const std::string& name = parsed.collisionGroups[g].mergeGroupName;
            if (!name.empty()) merged[name].insert(int(g));
        }
        auto withMerged = [&](int g) {
            const std::string& name = parsed.collisionGroups[g].mergeGroupName;
            return name.empty() ? std::set<int>{g} : merged[name];
        };
        std::vector<std::set<int>> filtered(groupCount);
        for (std::size_t g = 0; g < groupCount; ++g) {
            for (const int same : withMerged(int(g))) {
                for (const SdfPath& path : parsed.collisionGroups[same].filteredGroups) {
                    if (const auto it = groupIndex.find(path); it != groupIndex.end()) {
                        for (const int h : withMerged(it->second)) filtered[g].insert(h);
                    }
                }
            }
        }
        filter.groupsCollide.assign(groupCount, std::vector<bool>(groupCount, true));
        bool anyFiltered = false;
        for (std::size_t g = 0; g < groupCount; ++g) {
            const bool invert = parsed.collisionGroups[g].invertFilteredGroups;
            for (std::size_t h = 0; h < groupCount; ++h) {
                const bool listed = filtered[g].count(int(h)) > 0;
                if (invert ? listed : !listed) continue;
                filter.groupsCollide[g][h] = filter.groupsCollide[h][g] = false;
                anyFiltered = true;
            }
        }
        if (!anyFiltered) {
            filter.groupsCollide.clear();
            return;
        }
        filter.ownerGroups.assign(m_scene.bodies.size() + m_scene.statics.size(), {});
        for (const auto& [owner, groups] : m_ownerGroups) {
            for (const SdfPath& group : groups) {
                if (const auto it = groupIndex.find(group); it != groupIndex.end()) filter.ownerGroups[owner].push_back(it->second);
            }
        }
    }

    // A joint's frame on one side: in its body's frame, or in the scene's.
    struct Side {
        std::optional<BodyId> body;
        Pose frame;
        bool valid = true;
    };

    Side jointSide(const UsdPhysicsJointDesc& joint, const SdfPath& bodyPath, const GfVec3f& position,
                   const GfQuatf& rotation) {
        const double m = m_scene.metersPerUnit;
        Side side;
        const Pose local{GfVec3d(position) * m, GfQuatd(rotation).GetNormalized()};
        if (bodyPath.IsEmpty()) {
            side.frame = {local.position - m_scene.origin, local.rotation};
            return side;
        }
        if (const auto it = m_bodyIndex.find(bodyPath); it != m_bodyIndex.end()) {
            side.body = it->second;
            side.frame = local;
            return side;
        }
        if (m_leftOut.count(bodyPath)) {
            report(joint.primPath, "joins " + bodyPath.GetString() + ", which isn't simulated: left out");
            side.valid = false;
            return side;
        }
        // A body that isn't simulated as one (its RigidBodyAPI disabled): where it stands.
        const Pose world = worldPose(m_xforms.GetLocalToWorldTransform(m_stage->GetPrimAtPath(bodyPath)));
        side.frame.rotation = (world.rotation * local.rotation).GetNormalized();
        side.frame.position = world.position + GfRotation(world.rotation).TransformDir(local.position) - m_scene.origin;
        return side;
    }

    // Degrees to radians for angles, the stage's units to metres for distances.
    JointLimitDesc limitFrom(const UsdPhysicsJointLimit& limit, bool angular) const {
        const double scale = angular ? kDegrees : m_scene.metersPerUnit;
        return {limit.enabled, limit.lower * scale, limit.upper * scale};
    }

    JointDriveDesc driveFrom(const UsdPhysicsJointDrive& drive, bool angular) const {
        const double m = m_scene.metersPerUnit;
        const double kg = UsdPhysicsGetStageKilogramsPerUnit(m_stage);
        JointDriveDesc result;
        result.enabled = drive.enabled;
        result.acceleration = drive.acceleration;
        const double perUnit = angular ? kDegrees : m;  // a position or velocity
        result.targetPosition = drive.targetPosition * perUnit;
        result.targetVelocity = drive.targetVelocity * perUnit;
        // A force (kg u/s²) or a torque (kg u²/s²), per unit or per degree.
        const double force = angular ? kg * m * m : kg * m;
        result.forceLimit = finite(drive.forceLimit) ? drive.forceLimit * force : 0.0;
        if (drive.acceleration) {
            result.stiffness = drive.stiffness;
            result.damping = drive.damping;
        } else {
            result.stiffness = drive.stiffness * force / perUnit;
            result.damping = drive.damping * force / perUnit;
        }
        return result;
    }

    static bool finite(float value) { return std::isfinite(value) && value < 1e30f && value > 0.0f; }

    // Whether it can be simulated, and its common part filled in.
    bool commonJoint(const UsdPhysicsJointDesc& d, JointKind kind, JointDesc& joint) {
        if (!d.jointEnabled) return false;
        const Side side0 = jointSide(d, d.body0, d.localPose0Position, d.localPose0Orientation);
        const Side side1 = jointSide(d, d.body1, d.localPose1Position, d.localPose1Orientation);
        if (!side0.valid || !side1.valid) return false;
        if (!side0.body && !side1.body) {
            report(d.primPath, "joins nothing that moves: left out");
            return false;
        }
        joint.prim = d.primPath;
        joint.kind = kind;
        joint.body0 = side0.body;
        joint.body1 = side1.body;
        joint.frame0 = side0.frame;
        joint.frame1 = side1.frame;
        joint.collisionEnabled = d.collisionEnabled;
        const double m = m_scene.metersPerUnit;
        const double kg = UsdPhysicsGetStageKilogramsPerUnit(m_stage);
        joint.breakForce = finite(d.breakForce) ? d.breakForce * kg * m : 0.0;
        joint.breakTorque = finite(d.breakTorque) ? d.breakTorque * kg * m * m : 0.0;
        return true;
    }

    void addJoint(JointDesc joint) {
        if (!joint.collisionEnabled && joint.body0 && joint.body1) {
            m_scene.filter.pairs.emplace_back(std::min(*joint.body0, *joint.body1), std::max(*joint.body0, *joint.body1));
        }
        m_scene.joints.push_back(std::move(joint));
    }

    void addJoints(const Parsed& parsed) {
        for (const auto& d : parsed.fixedJoints) {
            JointDesc joint;
            if (commonJoint(d, JointKind::Fixed, joint)) addJoint(std::move(joint));
        }
        for (const auto& d : parsed.revoluteJoints) {
            JointDesc joint;
            if (!commonJoint(d, JointKind::Revolute, joint)) continue;
            joint.axis = axisOf(d.axis);
            joint.limit = limitFrom(d.limit, true);
            joint.drive = driveFrom(d.drive, true);
            addJoint(std::move(joint));
        }
        for (const auto& d : parsed.prismaticJoints) {
            JointDesc joint;
            if (!commonJoint(d, JointKind::Prismatic, joint)) continue;
            joint.axis = axisOf(d.axis);
            joint.limit = limitFrom(d.limit, false);
            joint.drive = driveFrom(d.drive, false);
            addJoint(std::move(joint));
        }
        for (const auto& d : parsed.sphericalJoints) {
            JointDesc joint;
            if (!commonJoint(d, JointKind::Spherical, joint)) continue;
            joint.axis = axisOf(d.axis);
            joint.limit = limitFrom(d.limit, true);
            addJoint(std::move(joint));
        }
        for (const auto& d : parsed.distanceJoints) {
            JointDesc joint;
            if (!commonJoint(d, JointKind::Distance, joint)) continue;
            joint.minEnabled = d.minEnabled;
            joint.maxEnabled = d.maxEnabled;
            joint.limit = limitFrom(d.limit, false);
            addJoint(std::move(joint));
        }
        for (const auto& d : parsed.genericJoints) {
            JointDesc joint;
            if (!commonJoint(d, JointKind::Generic, joint)) continue;
            auto dofOf = [](UsdPhysicsJointDOF dof) -> std::optional<Dof> {
                switch (dof) {
                    case UsdPhysicsJointDOF::TransX: return Dof::TransX;
                    case UsdPhysicsJointDOF::TransY: return Dof::TransY;
                    case UsdPhysicsJointDOF::TransZ: return Dof::TransZ;
                    case UsdPhysicsJointDOF::RotX: return Dof::RotX;
                    case UsdPhysicsJointDOF::RotY: return Dof::RotY;
                    case UsdPhysicsJointDOF::RotZ: return Dof::RotZ;
                    default: return std::nullopt;
                }
            };
            auto angular = [](Dof dof) { return dof == Dof::RotX || dof == Dof::RotY || dof == Dof::RotZ; };
            for (const auto& [dof, limit] : d.jointLimits) {
                if (const std::optional<Dof> which = dofOf(dof)) {
                    joint.limits.emplace_back(*which, limitFrom(limit, angular(*which)));
                } else {
                    report(d.primPath, "a distance limit on a generic joint isn't simulated yet");
                }
            }
            for (const auto& [dof, drive] : d.jointDrives) {
                if (const std::optional<Dof> which = dofOf(dof)) joint.drives.emplace_back(*which, driveFrom(drive, angular(*which)));
            }
            addJoint(std::move(joint));
        }
        for (const SdfPath& path : parsed.customJoints) report(path, "a custom joint: left out");
    }

    void computeMass(BodyDesc& body) {
        std::map<SdfPath, const ShapeDesc*> shapes;
        for (const ShapeDesc& shape : body.shapes) shapes[shape.prim] = &shape;
        const double m = m_scene.metersPerUnit;
        // In stage units, as ComputeMassProperties() works in them.
        const UsdPhysicsRigidBodyAPI::MassInformationFn info = [&shapes, m](const UsdPrim& prim) {
            UsdPhysicsRigidBodyAPI::MassInformation result{};
            result.localRot = GfQuatf(1.0f);
            const auto it = shapes.find(prim.GetPath());
            if (it == shapes.end()) {
                result.volume = -1.0f;
                return result;
            }
            const ShapeMass mass = shapeMass(*it->second);
            result.volume = static_cast<float>(mass.volume / (m * m * m));
            result.inertia = GfMatrix3f(mass.inertia * (1.0 / (m * m * m * m * m)));
            result.centerOfMass = GfVec3f(mass.centerOfMass / m);
            result.localPos = GfVec3f(it->second->local.position / m);
            result.localRot = GfQuatf(it->second->local.rotation);
            return result;
        };
        GfVec3f inertia(0.0f), centre(0.0f);
        GfQuatf axes(1.0f);
        const float mass = UsdPhysicsRigidBodyAPI(m_stage->GetPrimAtPath(body.prim))
                               .ComputeMassProperties(&inertia, &centre, &axes, info);
        if (!(mass > 0.0f)) return;  // the backend works it out
        const double kilograms = UsdPhysicsGetStageKilogramsPerUnit(m_stage);
        body.mass = mass * kilograms;
        body.inertia = GfVec3d(inertia) * kilograms * m * m;
        body.principalAxes = GfQuatd(axes).GetNormalized();
        body.centerOfMass = GfVec3d(centre) * m;
    }

    // Poses from the dynamic bodies' centre (§7 item 7).
    void recentre() {
        GfVec3d sum(0.0);
        size_t count = 0;
        for (const BodyDesc& body : m_scene.bodies) {
            if (body.kinematic) continue;
            sum += body.pose.position;
            ++count;
        }
        if (count == 0) return;
        // Whole metres: the offset itself is then exact, whatever the units.
        GfVec3d origin = sum / double(count);
        for (int k = 0; k < 3; ++k) origin[k] = std::round(origin[k]);
        m_scene.origin = origin;
        for (BodyDesc& body : m_scene.bodies) body.pose.position -= origin;
        for (ShapeDesc& shape : m_scene.statics) shape.local.position -= origin;
    }

    UsdStageRefPtr m_stage;
    UsdGeomXformCache m_xforms;
    UsdTimeCode m_start;
    std::vector<PhysicsReport>& m_reports;
    PhysicsSceneDesc m_scene;
    std::map<SdfPath, MaterialDesc> m_materials;
    std::map<SdfPath, BodyId> m_bodyIndex;
    std::map<SdfPath, GfMatrix4d> m_bodyWorld;
    std::set<SdfPath> m_leftOut;  // bodies not simulated, whose colliders go with them
    std::map<SdfPath, std::size_t> m_colliderOwner;
    std::map<std::size_t, std::set<SdfPath>> m_ownerGroups;
    std::set<std::size_t> m_mixedGroups;  // reported already
    std::vector<std::pair<std::size_t, SdfPath>> m_filtered;  // an owner, and what it doesn't touch
};

}  // namespace

PhysicsSceneDesc parseScene(const UsdStageRefPtr& stage, UsdTimeCode startTime, std::vector<PhysicsReport>& reports) {
    if (!stage) return {};
    return Builder(stage, startTime, reports).build();
}

GfVec3d toStagePosition(const PhysicsSceneDesc& scene, const GfVec3d& position) {
    return (position + scene.origin) / scene.metersPerUnit;
}

Pose poseFromWorld(const PhysicsSceneDesc& scene, const GfMatrix4d& world) {
    const GfTransform transform(world);
    Pose pose;
    pose.position = transform.GetTranslation() * scene.metersPerUnit - scene.origin;
    pose.rotation = transform.GetRotation().GetQuat().GetNormalized();
    return pose;
}

}  // namespace usdcc::usd::physics
