// Phase 0 of docs/PHYSICS.md: a spike, not the design. Jolt is driven straight from
// OpenUSD's UsdPhysics parser, to see a cube come to rest on a plane and to check what
// §7 of the plan only believed. The tests that measure print what they found; their
// CHECKs are the plan's assumptions, so a failure here is a finding, not a flake.
#include "test_harness.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCylinderShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/tf/span.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdPhysics/collisionAPI.h>
#include <pxr/usd/usdPhysics/massAPI.h>
#include <pxr/usd/usdPhysics/parseDesc.h>
#include <pxr/usd/usdPhysics/parseUtils.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/scene.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// --- Jolt --------------------------------------------------------------------------------

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
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == kMovingLayer || b == kMovingLayer;
    }
};

struct World {
    // First: the members after it allocate through Jolt's allocator, which this registers.
    struct Init {
        Init() { initJolt(); }
    } init;
    BroadPhaseLayers broadPhase;
    ObjectVsBroadPhase objectVsBroadPhase;
    ObjectPairs objectPairs;
    JPH::TempAllocatorImpl temp{32 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                  static_cast<int>(std::max(1u, std::thread::hardware_concurrency() - 1))};
    JPH::PhysicsSystem system;

    explicit World(JPH::uint maxBodies = 4096) {
        system.Init(maxBodies, 0, maxBodies * 4, maxBodies * 4, broadPhase, objectVsBroadPhase, objectPairs);
    }
    JPH::BodyInterface& bodies() { return system.GetBodyInterface(); }
    void step(double seconds, int substeps) {
        system.Update(static_cast<float>(seconds), substeps, &temp, &jobs);
    }
};

JPH::Quat toJolt(const GfQuatf& q) {
    const GfVec3f& i = q.GetImaginary();
    return JPH::Quat(i[0], i[1], i[2], q.GetReal()).Normalized();
}

// The rotation that takes Jolt's +Y (a plane's normal, a capsule's axis) onto a UsdPhysics axis.
JPH::Quat yOnto(UsdPhysicsAxis axis) {
    switch (axis) {
        case UsdPhysicsAxis::X: return JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -JPH::JPH_PI / 2);
        case UsdPhysicsAxis::Z: return JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::JPH_PI / 2);
        default: return JPH::Quat::sIdentity();
    }
}

// --- UsdPhysics -> Jolt, just enough for cubes, spheres and planes ---------------------------

struct Parsed {
    std::vector<UsdPhysicsSceneDesc> scenes;
    std::vector<UsdPhysicsRigidBodyDesc> bodies;
    std::vector<UsdPhysicsCubeShapeDesc> cubes;
    std::vector<UsdPhysicsSphereShapeDesc> spheres;
    std::vector<UsdPhysicsPlaneShapeDesc> planes;
};

template <class T>
void append(std::vector<T>& out, TfSpan<const UsdPhysicsObjectDesc> descs) {
    // The span's elements are really T: the parser reports one type at a time.
    const T* first = static_cast<const T*>(descs.data());
    out.insert(out.end(), first, first + descs.size());
}

Parsed parse(const UsdStageRefPtr& stage) {
    Parsed parsed;
    UsdPhysicsLoadStageFromPrimRange(
        stage, {SdfPath::AbsoluteRootPath()},
        [&parsed](UsdPhysicsObjectType type, TfSpan<const SdfPath>, TfSpan<const UsdPhysicsObjectDesc> descs,
                  const VtValue&) {
            switch (type) {
                case UsdPhysicsObjectType::Scene: append(parsed.scenes, descs); break;
                case UsdPhysicsObjectType::RigidBody: append(parsed.bodies, descs); break;
                case UsdPhysicsObjectType::CubeShape: append(parsed.cubes, descs); break;
                case UsdPhysicsObjectType::SphereShape: append(parsed.spheres, descs); break;
                case UsdPhysicsObjectType::PlaneShape: append(parsed.planes, descs); break;
                default: break;
            }
        },
        VtValue());
    return parsed;
}

struct Simulation {
    World world;
    std::vector<JPH::BodyID> dynamic;  // in the parser's body order
    std::vector<SdfPath> paths;
    double metersPerUnit = 1.0;

