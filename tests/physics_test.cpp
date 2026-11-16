// Phase 1 of docs/PHYSICS.md: the stage parsed into plain descriptions (physics_desc), the
// mass rules (physics_mass), the cache in a session sublayer (SimCache) and the session
// that drives a backend (PhysicsSession), with a fake backend and with Jolt.
#include "test_harness.h"

#include "jolt_backend.h"
#include "usdcc/usd/physics_backend.h"
#include "usdcc/usd/physics_desc.h"
#include "usdcc/usd/physics_mass.h"
#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/sim_cache.h"
#include "usdcc/usd/stage.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdPhysics/collisionAPI.h>
#include <pxr/usd/usdPhysics/massAPI.h>
#include <pxr/usd/usdPhysics/materialAPI.h>
#include <pxr/usd/usdPhysics/meshCollisionAPI.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/scene.h>
#include <pxr/usd/usdPhysics/tokens.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace usdcc::usd;
using namespace usdcc::usd::physics;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool isNear(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

// The backends the app has, set to `use` for as long as this lives.
struct UseBackend {
    explicit UseBackend(std::shared_ptr<PhysicsBackend> use) : m_before(PhysicsBackends::instance().all()) {
        for (const auto& backend : m_before) PhysicsBackends::instance().remove(backend->id());
        if (use) PhysicsBackends::instance().add(std::move(use));
    }
    ~UseBackend() {
        for (const auto& backend : std::vector(PhysicsBackends::instance().all())) PhysicsBackends::instance().remove(backend->id());
        for (const auto& backend : m_before) PhysicsBackends::instance().add(backend);
    }
    std::vector<std::shared_ptr<PhysicsBackend>> m_before;
};

std::shared_ptr<PhysicsBackend> jolt() {
    static const std::shared_ptr<PhysicsBackend> backend = usdcc::physics_jolt::makeJoltBackend();
    return backend;
}

// Every dynamic body falls a metre a step; kinematic ones go where they're told.
class FakeWorld final : public PhysicsWorld {
public:
    explicit FakeWorld(const PhysicsSceneDesc& scene) {
        for (const BodyDesc& body : scene.bodies) {
            m_poses.push_back(body.pose);
            m_kinematic.push_back(body.kinematic);
        }
    }
    void setKinematicTarget(BodyId body, const Pose& pose, double) override { m_poses[body] = pose; }
    void step(double, int) override {
        ++steps;
        for (size_t i = 0; i < m_poses.size(); ++i) {
            if (!m_kinematic[i]) m_poses[i].position[1] -= 1.0;
        }
    }
    void readPoses(std::vector<Pose>& out) const override { out = m_poses; }
    int steps = 0;

private:
    std::vector<Pose> m_poses;
    std::vector<bool> m_kinematic;
};

class FakeBackend final : public PhysicsBackend {
public:
    std::string id() const override { return "test.fake"; }
    std::string name() const override { return "Fake"; }
    std::unique_ptr<PhysicsWorld> build(const PhysicsSceneDesc& scene, std::vector<PhysicsReport>&) override {
        ++builds;
        lastScene = scene;
        return std::make_unique<FakeWorld>(scene);
    }
    int builds = 0;
    PhysicsSceneDesc lastScene;
};

// A ground plane at y = 0 and a 1 m cube whose centre is `height` metres up, in a stage of
// the given units, with a PhysicsScene.
UsdStageRefPtr cubeOverPlane(double height = 5.0, double metersPerUnit = 1.0) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, metersPerUnit);
    const double unit = 1.0 / metersPerUnit;
    UsdPhysicsScene::Define(stage, SdfPath("/PhysicsScene"));
    UsdGeomPlane ground = UsdGeomPlane::Define(stage, SdfPath("/Ground"));
    ground.CreateAxisAttr().Set(UsdGeomTokens->y);
    UsdPhysicsCollisionAPI::Apply(ground.GetPrim());
    UsdGeomCube cube = UsdGeomCube::Define(stage, SdfPath("/Cube"));
    cube.CreateSizeAttr().Set(1.0 * unit);
    cube.AddTranslateOp().Set(GfVec3d(0, height * unit, 0));
    UsdPhysicsRigidBodyAPI::Apply(cube.GetPrim());
    UsdPhysicsCollisionAPI::Apply(cube.GetPrim());
    return stage;
}

