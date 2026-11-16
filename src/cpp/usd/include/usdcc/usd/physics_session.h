#pragma once

// One stage's simulation (docs/PHYSICS.md §4.3, §4.4, §5.1): the backend's world, the
// cache, where it started and whether the stage has changed since. Stage::physics() makes
// it on first use. It knows no timeline: the PlaybackController (or a script) asks it for
// frames.
//
//   start(frame)   clears the cache, parses the stage at `frame`, builds a world and
//                  caches that frame
//   stepTo(frame)  solves every frame after the last cached one up to `frame`, in order
//   reset()        clears the cache and drops the world
//
// Any change to the stage that isn't the cache's own makes it stale: the cache stays, so
// a look at an attribute doesn't cost the run, but it is never extended again; the next
// start() solves anew. Muting or unmuting the cache layer isn't a change to the stage.
//
// Qt-free.

#include "usdcc/core/signal.h"
#include "usdcc/usd/physics_backend.h"
#include "usdcc/usd/physics_desc.h"
#include "usdcc/usd/sim_cache.h"
#include "usdcc/usd/time_range.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {
class Stage;
struct StageChanges;
}  // namespace usdcc::usd

namespace usdcc::usd::physics {

class PhysicsSession {
public:
    explicit PhysicsSession(Stage& stage);
    ~PhysicsSession();
    PhysicsSession(const PhysicsSession&) = delete;
    PhysicsSession& operator=(const PhysicsSession&) = delete;

    // False, with a report saying why, when there's no backend or it built nothing.
    bool start(double startFrame);
    // False when there's no world to step (never started, stale, or reset).
    bool stepTo(double frame);
    void reset();

    bool hasWorld() const { return m_world != nullptr; }
    bool isStale() const { return m_stale; }
    std::optional<TimeRange> cachedRange() const { return m_cache.range(); }
    double startFrame() const { return m_startFrame; }
    // From the last start(): the parsing's and the backend's.
    const std::vector<PhysicsReport>& reports() const { return m_reports; }
    const SimCache& cache() const { return m_cache; }
    const PhysicsSceneDesc& scene() const { return m_scene; }

    // Steps each frame is cut into (docs/PHYSICS.md §4.3); 4 unless set.
    int substeps() const { return m_substeps; }
    void setSubsteps(int substeps);

    // After the cached range, the stale mark or the reports changed.
    usdcc::core::Signal<> changed;

private:
    void onStageChanged();
    bool cacheMuted() const;

    Stage& m_stage;
    SimCache m_cache;
    PhysicsSceneDesc m_scene;
    std::unique_ptr<PhysicsWorld> m_world;
    std::vector<PhysicsReport> m_reports;
    std::vector<Pose> m_poses;
    double m_startFrame = 0.0;
    double m_lastFrame = 0.0;
    int m_substeps = 4;
    bool m_stale = false;
    bool m_writing = false;     // the changes now are the cache's own
    bool m_cacheMuted = false;  // as last seen, to tell a mute from an edit
    usdcc::core::Signal<const StageChanges&>::Connection m_changedConnection;
};

}  // namespace usdcc::usd::physics