    // Everything in metres (§5.1). `origin`, in metres, is subtracted from every position:
    // solving relative to a centre (§7 item 7).
    Simulation(const UsdStageRefPtr& stage, const GfVec3d& origin = GfVec3d(0.0)) {
        metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
        const Parsed parsed = parse(stage);
        const double m = metersPerUnit;
        // §4.3: the world pose at the start frame, in double. The parser's own is a GfVec3f read
        // at Default().
        UsdGeomXformCache xforms(UsdTimeCode(0.0));
        auto worldPose = [&](const SdfPath& path) {
            const GfTransform world(xforms.GetLocalToWorldTransform(stage->GetPrimAtPath(path)));
            const GfVec3d p = world.GetTranslation() * m - origin;
            return std::make_pair(JPH::RVec3(JPH::Real(p[0]), JPH::Real(p[1]), JPH::Real(p[2])),
                                  toJolt(GfQuatf(world.GetRotation().GetQuat())));
        };
        if (!parsed.scenes.empty()) {
            const UsdPhysicsSceneDesc& scene = parsed.scenes.front();
            const GfVec3f g = scene.gravityDirection * scene.gravityMagnitude * static_cast<float>(m);
            world.system.SetGravity(JPH::Vec3(g[0], g[1], g[2]));
        }
        for (const UsdPhysicsPlaneShapeDesc& plane : parsed.planes) {
            JPH::PlaneShapeSettings settings(JPH::Plane(JPH::Vec3::sAxisY(), 0.0f));
            const auto [at, turn] = worldPose(plane.primPath);
            JPH::BodyCreationSettings body(settings.Create().Get(), at, turn * yOnto(plane.axis), JPH::EMotionType::Static,
                                           kStaticLayer);
            world.bodies().CreateAndAddBody(body, JPH::EActivation::DontActivate);
        }
        auto shapeOf = [&](const SdfPath& body) -> JPH::ShapeRefC {
            for (const UsdPhysicsCubeShapeDesc& cube : parsed.cubes) {
                if (cube.rigidBody != body) continue;
                const GfVec3f h = cube.halfExtents * static_cast<float>(m);
                return JPH::BoxShapeSettings(JPH::Vec3(h[0], h[1], h[2])).Create().Get();
            }
            for (const UsdPhysicsSphereShapeDesc& sphere : parsed.spheres) {
                if (sphere.rigidBody != body) continue;
                return JPH::SphereShapeSettings(static_cast<float>(sphere.radius * m)).Create().Get();
            }
            return nullptr;
        };
        for (const UsdPhysicsRigidBodyDesc& desc : parsed.bodies) {
            const JPH::ShapeRefC shape = shapeOf(desc.primPath);
            if (!shape) continue;
            const auto [at, turn] = worldPose(desc.primPath);
            JPH::BodyCreationSettings body(shape, at, turn,
                                           desc.kinematicBody ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic,
                                           kMovingLayer);
            dynamic.push_back(world.bodies().CreateAndAddBody(body, JPH::EActivation::Activate));
            paths.push_back(desc.primPath);
        }
        world.system.OptimizeBroadPhase();
    }

    // Every body's position and rotation, as floats, in body order: what a cache frame holds.
    std::vector<float> poses() {
        std::vector<float> out;
        for (const JPH::BodyID id : dynamic) {
            const JPH::RVec3 p = world.bodies().GetPosition(id);
            const JPH::Quat q = world.bodies().GetRotation(id);
            for (const float v : {float(p.GetX()), float(p.GetY()), float(p.GetZ()), q.GetX(), q.GetY(), q.GetZ(), q.GetW()})
                out.push_back(v);
        }
        return out;
    }
};

constexpr double kFramesPerSecond = 24.0;
constexpr int kSubsteps = 4;

// A ground plane at y = 0 and `count` cubes of 1 m above it, in a stage of the given units.
// The cubes stand in columns of ten, each a little turned, so they land on each other.
UsdStageRefPtr cubesOnPlane(int count, double metersPerUnit = 1.0, const GfVec3d& offsetMeters = GfVec3d(0.0)) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, metersPerUnit);
    const double unit = 1.0 / metersPerUnit;  // one metre, in stage units
    UsdPhysicsScene::Define(stage, SdfPath("/PhysicsScene"));

    UsdGeomPlane ground = UsdGeomPlane::Define(stage, SdfPath("/Ground"));
    ground.CreateAxisAttr().Set(UsdGeomTokens->y);
    ground.AddTranslateOp().Set(offsetMeters * unit);
    UsdPhysicsCollisionAPI::Apply(ground.GetPrim());

    const int columns = std::max(1, count / 10);
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(columns))));
    for (int i = 0; i < count; ++i) {
        const int column = i / 10, level = i % 10;
        UsdGeomCube cube = UsdGeomCube::Define(stage, SdfPath(TfStringPrintf("/Cube_%d", i)));
        cube.CreateSizeAttr().Set(1.0 * unit);
        const GfVec3d at(2.0 * (column % side) + 0.05 * level, 5.0 + 1.5 * level, 2.0 * (column / side));
        cube.AddTranslateOp().Set((at + offsetMeters) * unit);
        cube.AddRotateYOp().Set(static_cast<float>(7 * level));
        UsdPhysicsRigidBodyAPI::Apply(cube.GetPrim());
        UsdPhysicsCollisionAPI::Apply(cube.GetPrim());
    }
    return stage;
}

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool isNear(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

}  // namespace