GfVec3d worldPosition(const UsdStageRefPtr& stage, const char* path, double time) {
    return UsdGeomXformCache(UsdTimeCode(time)).GetLocalToWorldTransform(stage->GetPrimAtPath(SdfPath(path))).ExtractTranslation();
}

bool reported(const std::vector<PhysicsReport>& reports, const std::string& part) {
    return std::any_of(reports.begin(), reports.end(), [&part](const PhysicsReport& r) { return r.message.find(part) != std::string::npos; });
}

}  // namespace

// §5.1: a cube over a plane, parsed: a body, a static plane, gravity, and poses in metres
// from the bodies' centre.
USDCC_TEST(physics_desc_parses) {
    std::vector<PhysicsReport> reports;
    const PhysicsSceneDesc scene = parseScene(cubeOverPlane(5.0, 0.01), UsdTimeCode(0.0), reports);
    CHECK(scene.bodies.size() == 1 && scene.statics.size() == 1, "one body, one static collider (%zu, %zu)", scene.bodies.size(),
          scene.statics.size());
    if (scene.bodies.size() != 1) return;
    const BodyDesc& body = scene.bodies.front();
    CHECK(body.shapes.size() == 1 && body.shapes.front().kind == ShapeKind::Box &&
              GfIsClose(body.shapes.front().halfExtents, GfVec3d(0.5), 1e-6),
          "a box collider half a metre each way, in metres though the stage is in centimetres");
    CHECK(GfIsClose(scene.origin + body.pose.position, GfVec3d(0, 5, 0), 1e-9), "five metres up, measured from the origin");
    CHECK(GfIsClose(scene.gravity, GfVec3d(0, -9.81, 0), 1e-4), "Earth's gravity, in m/s² (%g)", scene.gravity[1]);
    CHECK(isNear(body.mass, 1000.0, 1e-2), "a cubic metre at the schema's 1000 kg/m³ (%g kg)", body.mass);
    CHECK(reports.empty(), "nothing to report (%zu)", reports.size());
}

// §4.3: the pose at the start frame, not the parser's default-time one.
USDCC_TEST(physics_desc_start_frame) {
    UsdStageRefPtr stage = cubeOverPlane();
    UsdGeomCube cube(stage->GetPrimAtPath(SdfPath("/Cube")));
    cube.ClearXformOpOrder();
    UsdGeomXformOp op = cube.AddTranslateOp(UsdGeomXformOp::PrecisionDouble, TfToken("anim"));
    op.Set(GfVec3d(0, 5, 0), UsdTimeCode(10.0));
    op.Set(GfVec3d(0, 15, 0), UsdTimeCode(20.0));
    std::vector<PhysicsReport> reports;
    const PhysicsSceneDesc scene = parseScene(stage, UsdTimeCode(20.0), reports);
    CHECK(scene.bodies.size() == 1 && GfIsClose(scene.origin + scene.bodies.front().pose.position, GfVec3d(0, 15, 0), 1e-9),
          "a body animated only by samples starts where frame 20 has it");
}

