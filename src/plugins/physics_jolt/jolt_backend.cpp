#include "jolt_backend.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCylinderShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace usdcc::physics_jolt {

namespace {

using namespace usdcc::usd::physics;
namespace pxr = PXR_NS;

void initJolt() {
    static const bool done = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)done;
}

constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer.GetValue() == kStaticLayer ? "static" : "moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
        return layer == kMovingLayer || broadPhase.GetValue() == kMovingLayer;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == kMovingLayer || b == kMovingLayer; }
};

JPH::Vec3 toJolt(const pxr::GfVec3d& v) { return JPH::Vec3(float(v[0]), float(v[1]), float(v[2])); }
JPH::RVec3 toJoltR(const pxr::GfVec3d& v) { return JPH::RVec3(JPH::Real(v[0]), JPH::Real(v[1]), JPH::Real(v[2])); }
JPH::Quat toJolt(const pxr::GfQuatd& q) {
    const pxr::GfVec3d& i = q.GetImaginary();
    return JPH::Quat(float(i[0]), float(i[1]), float(i[2]), float(q.GetReal())).Normalized();
}

// The rotation taking Jolt's own axis for a shape (+Y) onto a UsdPhysics axis.
JPH::Quat yOnto(Axis axis) {
    switch (axis) {
        case Axis::X: return JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -JPH::JPH_PI / 2);
        case Axis::Z: return JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::JPH_PI / 2);
        default: return JPH::Quat::sIdentity();
    }
}

// Jolt rounds a convex shape's corners by this much, which may not exceed its size.
float convexRadius(float smallest) { return std::clamp(smallest * 0.5f, 0.0f, JPH::cDefaultConvexRadius); }

class JoltWorld final : public PhysicsWorld {
public:
    JoltWorld() = default;