// §6 phase 0, and §7 item 3: Jolt's Release DLL in this build, with the ABI its headers expect.
USDCC_TEST(physics_spike_jolt_links) {
    initJolt();
    CHECK(JPH::VerifyJoltVersionID(), "the Jolt DLL was built with the same features as its headers here");
    // VerifyJoltVersionID() can't see this one: it is computed before the asserts are switched
    // on, though they change the classes' layout. The Release DLL has them off.
#ifdef JPH_ENABLE_ASSERTS
    CHECK(false, "Jolt's asserts are off in its headers here, as in its Release DLL (JPH_NO_DEBUG)");
#else
    CHECK(true, "Jolt's asserts are off in its headers here, as in its Release DLL");
#endif
}

// §6 phase 0: the stage is parsed by OpenUSD and a cube falls onto a plane, and stays there.
USDCC_TEST(physics_spike_cube_comes_to_rest) {
    Simulation sim(cubesOnPlane(1));
    CHECK(sim.dynamic.size() == 1, "one body parsed (%zu)", sim.dynamic.size());
    if (sim.dynamic.empty()) return;
    for (int frame = 0; frame < 4 * 24; ++frame) sim.world.step(1.0 / kFramesPerSecond, kSubsteps);
    const JPH::RVec3 p = sim.world.bodies().GetPosition(sim.dynamic.front());
    CHECK(isNear(p.GetY(), 0.5, 0.03), "the cube rests on the plane, its centre half a metre up (%.4f)", p.GetY());
    CHECK(isNear(p.GetX(), 0.0, 0.01) && isNear(p.GetZ(), 0.0, 0.01), "and didn't slide (%.4f, %.4f)", p.GetX(), p.GetZ());
    CHECK(!sim.world.bodies().IsActive(sim.dynamic.front()), "it went to sleep");
}

// §6 phase 1's unit test, tried here: a stage in centimetres falls as one in metres does.
USDCC_TEST(physics_spike_units) {
    Simulation metres(cubesOnPlane(1, 1.0));
    Simulation centimetres(cubesOnPlane(1, 0.01));
    for (int frame = 0; frame < 24; ++frame) {
        metres.world.step(1.0 / kFramesPerSecond, kSubsteps);
        centimetres.world.step(1.0 / kFramesPerSecond, kSubsteps);
    }
    const double a = metres.world.bodies().GetPosition(metres.dynamic.front()).GetY();
    const double b = centimetres.world.bodies().GetPosition(centimetres.dynamic.front()).GetY();
    CHECK(isNear(a, b, 1e-4), "after a second, the same height in metres (%.6f and %.6f)", a, b);
}

// §7 item 4: the same build on the same machine gives the same poses on every run, with
// Jolt's worker threads. 200 cubes, so there are contacts between bodies to order.
USDCC_TEST(physics_spike_deterministic) {
    const UsdStageRefPtr stage = cubesOnPlane(200);
    std::vector<float> runs[2];
    for (std::vector<float>& run : runs) {
        Simulation sim(stage);
        for (int frame = 0; frame < 3 * 24; ++frame) {
            sim.world.step(1.0 / kFramesPerSecond, kSubsteps);
            const std::vector<float> frameposes = sim.poses();
            run.insert(run.end(), frameposes.begin(), frameposes.end());
        }
    }
    CHECK(runs[0].size() == runs[1].size() &&
              std::memcmp(runs[0].data(), runs[1].data(), runs[0].size() * sizeof(float)) == 0,
          "two runs give bit-identical poses at every frame (%zu values)", runs[0].size());
}

// §8 open question 1: what a frame costs the solver, for the plan's "a few thousand".
USDCC_TEST(physics_spike_solver_cost) {
    for (const int count : {100, 1000, 3000}) {
        Simulation sim(cubesOnPlane(count));
        double worst = 0.0, total = 0.0;
        const int frames = 3 * 24;
        for (int frame = 0; frame < frames; ++frame) {
            const Clock::time_point start = Clock::now();
            sim.world.step(1.0 / kFramesPerSecond, kSubsteps);
            const double ms = msSince(start);
            worst = std::max(worst, ms);
            total += ms;
        }
        std::printf("    [measure] %d bodies, %d substeps: %.2f ms per frame on average, %.2f ms at worst\n", count,
                    kSubsteps, total / frames, worst);
    }
    CHECK(true, "measured");
}