// §5.1, §7 item 5: the schema's mass precedence, through our volumes.
USDCC_TEST(physics_mass_precedence) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
    auto body = [&stage](const char* path) {
        UsdGeomCube cube = UsdGeomCube::Define(stage, SdfPath(path));
        cube.CreateSizeAttr().Set(1.0);
        UsdPhysicsRigidBodyAPI::Apply(cube.GetPrim());
        UsdPhysicsCollisionAPI::Apply(cube.GetPrim());
        return cube.GetPrim();
    };
    UsdPhysicsMassAPI::Apply(body("/Explicit")).CreateMassAttr().Set(7.0f);
    UsdPhysicsMassAPI explicitAndDense = UsdPhysicsMassAPI::Apply(body("/ExplicitWins"));
    explicitAndDense.CreateMassAttr().Set(3.0f);
    explicitAndDense.CreateDensityAttr().Set(500.0f);
    UsdPhysicsMassAPI::Apply(body("/Density")).CreateDensityAttr().Set(500.0f);
    const UsdShadeMaterial material = UsdShadeMaterial::Define(stage, SdfPath("/Light"));
    UsdPhysicsMaterialAPI::Apply(material.GetPrim()).CreateDensityAttr().Set(200.0f);
    UsdShadeMaterialBindingAPI::Apply(body("/Material")).Bind(material, UsdShadeTokens->fallbackStrength, TfToken("physics"));
    body("/Default");

    std::vector<PhysicsReport> reports;
    const PhysicsSceneDesc scene = parseScene(stage, UsdTimeCode(0.0), reports);
    auto massOf = [&scene](const char* path) {
        for (const BodyDesc& b : scene.bodies)
            if (b.prim == SdfPath(path)) return b.mass;
        return -1.0;
    };
    CHECK(isNear(massOf("/Explicit"), 7.0, 1e-4), "an explicit mass (%g)", massOf("/Explicit"));
    CHECK(isNear(massOf("/ExplicitWins"), 3.0, 1e-4), "wins over a density (%g)", massOf("/ExplicitWins"));
    CHECK(isNear(massOf("/Density"), 500.0, 1e-2), "then the body's density (%g)", massOf("/Density"));
    CHECK(isNear(massOf("/Material"), 200.0, 1e-2), "then its material's (%g)", massOf("/Material"));
    CHECK(isNear(massOf("/Default"), 1000.0, 1e-2), "then 1000 kg/m³ (%g)", massOf("/Default"));
}

// physics_mass: hulls and meshes against the closed forms.
USDCC_TEST(physics_mass_shapes) {
    ShapeDesc box;
    box.kind = ShapeKind::Box;
    box.halfExtents = GfVec3d(1.0, 0.5, 0.25);
    const ShapeMass exact = shapeMass(box);
    ShapeDesc hull;
    hull.kind = ShapeKind::ConvexHull;
    for (int i = 0; i < 8; ++i) {
        hull.points.emplace_back(i & 1 ? 1.0f : -1.0f, i & 2 ? 0.5f : -0.5f, i & 4 ? 0.25f : -0.25f);
    }
    hull.points.emplace_back(0.1f, 0.0f, 0.0f);  // inside: no difference
    const ShapeMass fromHull = shapeMass(hull);
    CHECK(isNear(fromHull.volume, exact.volume, 1e-9), "a box's corners as a hull: its volume (%g, %g)", fromHull.volume, exact.volume);
    bool same = true;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) same = same && isNear(fromHull.inertia[r][c], exact.inertia[r][c], 1e-9);
    CHECK(same && GfIsClose(fromHull.centerOfMass, GfVec3d(0.0), 1e-9), "and its inertia, about its centre");
    CHECK(convexHullTriangles(hull.points).size() == 12 * 3, "twelve triangles: the inside point isn't on it");
}

// §4.8: a body inside an instance is reported, not simulated.
USDCC_TEST(physics_desc_instances) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform prototype = UsdGeomXform::Define(stage, SdfPath("/Prototype"));
    UsdGeomCube cube = UsdGeomCube::Define(stage, SdfPath("/Prototype/Cube"));
    UsdPhysicsRigidBodyAPI::Apply(cube.GetPrim());
    UsdPhysicsCollisionAPI::Apply(cube.GetPrim());
    UsdPrim instance = UsdGeomXform::Define(stage, SdfPath("/Instance")).GetPrim();
    instance.GetReferences().AddInternalReference(SdfPath("/Prototype"));
    instance.SetInstanceable(true);
    std::vector<PhysicsReport> reports;
    const PhysicsSceneDesc scene = parseScene(stage, UsdTimeCode(0.0), reports);
    const bool proxySimulated = std::any_of(scene.bodies.begin(), scene.bodies.end(),
                                            [](const BodyDesc& b) { return b.prim == SdfPath("/Instance/Cube"); });
    CHECK(!proxySimulated, "the body inside the instance isn't simulated");
    CHECK(std::any_of(reports.begin(), reports.end(), [](const PhysicsReport& r) { return r.prim == SdfPath("/Instance"); }),
          "the instance is reported");
}