    bool build(const PhysicsSceneDesc& scene, std::vector<PhysicsReport>& reports) {
        const JPH::uint maxBodies = JPH::uint(scene.bodies.size() + scene.statics.size() + 16);
        const JPH::uint maxPairs = std::max<JPH::uint>(1024, maxBodies * 8);
        m_system.Init(maxBodies, 0, maxPairs, maxPairs, m_broadPhase, m_objectVsBroadPhase, m_objectPairs);
        m_system.SetGravity(toJolt(scene.gravity));
        // UsdPhysics' rule for two materials meeting: their average.
        m_system.SetCombineFriction([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) {
            return 0.5f * (a.GetFriction() + b.GetFriction());
        });
        m_system.SetCombineRestitution([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) {
            return 0.5f * (a.GetRestitution() + b.GetRestitution());
        });
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();

        for (const ShapeDesc& shape : scene.statics) {
            const JPH::ShapeRefC made = makeShape(shape, reports);
            if (!made) continue;
            JPH::BodyCreationSettings settings(made, toJoltR(shape.local.position), toJolt(shape.local.rotation) * axisTurn(shape),
                                               JPH::EMotionType::Static, kStaticLayer);
            setMaterial(settings, shape.material);
            bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        }

        m_ids.assign(scene.bodies.size(), JPH::BodyID());
        m_start.resize(scene.bodies.size());
        for (BodyId i = 0; i < scene.bodies.size(); ++i) {
            const BodyDesc& body = scene.bodies[i];
            m_start[i] = body.pose;
            JPH::ShapeRefC shape = bodyShape(body, reports);
            if (!shape) continue;
            JPH::BodyCreationSettings settings(shape, toJoltR(body.pose.position), toJolt(body.pose.rotation),
                                               body.kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic,
                                               kMovingLayer);
            if (!body.shapes.empty()) setMaterial(settings, body.shapes.front().material);
            for (const ShapeDesc& other : body.shapes) {
                const MaterialDesc& a = other.material;
                const MaterialDesc& b = body.shapes.front().material;
                if (a.dynamicFriction != b.dynamicFriction || a.restitution != b.restitution) {
                    reports.push_back({body.prim, "colliders with different materials: Jolt gives a body one, its first collider's"});
                    break;
                }
            }
            if (body.mass > 0.0) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                settings.mMassPropertiesOverride.mMass = float(body.mass);
                const JPH::Mat44 axes = JPH::Mat44::sRotation(toJolt(body.principalAxes));
                settings.mMassPropertiesOverride.mInertia =
                    axes * JPH::Mat44::sScale(toJolt(body.inertia)) * axes.Transposed3x3();
                settings.mMassPropertiesOverride.mInertia.SetColumn4(3, JPH::Vec4(0, 0, 0, 1));
            } else if (body.shapes.empty()) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3::sReplicate(0.1f), 1000.0f);
            }
            settings.mLinearVelocity = toJolt(body.linearVelocity);
            settings.mAngularVelocity = toJolt(body.angularVelocity);
            const JPH::EActivation activation =
                body.startsAsleep && !body.kinematic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
            m_ids[i] = bodies.CreateAndAddBody(settings, activation);
            if (m_ids[i].IsInvalid()) reports.push_back({body.prim, "Jolt couldn't add the body: too many bodies"});
        }
        m_system.OptimizeBroadPhase();
        return true;
    }

    void setKinematicTarget(BodyId body, const Pose& pose, double seconds) override {
        if (body >= m_ids.size() || m_ids[body].IsInvalid()) return;
        m_system.GetBodyInterface().MoveKinematic(m_ids[body], toJoltR(pose.position), toJolt(pose.rotation), float(seconds));
    }

    void step(double seconds, int substeps) override {
        m_system.Update(float(seconds), std::max(1, substeps), &m_temp, &m_jobs);
    }

    void readPoses(std::vector<Pose>& out) const override {
        out.resize(m_ids.size());
        const JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        for (size_t i = 0; i < m_ids.size(); ++i) {
            if (m_ids[i].IsInvalid()) {
                out[i] = m_start[i];
                continue;
            }
            const JPH::RVec3 p = bodies.GetPosition(m_ids[i]);
            const JPH::Quat q = bodies.GetRotation(m_ids[i]);
            out[i].position = pxr::GfVec3d(double(p.GetX()), double(p.GetY()), double(p.GetZ()));
            out[i].rotation = pxr::GfQuatd(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
        }
    }

private:
    static JPH::Quat axisTurn(const ShapeDesc& shape) {
        switch (shape.kind) {
            case ShapeKind::Capsule:
            case ShapeKind::Cylinder:
            case ShapeKind::Cone:
            case ShapeKind::Plane: return yOnto(shape.axis);
            default: return JPH::Quat::sIdentity();
        }
    }

    static void setMaterial(JPH::BodyCreationSettings& settings, const MaterialDesc& material) {
        settings.mFriction = float(material.dynamicFriction);
        settings.mRestitution = float(material.restitution);
    }

    static JPH::ShapeRefC created(const JPH::ShapeSettings& settings, const ShapeDesc& shape, std::vector<PhysicsReport>& reports) {
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            reports.push_back({shape.prim, std::string("Jolt couldn't make this collider: ") + result.GetError().c_str()});
            return nullptr;
        }
        return result.Get();
    }

    // A collider in its own frame, before its pose.
    static JPH::ShapeRefC makeShape(const ShapeDesc& shape, std::vector<PhysicsReport>& reports) {
        switch (shape.kind) {
            case ShapeKind::Sphere: return created(JPH::SphereShapeSettings(float(shape.radius)), shape, reports);
            case ShapeKind::Box: {
                const JPH::Vec3 half = toJolt(shape.halfExtents);
                return created(JPH::BoxShapeSettings(half, convexRadius(half.ReduceMin())), shape, reports);
            }
            case ShapeKind::Capsule:
                if (shape.halfHeight <= 0.0) return created(JPH::SphereShapeSettings(float(shape.radius)), shape, reports);
                return created(JPH::CapsuleShapeSettings(float(shape.halfHeight), float(shape.radius)), shape, reports);
            case ShapeKind::Cylinder:
                return created(JPH::CylinderShapeSettings(float(shape.halfHeight), float(shape.radius),
                                                          convexRadius(float(std::min(shape.halfHeight, shape.radius)))),
                               shape, reports);
            case ShapeKind::Cone:
                // Its apex up its axis (+Y here), as UsdGeomCone has it.
                return created(JPH::TaperedCylinderShapeSettings(float(shape.halfHeight), 0.0f, float(shape.radius), 0.0f),
                               shape, reports);
            case ShapeKind::Plane:
                return created(JPH::PlaneShapeSettings(JPH::Plane(JPH::Vec3::sAxisY(), 0.0f), nullptr, 1000.0f), shape, reports);
            case ShapeKind::ConvexHull: {
                JPH::Array<JPH::Vec3> points;
                points.reserve(shape.points.size());
                for (const pxr::GfVec3f& p : shape.points) points.emplace_back(p[0], p[1], p[2]);
                return created(JPH::ConvexHullShapeSettings(points.data(), int(points.size())), shape, reports);
            }
            case ShapeKind::TriangleMesh: {
                JPH::VertexList vertices;
                vertices.reserve(shape.points.size());
                for (const pxr::GfVec3f& p : shape.points) vertices.push_back(JPH::Float3(p[0], p[1], p[2]));
                JPH::IndexedTriangleList triangles;
                for (size_t i = 0; i + 2 < shape.triangles.size(); i += 3) {
                    triangles.push_back(JPH::IndexedTriangle(JPH::uint32(shape.triangles[i]), JPH::uint32(shape.triangles[i + 1]),
                                                             JPH::uint32(shape.triangles[i + 2])));
                }
                return created(JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)), shape, reports);
            }
        }
        return nullptr;
    }

    // All of a body's colliders, in its frame, with its centre of mass where UsdPhysics says.
    static JPH::ShapeRefC bodyShape(const BodyDesc& body, std::vector<PhysicsReport>& reports) {
        JPH::ShapeRefC shape;
        if (body.shapes.empty()) {
            reports.push_back({body.prim, "a rigid body without a collider: it moves, but hits nothing"});
            shape = created(JPH::EmptyShapeSettings(), ShapeDesc{body.prim}, reports);
        } else {
            const ShapeDesc& first = body.shapes.front();
            const JPH::Quat firstTurn = toJolt(first.local.rotation) * axisTurn(first);
            const bool plain = body.shapes.size() == 1 && first.local.position.GetLength() < 1e-9 &&
                               firstTurn.IsClose(JPH::Quat::sIdentity());
            if (plain) {
                shape = makeShape(first, reports);
            } else {
                JPH::StaticCompoundShapeSettings compound;
                int added = 0;
                for (const ShapeDesc& each : body.shapes) {
                    const JPH::ShapeRefC made = makeShape(each, reports);
                    if (!made) continue;
                    compound.AddShape(toJolt(each.local.position), toJolt(each.local.rotation) * axisTurn(each), made);
                    ++added;
                }
                if (added == 0) return nullptr;
                shape = created(compound, ShapeDesc{body.prim}, reports);
            }
        }
        if (!shape || body.mass <= 0.0) return shape;
        // Jolt puts the centre of mass where the shapes have it; UsdPhysics may say elsewhere.
        const JPH::Vec3 offset = toJolt(body.centerOfMass) - shape->GetCenterOfMass();
        if (offset.LengthSq() < 1e-12f) return shape;
        return created(JPH::OffsetCenterOfMassShapeSettings(offset, shape), ShapeDesc{body.prim}, reports);
    }

    // First: the members after it allocate through Jolt's allocator, which this registers.
    struct Init {
        Init() { initJolt(); }
    } m_init;
    BroadPhaseLayers m_broadPhase;
    ObjectVsBroadPhase m_objectVsBroadPhase;
    ObjectPairs m_objectPairs;
    JPH::TempAllocatorImpl m_temp{64 * 1024 * 1024};
    JPH::JobSystemThreadPool m_jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                    int(std::max(1u, std::thread::hardware_concurrency() - 1))};
    JPH::PhysicsSystem m_system;
    std::vector<JPH::BodyID> m_ids;  // per BodyId; invalid for one Jolt couldn't take
    std::vector<Pose> m_start;
};

class JoltBackend final : public PhysicsBackend {
public:
    std::string id() const override { return "org.usdcc.physics-jolt"; }
    std::string name() const override { return "Jolt"; }

    std::unique_ptr<PhysicsWorld> build(const PhysicsSceneDesc& scene, std::vector<PhysicsReport>& reports) override {
        auto world = std::make_unique<JoltWorld>();
        if (!world->build(scene, reports)) return nullptr;
        return world;
    }
};

}  // namespace

std::shared_ptr<PhysicsBackend> makeJoltBackend() {
    initJolt();
    return std::make_shared<JoltBackend>();
}

}  // namespace usdcc::physics_jolt
