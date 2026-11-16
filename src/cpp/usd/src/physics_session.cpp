#include "usdcc/usd/physics_session.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformCache.h>

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::physics {

namespace {
// While alive, the stage's changes are the cache's own.
struct Writing {
    explicit Writing(bool& flag) : m_flag(flag), m_was(flag) { m_flag = true; }
    ~Writing() { m_flag = m_was; }
    bool& m_flag;
    bool m_was;
};
}  // namespace

PhysicsSession::PhysicsSession(Stage& stage) : m_stage(stage), m_cache(stage.usdStage()) {
    m_changedConnection = stage.changed.connect([this](const StageChanges&) { onStageChanged(); });
}

PhysicsSession::~PhysicsSession() = default;

void PhysicsSession::setSubsteps(int substeps) { m_substeps = std::clamp(substeps, 1, 64); }

bool PhysicsSession::cacheMuted() const {
    const SdfLayerHandle layer = m_cache.layer();
    return layer && m_stage.usdStage()->IsLayerMuted(layer->GetIdentifier());
}

void PhysicsSession::onStageChanged() {
    if (m_writing) return;
    // Muting the cache to see the scene as authored, or unmuting it, isn't an edit.
    const bool muted = cacheMuted();
    if (muted != m_cacheMuted) {
        m_cacheMuted = muted;
        return;
    }
    if (!m_cache.range() || m_stale) return;
    m_stale = true;
    m_world.reset();
    changed();
}

bool PhysicsSession::start(double startFrame) {
    {
        Writing writing(m_writing);
        // The parser must see the stage as authored (§4.4).
        m_cache.clear();
    }
    m_world.reset();
    m_stale = false;
    m_reports.clear();
    m_startFrame = startFrame;
    m_lastFrame = startFrame;

    m_scene = parseScene(m_stage.usdStage(), UsdTimeCode(startFrame), m_reports);
    const std::shared_ptr<PhysicsBackend> backend = PhysicsBackends::instance().current();
    if (!backend) {
        m_reports.push_back({SdfPath::AbsoluteRootPath(), "no physics backend: is the Jolt Physics plugin enabled?"});
    } else {
        m_world = backend->build(m_scene, m_reports);
    }
    for (const PhysicsReport& report : m_reports) {
        USDCC_LOG_WARNING("usdcc.physics", "{}: {}", report.prim.GetString(), report.message);
    }
    if (!m_world) {
        changed();
        return false;
    }
    {
        Writing writing(m_writing);
        m_cache.prepare(m_scene);
        m_world->readPoses(m_poses);
        m_cache.write(startFrame, m_scene, m_poses);
    }
    m_cacheMuted = cacheMuted();
    USDCC_LOG_INFO("usdcc.physics", "Simulating {} bodies with {}, from frame {}", m_scene.bodies.size(),
                   backend->name(), startFrame);
    changed();
    return true;
}

bool PhysicsSession::stepTo(double frame) {
    if (!m_world || m_stale) return false;
    const double seconds = 1.0 / timeCodesPerSecond(m_stage.usdStage());
    bool stepped = false;
    while (m_lastFrame + 1.0 <= frame + 1e-9) {
        const double next = m_lastFrame + 1.0;
        // Kinematic bodies go where their own animation has them at the end of the step.
        UsdGeomXformCache xforms{UsdTimeCode(next)};
        for (BodyId id = 0; id < m_scene.bodies.size(); ++id) {
            const BodyDesc& body = m_scene.bodies[id];
            if (!body.kinematic) continue;
            const UsdPrim prim = m_stage.usdStage()->GetPrimAtPath(body.prim);
            if (prim) m_world->setKinematicTarget(id, poseFromWorld(m_scene, xforms.GetLocalToWorldTransform(prim)), seconds);
        }
        m_world->step(seconds, m_substeps);
        m_world->readPoses(m_poses);
        {
            Writing writing(m_writing);
            m_cache.write(next, m_scene, m_poses);
        }
        m_lastFrame = next;
        stepped = true;
    }
    if (stepped) changed();
    return true;
}

void PhysicsSession::reset() {
    {
        Writing writing(m_writing);
        m_cache.clear();
    }
    m_world.reset();
    m_stale = false;
    m_lastFrame = m_startFrame;
    changed();
}

}  // namespace usdcc::usd::physics