// §7 item 5: ComputeMassProperties() with our own volume callback matches Jolt's own mass
// for a box and a sphere of uniform density.
USDCC_TEST(physics_spike_mass) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
    UsdGeomCube box = UsdGeomCube::Define(stage, SdfPath("/Box"));
    box.CreateSizeAttr().Set(1.0);
    box.AddScaleOp().Set(GfVec3f(2.0f, 1.0f, 0.5f));
    UsdPhysicsRigidBodyAPI::Apply(box.GetPrim());
    UsdPhysicsCollisionAPI::Apply(box.GetPrim());
    UsdPhysicsMassAPI::Apply(box.GetPrim()).CreateDensityAttr().Set(500.0f);
    UsdGeomSphere ball = UsdGeomSphere::Define(stage, SdfPath("/Ball"));
    ball.CreateRadiusAttr().Set(0.5);
    UsdPhysicsRigidBodyAPI::Apply(ball.GetPrim());
    UsdPhysicsCollisionAPI::Apply(ball.GetPrim());  // no density: the schema's 1000 kg/m³

    // What physics_mass (§5.1) will do, for these two shapes: volume, and inertia at unit density.
    const UsdPhysicsRigidBodyAPI::MassInformationFn massInfo = [](const UsdPrim& prim) {
        UsdPhysicsRigidBodyAPI::MassInformation info{};
        info.localRot = GfQuatf(1.0f);
        const GfVec3d scale = GfTransform(UsdGeomXformable(prim).ComputeLocalToWorldTransform(UsdTimeCode::Default())).GetScale();
        if (const UsdGeomCube cube{prim}) {
            double size = 2.0;
            cube.GetSizeAttr().Get(&size);
            const GfVec3d e(size * scale[0], size * scale[1], size * scale[2]);
            info.volume = static_cast<float>(e[0] * e[1] * e[2]);
            info.inertia = GfMatrix3f(GfVec3f(static_cast<float>(info.volume / 12.0 * (e[1] * e[1] + e[2] * e[2])),
                                              static_cast<float>(info.volume / 12.0 * (e[0] * e[0] + e[2] * e[2])),
                                              static_cast<float>(info.volume / 12.0 * (e[0] * e[0] + e[1] * e[1]))));
        } else if (const UsdGeomSphere sphere{prim}) {
            double radius = 1.0;
            sphere.GetRadiusAttr().Get(&radius);
            radius *= scale[0];
            info.volume = static_cast<float>(4.0 / 3.0 * 3.14159265358979323846 * radius * radius * radius);
            info.inertia = GfMatrix3f(static_cast<float>(0.4 * info.volume * radius * radius));
        } else {
            info.volume = -1.0f;
        }
        return info;
    };

    initJolt();
    struct Case {
        const char* name;
        UsdPrim prim;
        JPH::ShapeRefC shape;
    };
    JPH::BoxShapeSettings boxSettings(JPH::Vec3(1.0f, 0.5f, 0.25f));
    boxSettings.SetDensity(500.0f);
    JPH::SphereShapeSettings ballSettings(0.5f);
    ballSettings.SetDensity(1000.0f);
    for (const Case& c : {Case{"box", box.GetPrim(), boxSettings.Create().Get()},
                          Case{"sphere", ball.GetPrim(), ballSettings.Create().Get()}}) {
        GfVec3f inertia, com;
        GfQuatf axes;
        const float mass = UsdPhysicsRigidBodyAPI(c.prim).ComputeMassProperties(&inertia, &com, &axes, massInfo);
        const JPH::MassProperties jolt = c.shape->GetMassProperties();
        const JPH::Vec3 joltInertia = jolt.mInertia.GetDiagonal3();
        CHECK(isNear(mass, jolt.mMass, jolt.mMass * 1e-4), "%s: the same mass (%.4f and Jolt's %.4f)", c.name, mass, jolt.mMass);
        CHECK(isNear(inertia[0], joltInertia.GetX(), joltInertia.GetX() * 1e-3) &&
                  isNear(inertia[1], joltInertia.GetY(), joltInertia.GetY() * 1e-3) &&
                  isNear(inertia[2], joltInertia.GetZ(), joltInertia.GetZ() * 1e-3),
              "%s: the same inertia (%.4f %.4f %.4f and Jolt's %.4f %.4f %.4f)", c.name, inertia[0], inertia[1], inertia[2],
              joltInertia.GetX(), joltInertia.GetY(), joltInertia.GetZ());
    }
}

