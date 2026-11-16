#pragma once

// A physics solver behind an interface (docs/PHYSICS.md §4.1, §5.2): the app parses the
// stage (physics_desc.h), hands a backend plain descriptions, steps it and reads poses
// back. No backend touches a UsdStage, and no solver's types reach usdcc's headers. Jolt
// is one, in the bundled physics_jolt plugin, which registers it through
// Host::addPhysicsBackend().
//
// Qt-free.

#include "usdcc/core/signal.h"
#include "usdcc/usd/physics_desc.h"

#include <memory>
#include <string>
#include <vector>

namespace usdcc::usd::physics {

// One simulation of one stage. Built from the start frame, stepped forward only; a
// changed stage gets a new one (§5.2), nothing is patched in place.
class PhysicsWorld {
public:
    virtual ~PhysicsWorld() = default;
    // Where a kinematic body has to be at the end of the next step of `seconds`.
    virtual void setKinematicTarget(BodyId body, const Pose& pose, double seconds) = 0;
    virtual void step(double seconds, int substeps) = 0;
    // Every body's pose, in BodyId order (kinematic ones too).
    virtual void readPoses(std::vector<Pose>& out) const = 0;
    // What happened in the steps since the last call that's worth saying: a joint that
    // broke (§5.3). Appended to `out`.
    virtual void takeEvents(std::vector<PhysicsReport>& out) { (void)out; }
};

class PhysicsBackend {
public:
    virtual ~PhysicsBackend() = default;
    virtual std::string id() const = 0;    // "org.usdcc.physics-jolt"
    virtual std::string name() const = 0;  // "Jolt"
    // Null when it can't; what it leaves out or changes goes to `reports` (§4.8).
    virtual std::unique_ptr<PhysicsWorld> build(const PhysicsSceneDesc& scene, std::vector<PhysicsReport>& reports) = 0;
};

// The backends the app has. The first one added is the one used.
class PhysicsBackends {
public:
    static PhysicsBackends& instance();

    // Replaces one with the same id.
    void add(std::shared_ptr<PhysicsBackend> backend);
    void remove(const std::string& id);
    std::shared_ptr<PhysicsBackend> current() const;
    const std::vector<std::shared_ptr<PhysicsBackend>>& all() const { return m_backends; }

    usdcc::core::Signal<> changed;

private:
    std::vector<std::shared_ptr<PhysicsBackend>> m_backends;
};

}  // namespace usdcc::usd::physics