// §4.2, §4.4: the session with a fake backend: the cache, its range, and staleness.
USDCC_TEST(physics_session_fake_backend) {
    auto fake = std::make_shared<FakeBackend>();
    UseBackend use(fake);
    Stage stage(cubeOverPlane(10.0));
    PhysicsSession& session = stage.physics();
    int changes = 0;
    auto connection = session.changed.connect([&changes]() { ++changes; });

    CHECK(session.start(1.0), "starts");
    const SdfLayerHandle cache = session.cache().layer();
    const std::vector<std::string> sublayers = stage.usdStage()->GetSessionLayer()->GetSubLayerPaths();
    CHECK(cache && SimCache::isCacheLayer(cache) && !sublayers.empty() && sublayers.front() == cache->GetIdentifier(),
          "the cache is the session layer's first sublayer");
    CHECK(session.cachedRange() && session.cachedRange()->start == 1.0 && session.cachedRange()->end == 1.0,
          "holding the start frame");
    CHECK(session.stepTo(4.0) && session.cachedRange()->end == 4.0, "three more frames, in order");
    CHECK(isNear(worldPosition(stage.usdStage(), "/Cube", 4.0)[1], 7.0, 1e-9), "the composed stage shows them (%g)",
          worldPosition(stage.usdStage(), "/Cube", 4.0)[1]);
    CHECK(isNear(worldPosition(stage.usdStage(), "/Cube", 2.5)[1], 8.5, 1e-9), "and between them");
    CHECK(isNear(worldPosition(stage.usdStage(), "/Cube", 9.0)[1], 7.0, 1e-9), "past the last frame: held there");
    CHECK(!session.isStale(), "its own writes don't make it stale");
    CHECK(stage.editTargetLayer() == stage.usdStage()->GetRootLayer() && !stage.setEditTargetLayer(cache),
          "the cache can't be made the edit target");

    // Muting the cache shows the stage as authored, and isn't an edit.
    stage.usdStage()->MuteLayer(cache->GetIdentifier());
    CHECK(isNear(worldPosition(stage.usdStage(), "/Cube", 4.0)[1], 10.0, 1e-9) && !session.isStale(),
          "muted: the cube where it's authored, the cache not stale");
    stage.usdStage()->UnmuteLayer(cache->GetIdentifier());
    CHECK(!session.isStale() && isNear(worldPosition(stage.usdStage(), "/Cube", 4.0)[1], 7.0, 1e-9), "unmuted: back");

    // An edit makes it stale; it stays, but isn't extended.
    UsdGeomCube(stage.usdStage()->GetPrimAtPath(SdfPath("/Cube"))).GetSizeAttr().Set(2.0);
    CHECK(session.isStale() && session.cachedRange() && !session.stepTo(6.0), "an edit: stale, kept, not extended");

    // Solving anew parses the stage as authored, not through the cache.
    CHECK(session.start(1.0) && !session.isStale(), "started again");
    CHECK(isNear(fake->lastScene.origin[1] + fake->lastScene.bodies.front().pose.position[1], 10.0, 1e-9),
          "from the authored pose, not the cached one");
    session.reset();
    CHECK(!session.cachedRange() && !session.hasWorld() && isNear(worldPosition(stage.usdStage(), "/Cube", 4.0)[1], 10.0, 1e-9),
          "reset: nothing cached, the cube where it's authored");
    CHECK(changes >= 4, "every change announced (%d)", changes);
}

// §6 phase 1: the user's layers are untouched and not dirty after a run.
USDCC_TEST(physics_session_leaves_user_layers) {
    UseBackend use(jolt());
    QTemporaryDir folder;
    const std::string file = QDir(folder.path()).filePath("drop.usda").toStdString();
    cubeOverPlane()->GetRootLayer()->Export(file);
    Stage stage(UsdStage::Open(file));
    std::string before;
    stage.usdStage()->GetRootLayer()->ExportToString(&before);
    CHECK(stage.physics().start(0.0) && stage.physics().stepTo(24.0), "a second simulated");
    std::string after;
    stage.usdStage()->GetRootLayer()->ExportToString(&after);
    CHECK(after == before && !stage.usdStage()->GetRootLayer()->IsDirty(), "the file's layer unchanged and not dirty");
}