// §7 item 6: MoveKinematic() from sampled transforms gives a kinematic body the velocity
// that pushes dynamic bodies, with no step of lag at the first frame.
USDCC_TEST(physics_spike_kinematic) {
    World world;
    world.system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    JPH::PlaneShapeSettings ground(JPH::Plane(JPH::Vec3::sAxisY(), 0.0f));
    world.bodies().CreateAndAddBody(
        JPH::BodyCreationSettings(ground.Create().Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStaticLayer),
        JPH::EActivation::DontActivate);
    JPH::BoxShapeSettings box(JPH::Vec3(0.5f, 0.5f, 0.5f));
    const JPH::BodyID pusher = world.bodies().CreateAndAddBody(
        JPH::BodyCreationSettings(box.Create().Get(), JPH::RVec3(0, 0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, kMovingLayer),
        JPH::EActivation::Activate);
    const JPH::BodyID pushed = world.bodies().CreateAndAddBody(
        JPH::BodyCreationSettings(box.Create().Get(), JPH::RVec3(1.5, 0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, kMovingLayer),
        JPH::EActivation::Activate);
    world.system.OptimizeBroadPhase();

    // The animation: 2 m/s along x, read at each frame's time as a sampled transform would be.
    const double dt = 1.0 / kFramesPerSecond;
    auto animatedX = [](double seconds) { return 2.0 * seconds; };
    world.bodies().MoveKinematic(pusher, JPH::RVec3(animatedX(dt), 0.5, 0), JPH::Quat::sIdentity(), static_cast<float>(dt));
    world.step(dt, kSubsteps);
    const double x1 = world.bodies().GetPosition(pusher).GetX();
    const double v1 = world.bodies().GetLinearVelocity(pusher).GetX();
    CHECK(isNear(x1, animatedX(dt), 1e-5), "after the first frame the kinematic body is where its animation is (%.5f)", x1);
    CHECK(isNear(v1, 2.0, 1e-3), "and moved at the animation's speed during it (%.4f m/s)", v1);
    for (int frame = 2; frame <= 2 * 24; ++frame) {
        world.bodies().MoveKinematic(pusher, JPH::RVec3(animatedX(frame * dt), 0.5, 0), JPH::Quat::sIdentity(),
                                     static_cast<float>(dt));
        world.step(dt, kSubsteps);
    }
    const double pushedX = world.bodies().GetPosition(pushed).GetX();
    CHECK(pushedX > 4.0, "a dynamic box in its way is pushed along ahead of it (to x = %.3f)", pushedX);
}

// §7 item 7: single precision a few kilometres from the origin, against the same scene at
// the origin, and solved relative to a centre near the bodies.
USDCC_TEST(physics_spike_far_from_origin) {
    const GfVec3d distant(5000.0, 0.0, 5000.0);
    // The largest distance, after three seconds, between a body and its twin in the scene at
    // the origin.
    auto worstError = [&](int count, const GfVec3d& solveAround) {
        Simulation home(cubesOnPlane(count));
        Simulation away(cubesOnPlane(count, 1.0, distant), solveAround);
        for (int frame = 0; frame < 3 * 24; ++frame) {
            home.world.step(1.0 / kFramesPerSecond, kSubsteps);
            away.world.step(1.0 / kFramesPerSecond, kSubsteps);
        }
        const GfVec3d shift = distant - solveAround;
        double worst = 0.0;
        for (size_t i = 0; i < home.dynamic.size(); ++i) {
            const JPH::RVec3 h = home.world.bodies().GetPosition(home.dynamic[i]);
            const JPH::RVec3 a = away.world.bodies().GetPosition(away.dynamic[i]) - JPH::RVec3(shift[0], shift[1], shift[2]);
            worst = std::max(worst, double((a - h).Length()));
        }
        return worst;
    };
    // One cube coming to rest: not chaotic, so any difference is precision.
    const double awayError = worstError(1, GfVec3d(0.0));
    const double recentredError = worstError(1, distant);
    // A stack falling over is chaotic: half a millimetre of rounding sends it down another way.
    const double stackError = worstError(30, GfVec3d(0.0));
    const double stackRecentredError = worstError(30, distant);
    // What the parser's own GfVec3f positions lose out there, against the same in double.
    const UsdStageRefPtr farStage = cubesOnPlane(30, 1.0, distant);
    UsdGeomXformCache xforms(UsdTimeCode(0.0));
    double floatError = 0.0;
    for (const UsdPhysicsRigidBodyDesc& body : parse(farStage).bodies) {
        const GfVec3d exact = xforms.GetLocalToWorldTransform(farStage->GetPrimAtPath(body.primPath)).ExtractTranslation();
        floatError = std::max(floatError, (GfVec3d(body.position) - exact).GetLength());
    }
    std::printf("    [measure] at 5 km, against the same scene at the origin: one cube at rest %.6f m off, recentred "
                "%.6f m; a falling stack of 30 up to %.4f m off, recentred %.6f m; the parser's float positions "
                "are off by up to %.6f m\n",
                awayError, recentredError, stackError, stackRecentredError, floatError);
    CHECK(recentredError < 1e-9 && stackRecentredError < 1e-9,
          "solved relative to a centre near them, bodies land exactly as at the origin (%g m, %g m)", recentredError,
          stackRecentredError);
    CHECK(awayError < 0.005, "uncentred, a cube at rest is within 5 mm (%.6f m)", awayError);
}

// §5.3's shape table: a cone as a TaperedCylinderShape, and which scales capsules and
// cylinders take (§7 item 9).
USDCC_TEST(physics_spike_shapes) {
    initJolt();
    const JPH::ShapeSettings::ShapeResult cone = JPH::TaperedCylinderShapeSettings(0.5f, 0.0f, 0.5f).Create();
    const JPH::ShapeSettings::ShapeResult sharpCone = JPH::TaperedCylinderShapeSettings(0.5f, 0.0f, 0.5f, 0.0f).Create();
    std::printf("    [measure] cone, default convex radius: %s; convex radius 0: %s\n",
                cone.HasError() ? cone.GetError().c_str() : "made", sharpCone.HasError() ? sharpCone.GetError().c_str() : "made");
    CHECK(!cone.HasError() || !sharpCone.HasError(), "a TaperedCylinderShape can be a cone");

    const JPH::ShapeRefC capsule = JPH::CapsuleShapeSettings(0.5f, 0.25f).Create().Get();
    const JPH::ShapeRefC cylinder = JPH::CylinderShapeSettings(0.5f, 0.25f).Create().Get();
    for (const JPH::Vec3 scale : {JPH::Vec3(2, 2, 2), JPH::Vec3(1, 2, 1), JPH::Vec3(2, 1, 2), JPH::Vec3(1, 1, 2)}) {
        std::printf("    [measure] scale (%g %g %g): capsule %s, cylinder %s\n", scale.GetX(), scale.GetY(), scale.GetZ(),
                    capsule->IsValidScale(scale) ? "valid" : "invalid", cylinder->IsValidScale(scale) ? "valid" : "invalid");
    }
    CHECK(capsule->IsValidScale(JPH::Vec3(2, 2, 2)) && cylinder->IsValidScale(JPH::Vec3(2, 2, 2)), "a uniform scale is fine for both");
}

namespace {

// The cache layer of §4.2, as SimCache will make it: an anonymous sublayer of the session layer.
SdfLayerRefPtr addCacheLayer(const UsdStageRefPtr& stage) {
    SdfLayerRefPtr cache = SdfLayer::CreateAnonymous("simulation_cache");
    stage->GetSessionLayer()->InsertSubLayerPath(cache->GetIdentifier(), 0);
    return cache;
}

// §4.6: the body's world pose, after `!resetXformStack!`, in three ops. Written with Sdf, as
// SimCache will: inside an SdfChangeBlock the Usd API would read a stale xformOpOrder.
const TfToken kTranslate("xformOp:translate:sim"), kOrient("xformOp:orient:sim"), kScale("xformOp:scale:sim");

void addSimOps(const SdfLayerHandle& cache, const SdfPath& path) {
    const SdfPrimSpecHandle prim = SdfCreatePrimInLayer(cache, path);  // an over
    SdfAttributeSpec::New(prim, UsdGeomTokens->xformOpOrder, SdfValueTypeNames->TokenArray, SdfVariabilityUniform)
        ->SetDefaultValue(VtValue(VtTokenArray{UsdGeomXformOpTypes->resetXformStack, kTranslate, kOrient, kScale}));
    SdfAttributeSpec::New(prim, kTranslate, SdfValueTypeNames->Double3);
    SdfAttributeSpec::New(prim, kOrient, SdfValueTypeNames->Quatf);
    SdfAttributeSpec::New(prim, kScale, SdfValueTypeNames->Float3)->SetDefaultValue(VtValue(GfVec3f(1.0f)));
}

void setPose(const SdfLayerHandle& cache, const SdfPath& path, double time, const GfVec3d& at, const GfQuatf& turn) {
    cache->SetTimeSample(path.AppendProperty(kTranslate), time, at);
    cache->SetTimeSample(path.AppendProperty(kOrient), time, turn);
}

}  // namespace

// §7 item 2, and §4.4's reason for clearing the cache before parsing.
USDCC_TEST(physics_spike_cache_layer) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform parent = UsdGeomXform::Define(stage, SdfPath("/Parent"));
    parent.AddTranslateOp().Set(GfVec3d(10, 0, 0));
    parent.AddRotateYOp().Set(90.0f);
    parent.AddScaleOp().Set(GfVec3f(1.0f, 3.0f, 1.0f));  // rotated, non-uniform: shear below it
    UsdGeomCube body = UsdGeomCube::Define(stage, SdfPath("/Parent/Body"));
    body.AddTranslateOp().Set(GfVec3d(1, 0, 0));
    body.AddRotateZOp().Set(30.0f);
    UsdPhysicsRigidBodyAPI::Apply(body.GetPrim());
    UsdPhysicsCollisionAPI::Apply(body.GetPrim());
    const GfMatrix4d authored = UsdGeomXformCache(UsdTimeCode(0.0)).GetLocalToWorldTransform(body.GetPrim());
    VtTokenArray authoredOrder;
    body.GetXformOpOrderAttr().Get(&authoredOrder);

    const SdfLayerRefPtr cache = addCacheLayer(stage);
    addSimOps(cache, body.GetPath());
    const GfVec3d at0(3, 4, 5), at1(3, 2, 5);
    const GfQuatf turned(GfRotation(GfVec3d(1, 0, 0), 45.0).GetQuat());
    setPose(cache, body.GetPath(), 0.0, at0, GfQuatf(1.0f));
    setPose(cache, body.GetPath(), 1.0, at1, turned);

    const GfMatrix4d world0 = UsdGeomXformCache(UsdTimeCode(0.0)).GetLocalToWorldTransform(body.GetPrim());
    const GfMatrix4d world1 = UsdGeomXformCache(UsdTimeCode(1.0)).GetLocalToWorldTransform(body.GetPrim());
    CHECK(GfIsClose(world0.ExtractTranslation(), at0, 1e-9), "the cache's pose is the world pose, whatever the parent does");
    CHECK(GfIsClose(world1.ExtractTranslation(), at1, 1e-9) &&
              GfIsClose(world1.ExtractRotationQuat().GetImaginary(), GfVec3d(turned.GetImaginary()), 1e-6),
          "at the next frame too, turned");
    const VtValue userOrderValue =
        stage->GetRootLayer()->GetAttributeAtPath(SdfPath("/Parent/Body.xformOpOrder"))->GetDefaultValue();
    const VtTokenArray userOrder = userOrderValue.GetWithDefault<VtTokenArray>();
    CHECK(userOrder == authoredOrder, "the user's layer keeps its own xformOpOrder");

    // The parser reads at Default(): through the cache, a body is where no frame put it.
    const Parsed throughCache = parse(stage);
    CHECK(throughCache.bodies.size() == 1 &&
              GfIsClose(GfVec3d(throughCache.bodies.front().position), GfVec3d(0.0), 1e-6),
          "parsed through the cache, the body is at the origin: why §4.4 clears it first");

    cache->Clear();
    const GfMatrix4d cleared = UsdGeomXformCache(UsdTimeCode(0.0)).GetLocalToWorldTransform(body.GetPrim());
    CHECK(GfIsClose(cleared, authored, 1e-9), "clearing the cache brings the prim's own stack back");
    const Parsed afterClear = parse(stage);
    CHECK(afterClear.bodies.size() == 1 &&
              GfIsClose(GfVec3d(afterClear.bodies.front().position), authored.ExtractTranslation(), 1e-4),
          "and the parser sees the body where it is authored");
}

namespace {

// What Hydra is told, counted: §7 item 1.
class NoticeCounter final : public HdSceneIndexObserver {
public:
    size_t added = 0, removed = 0, dirtied = 0, dirtiedBeyondXform = 0, renamed = 0;
    void reset() { added = removed = dirtied = dirtiedBeyondXform = renamed = 0; }

    void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries& entries) override { added += entries.size(); }
    void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries& entries) override { removed += entries.size(); }
    void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries& entries) override {
        dirtied += entries.size();
        for (const DirtiedPrimEntry& entry : entries) {
            for (const HdDataSourceLocator& locator : entry.dirtyLocators) {
                if (!HdXformSchema::GetDefaultLocator().Intersects(locator)) {
                    ++dirtiedBeyondXform;
                    if (dirtiedBeyondXform <= 3)
                        std::printf("    [measure]   dirtied beyond the transform: %s %s\n", entry.primPath.GetText(),
                                    locator.GetString().c_str());
                    break;
                }
            }
        }
    }
    void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries& entries) override { renamed += entries.size(); }
};

UsdStageRefPtr meshBodies(int count) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const VtVec3fArray points{{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
                              {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
    const VtIntArray counts{4, 4, 4, 4, 4, 4};
    const VtIntArray indices{0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 0, 4, 7, 3};
    for (int i = 0; i < count; ++i) {
        UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(TfStringPrintf("/Body_%d", i)));
        mesh.CreatePointsAttr().Set(points);
        mesh.CreateFaceVertexCountsAttr().Set(counts);
        mesh.CreateFaceVertexIndicesAttr().Set(indices);
        mesh.AddTranslateOp().Set(GfVec3d(i % 32, 0, i / 32));
        UsdPhysicsRigidBodyAPI::Apply(mesh.GetPrim());
    }
    return stage;
}

}  // namespace

// §7 item 1, phase 0's main measurement: writing a frame's samples into the cache for 1,000
// mesh bodies. Is each one only a dirtied transform, or a resync (removed and added again)?
// Measured on UsdImaging's scene indices, which is what the viewport's engine uses in 26.08
// (USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX is on by default). Storm's own cost isn't in it.
USDCC_TEST(physics_spike_hydra_notices) {
    const int count = 1000;
    UsdStageRefPtr stage = meshBodies(count);
    const SdfLayerRefPtr cache = addCacheLayer(stage);

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    UsdImagingSceneIndices indices = UsdImagingCreateSceneIndices(info);
    NoticeCounter counter;
    indices.finalSceneIndex->AddObserver(HdSceneIndexObserverPtr(&counter));
    std::vector<SdfPath> paths;
    for (int i = 0; i < count; ++i) paths.emplace_back(TfStringPrintf("/Body_%d", i));

    auto pull = [&](double time) {
        indices.stageSceneIndex->SetTime(UsdTimeCode(time));
        indices.stageSceneIndex->ApplyPendingUpdates();
        double sum = 0.0;
        for (const SdfPath& path : paths) {
            const HdSceneIndexPrim prim = indices.finalSceneIndex->GetPrim(path);
            if (const HdMatrixDataSourceHandle m = HdXformSchema::GetFromParent(prim.dataSource).GetMatrix())
                sum += m->GetTypedValue(0.0f)[3][1];
        }
        return sum;
    };
    auto pose = [](int i, int frame) { return GfVec3d(i % 32, 5.0 - 0.1 * frame, i / 32); };
    auto report = [&](const char* what, double writeMs, double syncMs) {
        std::printf("    [measure] %s: %zu added, %zu removed, %zu dirtied (%zu beyond the transform); "
                    "write %.2f ms, sync %.2f ms\n",
                    what, counter.added, counter.removed, counter.dirtied, counter.dirtiedBeyondXform, writeMs, syncMs);
    };
    pull(0.0);

    // 1. Creating the ops (what SimCache does once, before the first frame).
    counter.reset();
    Clock::time_point start = Clock::now();
    {
        SdfChangeBlock block;
        for (int i = 0; i < count; ++i) {
            addSimOps(cache, paths[i]);
            setPose(cache, paths[i], 0.0, pose(i, 0), GfQuatf(1.0f));
        }
    }
    double writeMs = msSince(start);
    start = Clock::now();
    pull(0.0);
    report("creating the ops, once", writeMs, msSince(start));

    // 2. Each later frame: one more sample per body. The plan's assumption: no resync.
    size_t addedPerFrame = 0, removedPerFrame = 0, beyondPerFrame = 0;
    double worstWrite = 0.0, worstSync = 0.0;
    for (int frame = 1; frame <= 10; ++frame) {
        counter.reset();
        start = Clock::now();
        {
            SdfChangeBlock block;
            const GfQuatf q(GfRotation(GfVec3d(0, 1, 0), 3.0 * frame).GetQuat());
            for (int i = 0; i < count; ++i) setPose(cache, paths[i], frame, pose(i, frame), q);
        }
        const double w = msSince(start);
        start = Clock::now();
        const double heights = pull(frame);
        const double s = msSince(start);
        worstWrite = std::max(worstWrite, w);
        worstSync = std::max(worstSync, s);
        addedPerFrame = std::max(addedPerFrame, counter.added);
        removedPerFrame = std::max(removedPerFrame, counter.removed);
        beyondPerFrame = std::max(beyondPerFrame, counter.dirtiedBeyondXform);
        if (frame == 1) {
            report("a frame of samples", w, s);
            CHECK(isNear(heights, count * (5.0 - 0.1 * frame), 1e-3 * count), "Hydra sees the new frame's poses");
        }
    }
    std::printf("    [measure] 10 frames of %d bodies: %.2f ms to write at worst, %.2f ms to sync and pull at worst\n",
                count, worstWrite, worstSync);
    CHECK(addedPerFrame == 0 && removedPerFrame == 0,
          "a frame's samples don't resync a prim (at most %zu added, %zu removed in a frame)", addedPerFrame,
          removedPerFrame);
    CHECK(beyondPerFrame == 0, "and dirty only its transform (at most %zu prims dirtied beyond it)", beyondPerFrame);

    // 3. For comparison: the same frame as default values (§7 item 1's fallback).
    counter.reset();
    start = Clock::now();
    {
        SdfChangeBlock block;
        for (int i = 0; i < count; ++i)
            cache->GetAttributeAtPath(paths[i].AppendProperty(kScale))->SetDefaultValue(VtValue(GfVec3f(1.01f)));
    }
    writeMs = msSince(start);
    start = Clock::now();
    pull(10.0);
    report("a frame as default values instead", writeMs, msSince(start));
}