// Jolt through the session: a cube falls and rests on the plane, in centimetres as in metres.
USDCC_TEST(physics_jolt_cube_rests) {
    UseBackend use(jolt());
    for (const double metersPerUnit : {1.0, 0.01}) {
        Stage stage(cubeOverPlane(5.0, metersPerUnit));
        PhysicsSession& session = stage.physics();
        CHECK(session.start(0.0) && session.stepTo(96.0), "four seconds simulated");
        const double heightMetres = worldPosition(stage.usdStage(), "/Cube", 96.0)[1] * metersPerUnit;
        CHECK(isNear(heightMetres, 0.5, 0.03), "%g m per unit: at rest, its centre half a metre up (%.4f m)", metersPerUnit,
              heightMetres);
        const double halfway = worldPosition(stage.usdStage(), "/Cube", 12.0)[1] * metersPerUnit;
        CHECK(isNear(halfway, 5.0 - 0.5 * 9.81 * 0.25, 0.05), "half a second in, it has fallen as anything does (%.4f m)", halfway);
        CHECK(session.reports().empty(), "nothing to report (%zu)", session.reports().size());
    }
}

// Jolt: a kinematic body, moved by its own animation, pushes a dynamic one (§7 item 6).
USDCC_TEST(physics_jolt_kinematic_pushes) {
    UseBackend use(jolt());
    UsdStageRefPtr usd = cubeOverPlane(0.5);
    UsdGeomCube pusher = UsdGeomCube::Define(usd, SdfPath("/Pusher"));
    pusher.CreateSizeAttr().Set(1.0);
    UsdGeomXformOp op = pusher.AddTranslateOp();
    op.Set(GfVec3d(-2, 0.5, 0), UsdTimeCode(0.0));
    op.Set(GfVec3d(2, 0.5, 0), UsdTimeCode(48.0));
    UsdPhysicsRigidBodyAPI pusherBody = UsdPhysicsRigidBodyAPI::Apply(pusher.GetPrim());
    pusherBody.CreateKinematicEnabledAttr().Set(true);
    UsdPhysicsCollisionAPI::Apply(pusher.GetPrim());
    Stage stage(usd);
    CHECK(stage.physics().start(0.0) && stage.physics().stepTo(48.0), "two seconds simulated");
    const double x = worldPosition(usd, "/Cube", 48.0)[0];
    CHECK(x > 2.0, "the cube in its way is pushed ahead of it (to x = %.3f)", x);
    CHECK(isNear(worldPosition(usd, "/Pusher", 48.0)[0], 2.0, 1e-9), "and the kinematic body keeps its own animation");
}

// §4.6: a body under a rotated parent with non-uniform scale (shear) is written as one
// matrix, and its world transform at the start frame is what was authored.
USDCC_TEST(physics_cache_sheared_body) {
    auto fake = std::make_shared<FakeBackend>();
    UseBackend use(fake);
    UsdStageRefPtr usd = cubeOverPlane();
    UsdGeomXform parent = UsdGeomXform::Define(usd, SdfPath("/Parent"));
    parent.AddRotateYOp().Set(30.0f);
    parent.AddScaleOp().Set(GfVec3f(1.0f, 3.0f, 1.0f));
    UsdGeomCube child = UsdGeomCube::Define(usd, SdfPath("/Parent/Child"));
    child.AddTranslateOp().Set(GfVec3d(1, 2, 0));
    child.AddRotateZOp().Set(40.0f);
    UsdPhysicsRigidBodyAPI::Apply(child.GetPrim());
    UsdPhysicsCollisionAPI::Apply(child.GetPrim());
    const GfMatrix4d authored = UsdGeomXformCache(UsdTimeCode(0.0)).GetLocalToWorldTransform(child.GetPrim());
    Stage stage(usd);
    CHECK(stage.physics().start(0.0), "starts");
    const GfMatrix4d cached = UsdGeomXformCache(UsdTimeCode(0.0)).GetLocalToWorldTransform(child.GetPrim());
    CHECK(GfIsClose(cached, authored, 1e-6), "at the start frame, the cache puts it exactly where it was");
    const SdfLayerHandle cache = stage.physics().cache().layer();
    CHECK(cache->GetAttributeAtPath(SdfPath("/Parent/Child.xformOp:transform:sim")) &&
              cache->GetAttributeAtPath(SdfPath("/Cube.xformOp:translate:sim")),
          "the sheared one as a matrix, the plain one as translate, orient and scale");
}
