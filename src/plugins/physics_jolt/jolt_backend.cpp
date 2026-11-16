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
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <pxr/base/gf/rotation.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_set>

namespace usdcc::physics_jolt {

namespace {

using namespace usdcc::usd::physics;
namespace pxr = PXR_NS;
using PXR_NS::SdfPath;

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
        if (!scene.filter.empty()) m_filter = new OwnerFilter(scene.filter);
        auto filtered = [this](JPH::BodyCreationSettings& settings, std::size_t owner) {
            if (m_filter) settings.mCollisionGroup = JPH::CollisionGroup(m_filter, JPH::CollisionGroup::GroupID(owner), 0);
        };

        for (std::size_t s = 0; s < scene.statics.size(); ++s) {
            const ShapeDesc& shape = scene.statics[s];
            const JPH::ShapeRefC made = makeShape(shape, reports);
            if (!made) continue;
            JPH::BodyCreationSettings settings(made, toJoltR(shape.local.position), toJolt(shape.local.rotation) * axisTurn(shape),
                                               JPH::EMotionType::Static, kStaticLayer);
            setMaterial(settings, shape.material);
            filtered(settings, scene.bodies.size() + s);
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
            filtered(settings, i);
            const JPH::EActivation activation =
                body.startsAsleep && !body.kinematic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
            m_ids[i] = bodies.CreateAndAddBody(settings, activation);
            if (m_ids[i].IsInvalid()) reports.push_back({body.prim, "Jolt couldn't add the body: too many bodies"});
        }
        for (const JointDesc& joint : scene.joints) addJoint(scene, joint, reports);
        m_system.OptimizeBroadPhase();
        return true;
    }

    void takeEvents(std::vector<PhysicsReport>& out) override {
        out.insert(out.end(), m_events.begin(), m_events.end());
        m_events.clear();
    }

    void setKinematicTarget(BodyId body, const Pose& pose, double seconds) override {
        if (body >= m_ids.size() || m_ids[body].IsInvalid()) return;
        m_system.GetBodyInterface().MoveKinematic(m_ids[body], toJoltR(pose.position), toJolt(pose.rotation), float(seconds));
    }

    void step(double seconds, int substeps) override {
        substeps = std::max(1, substeps);
        m_system.Update(float(seconds), substeps, &m_temp, &m_jobs);
        breakJoints(seconds / substeps);
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
    // A joint's frame on one side, in the world (relative to the scene's origin).
    struct Frame {
        JPH::RVec3 position;
        JPH::Quat rotation;
        JPH::Vec3 along(Axis axis) const { return rotation * unit(axis); }
    };

    static JPH::Vec3 unit(Axis axis) {
        switch (axis) {
            case Axis::X: return JPH::Vec3::sAxisX();
            case Axis::Y: return JPH::Vec3::sAxisY();
            default: return JPH::Vec3::sAxisZ();
        }
    }
    static Axis perpendicular(Axis axis) {
        switch (axis) {
            case Axis::X: return Axis::Y;
            case Axis::Y: return Axis::Z;
            default: return Axis::X;
        }
    }

    static Frame worldFrame(const PhysicsSceneDesc& scene, const std::optional<BodyId>& body, const Pose& frame) {
        if (!body) return {toJoltR(frame.position), toJolt(frame.rotation)};
        const Pose& pose = scene.bodies[*body].pose;
        const pxr::GfVec3d position = pose.position + pxr::GfRotation(pose.rotation).TransformDir(frame.position);
        return {toJoltR(position), toJolt((pose.rotation * frame.rotation).GetNormalized())};
    }

    // A UsdPhysics drive as a Jolt motor: a spring to a position when it has stiffness, else a
    // velocity motor; an acceleration drive as a frequency, which doesn't depend on mass.
    static JPH::MotorSettings motorFor(const JointDriveDesc& drive, bool angular) {
        JPH::MotorSettings motor;
        const float stiffness = float(std::max(0.0, drive.stiffness));
        const float damping = float(std::max(0.0, drive.damping));
        if (drive.acceleration) {
            const float frequency = std::sqrt(stiffness) / (2.0f * JPH::JPH_PI);
            const float ratio = stiffness > 0.0f ? damping / (2.0f * std::sqrt(stiffness)) : 0.0f;
            motor.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, frequency, ratio);
        } else {
            motor.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, stiffness, damping);
        }
        const float limit = drive.forceLimit > 0.0 ? float(drive.forceLimit) : FLT_MAX;
        if (angular) {
            motor.SetTorqueLimit(limit);
        } else {
            motor.SetForceLimit(limit);
        }
        return motor;
    }
    static JPH::EMotorState motorState(const JointDriveDesc& drive) {
        if (!drive.enabled) return JPH::EMotorState::Off;
        if (drive.stiffness > 0.0) return JPH::EMotorState::Position;
        if (drive.damping > 0.0 || drive.targetVelocity != 0.0) return JPH::EMotorState::Velocity;
        return JPH::EMotorState::Off;
    }

    // A limit Jolt takes as lower <= 0 <= upper; lower > upper locks the axis.
    static std::pair<float, float> centredLimit(const JointLimitDesc& limit, float freeRange, const SdfPath& prim,
                                                std::vector<PhysicsReport>& reports) {
        if (!limit.enabled) return {-freeRange, freeRange};
        if (limit.lower > limit.upper) return {0.0f, 0.0f};
        const float lower = float(limit.lower), upper = float(limit.upper);
        if (lower > 0.0f || upper < 0.0f) {
            reports.push_back({prim, "a limit that excludes where the joint starts: widened to include it, as Jolt needs"});
        }
        return {std::clamp(std::min(lower, 0.0f), -freeRange, 0.0f), std::clamp(std::max(upper, 0.0f), 0.0f, freeRange)};
    }

    void addJoint(const PhysicsSceneDesc& scene, const JointDesc& joint, std::vector<PhysicsReport>& reports) {
        const JPH::BodyID id0 = joint.body0 ? m_ids[*joint.body0] : JPH::BodyID();
        const JPH::BodyID id1 = joint.body1 ? m_ids[*joint.body1] : JPH::BodyID();
        if ((joint.body0 && id0.IsInvalid()) || (joint.body1 && id1.IsInvalid())) {
            reports.push_back({joint.prim, "joins a body Jolt couldn't add: left out"});
            return;
        }
        const Frame f0 = worldFrame(scene, joint.body0, joint.frame0);
        const Frame f1 = worldFrame(scene, joint.body1, joint.frame1);
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        JPH::TwoBodyConstraint* constraint = nullptr;
        auto create = [&](const JPH::TwoBodyConstraintSettings& settings) {
            constraint = bodies.CreateConstraint(&settings, id0, id1);
        };
        switch (joint.kind) {
            case JointKind::Fixed: {
                JPH::FixedConstraintSettings settings;
                settings.mAutoDetectPoint = false;
                settings.mPoint1 = f0.position;
                settings.mAxisX1 = f0.along(Axis::X);
                settings.mAxisY1 = f0.along(Axis::Y);
                settings.mPoint2 = f1.position;
                settings.mAxisX2 = f1.along(Axis::X);
                settings.mAxisY2 = f1.along(Axis::Y);
                create(settings);
                break;
            }
            case JointKind::Revolute: {
                JPH::HingeConstraintSettings settings;
                settings.mPoint1 = f0.position;
                settings.mHingeAxis1 = f0.along(joint.axis);
                settings.mNormalAxis1 = f0.along(perpendicular(joint.axis));
                settings.mPoint2 = f1.position;
                settings.mHingeAxis2 = f1.along(joint.axis);
                settings.mNormalAxis2 = f1.along(perpendicular(joint.axis));
                std::tie(settings.mLimitsMin, settings.mLimitsMax) = centredLimit(joint.limit, JPH::JPH_PI, joint.prim, reports);
                settings.mMotorSettings = motorFor(joint.drive, true);
                create(settings);
                if (auto* hinge = static_cast<JPH::HingeConstraint*>(constraint); hinge && motorState(joint.drive) != JPH::EMotorState::Off) {
                    hinge->SetTargetAngle(float(joint.drive.targetPosition));
                    hinge->SetTargetAngularVelocity(float(joint.drive.targetVelocity));
                    hinge->SetMotorState(motorState(joint.drive));
                }
                break;
            }
            case JointKind::Prismatic: {
                JPH::SliderConstraintSettings settings;
                settings.mAutoDetectPoint = false;
                settings.mPoint1 = f0.position;
                settings.mSliderAxis1 = f0.along(joint.axis);
                settings.mNormalAxis1 = f0.along(perpendicular(joint.axis));
                settings.mPoint2 = f1.position;
                settings.mSliderAxis2 = f1.along(joint.axis);
                settings.mNormalAxis2 = f1.along(perpendicular(joint.axis));
                std::tie(settings.mLimitsMin, settings.mLimitsMax) = centredLimit(joint.limit, FLT_MAX, joint.prim, reports);
                settings.mMotorSettings = motorFor(joint.drive, false);
                create(settings);
                if (auto* slider = static_cast<JPH::SliderConstraint*>(constraint); slider && motorState(joint.drive) != JPH::EMotorState::Off) {
                    slider->SetTargetPosition(float(joint.drive.targetPosition));
                    slider->SetTargetVelocity(float(joint.drive.targetVelocity));
                    slider->SetMotorState(motorState(joint.drive));
                }
                break;
            }
            case JointKind::Spherical: {
                JPH::SwingTwistConstraintSettings settings;
                settings.mPosition1 = f0.position;
                settings.mTwistAxis1 = f0.along(joint.axis);
                settings.mPlaneAxis1 = f0.along(perpendicular(joint.axis));
                settings.mPosition2 = f1.position;
                settings.mTwistAxis2 = f1.along(joint.axis);
                settings.mPlaneAxis2 = f1.along(perpendicular(joint.axis));
                const bool limited = joint.limit.enabled && joint.limit.lower >= 0.0 && joint.limit.upper >= 0.0;
                settings.mNormalHalfConeAngle = limited ? std::min(float(joint.limit.lower), JPH::JPH_PI) : JPH::JPH_PI;
                settings.mPlaneHalfConeAngle = limited ? std::min(float(joint.limit.upper), JPH::JPH_PI) : JPH::JPH_PI;
                settings.mTwistMinAngle = -JPH::JPH_PI;
                settings.mTwistMaxAngle = JPH::JPH_PI;
                create(settings);
                break;
            }
            case JointKind::Distance: {
                JPH::DistanceConstraintSettings settings;
                settings.mPoint1 = f0.position;
                settings.mPoint2 = f1.position;
                settings.mMinDistance = joint.minEnabled ? float(std::max(0.0, joint.limit.lower)) : 0.0f;
                settings.mMaxDistance = joint.maxEnabled ? float(std::max(joint.limit.upper, double(settings.mMinDistance))) : 1.0e6f;
                create(settings);
                break;
            }
            case JointKind::Generic: {
                using EAxis = JPH::SixDOFConstraintSettings::EAxis;
                auto axisOf = [](Dof dof) {
                    switch (dof) {
                        case Dof::TransX: return EAxis::TranslationX;
                        case Dof::TransY: return EAxis::TranslationY;
                        case Dof::TransZ: return EAxis::TranslationZ;
                        case Dof::RotX: return EAxis::RotationX;
                        case Dof::RotY: return EAxis::RotationY;
                        default: return EAxis::RotationZ;
                    }
                };
                auto angular = [](Dof dof) { return dof == Dof::RotX || dof == Dof::RotY || dof == Dof::RotZ; };
                JPH::SixDOFConstraintSettings settings;
                settings.mPosition1 = f0.position;
                settings.mAxisX1 = f0.along(Axis::X);
                settings.mAxisY1 = f0.along(Axis::Y);
                settings.mPosition2 = f1.position;
                settings.mAxisX2 = f1.along(Axis::X);
                settings.mAxisY2 = f1.along(Axis::Y);
                settings.mSwingType = JPH::ESwingType::Pyramid;
                for (int axis = 0; axis < EAxis::Num; ++axis) settings.MakeFreeAxis(EAxis(axis));
                for (const auto& [dof, limit] : joint.limits) {
                    if (!limit.enabled) continue;
                    if (limit.lower > limit.upper) {
                        settings.MakeFixedAxis(axisOf(dof));
                    } else {
                        settings.SetLimitedAxis(axisOf(dof), float(limit.lower), float(limit.upper));
                    }
                }
                for (const auto& [dof, drive] : joint.drives) settings.mMotorSettings[axisOf(dof)] = motorFor(drive, angular(dof));
                create(settings);
                auto* sixDof = static_cast<JPH::SixDOFConstraint*>(constraint);
                if (!sixDof) break;
                JPH::Vec3 position = JPH::Vec3::sZero(), velocity = JPH::Vec3::sZero(), angle = JPH::Vec3::sZero(),
                          spin = JPH::Vec3::sZero();
                for (const auto& [dof, drive] : joint.drives) {
                    const int index = int(axisOf(dof)) % 3;
                    (angular(dof) ? angle : position).SetComponent(index, float(drive.targetPosition));
                    (angular(dof) ? spin : velocity).SetComponent(index, float(drive.targetVelocity));
                }
                sixDof->SetTargetPositionCS(position);
                sixDof->SetTargetVelocityCS(velocity);
                sixDof->SetTargetAngularVelocityCS(spin);
                sixDof->SetTargetOrientationCS(JPH::Quat::sEulerAngles(angle));
                for (const auto& [dof, drive] : joint.drives) sixDof->SetMotorState(axisOf(dof), motorState(drive));
                break;
            }
        }
        if (!constraint) {
            reports.push_back({joint.prim, "Jolt couldn't make this joint"});
            return;
        }
        m_system.AddConstraint(constraint);
        m_joints.push_back({constraint, joint.kind, joint.breakForce, joint.breakTorque, joint.prim, id0, id1});
    }

    // The force and torque a joint held through the last substep, from the impulses Jolt
    // applied in it.
    struct Joint {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        JointKind kind;
        double breakForce;
        double breakTorque;
        SdfPath prim;
        JPH::BodyID body0, body1;
        bool broken = false;
    };

    static std::pair<double, double> impulses(const Joint& joint) {
        JPH::Constraint* c = joint.constraint.GetPtr();
        auto length2 = [](const JPH::Vector<2>& v) { return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1]); };
        switch (joint.kind) {
            case JointKind::Fixed: {
                auto* f = static_cast<JPH::FixedConstraint*>(c);
                return {f->GetTotalLambdaPosition().Length(), f->GetTotalLambdaRotation().Length()};
            }
            case JointKind::Revolute: {
                auto* h = static_cast<JPH::HingeConstraint*>(c);
                return {h->GetTotalLambdaPosition().Length(), length2(h->GetTotalLambdaRotation())};
            }
            case JointKind::Prismatic: {
                auto* s = static_cast<JPH::SliderConstraint*>(c);
                return {length2(s->GetTotalLambdaPosition()), s->GetTotalLambdaRotation().Length()};
            }
            case JointKind::Spherical: {
                auto* s = static_cast<JPH::SwingTwistConstraint*>(c);
                const JPH::Vec3 rotation(s->GetTotalLambdaTwist(), s->GetTotalLambdaSwingY(), s->GetTotalLambdaSwingZ());
                return {s->GetTotalLambdaPosition().Length(), rotation.Length()};
            }
            case JointKind::Distance:
                return {std::abs(static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition()), 0.0};
            case JointKind::Generic: {
                auto* s = static_cast<JPH::SixDOFConstraint*>(c);
                return {s->GetTotalLambdaPosition().Length(), s->GetTotalLambdaRotation().Length()};
            }
        }
        return {0.0, 0.0};
    }

    void breakJoints(double substepSeconds) {
        if (substepSeconds <= 0.0) return;
        for (Joint& joint : m_joints) {
            if (joint.broken || (joint.breakForce <= 0.0 && joint.breakTorque <= 0.0)) continue;
            const auto [impulse, angularImpulse] = impulses(joint);
            const double force = impulse / substepSeconds, torque = angularImpulse / substepSeconds;
            const bool overForce = joint.breakForce > 0.0 && force > joint.breakForce;
            const bool overTorque = joint.breakTorque > 0.0 && torque > joint.breakTorque;
            if (!overForce && !overTorque) continue;
            joint.broken = true;
            joint.constraint->SetEnabled(false);
            JPH::BodyInterface& bodies = m_system.GetBodyInterface();
            if (!joint.body0.IsInvalid()) bodies.ActivateBody(joint.body0);
            if (!joint.body1.IsInvalid()) bodies.ActivateBody(joint.body1);
            char message[160];
            std::snprintf(message, sizeof message, overForce ? "broke under %.0f N" : "broke under %.0f N m",
                          overForce ? force : torque);
            m_events.push_back({joint.prim, message});
        }
    }

    // Which owners (bodies, then static colliders) may touch, from UsdPhysics' collision
    // groups and filtered pairs (§5.3): a GroupFilter of our own, as a collider can be in
    // several groups and Jolt gives a body one.
    class OwnerFilter final : public JPH::GroupFilter {
    public:
        explicit OwnerFilter(CollisionFilterDesc filter) : m_filter(std::move(filter)) {
            for (const auto& [a, b] : m_filter.pairs) m_pairs.insert(key(a, b));
        }
        bool CanCollide(const JPH::CollisionGroup& a, const JPH::CollisionGroup& b) const override {
            const std::size_t x = a.GetGroupID(), y = b.GetGroupID();
            if (m_pairs.count(key(x, y))) return false;
            if (m_filter.groupsCollide.empty() || x >= m_filter.ownerGroups.size() || y >= m_filter.ownerGroups.size()) {
                return true;
            }
            for (const int g : m_filter.ownerGroups[x]) {
                for (const int h : m_filter.ownerGroups[y]) {
                    if (!m_filter.groupsCollide[g][h]) return false;
                }
            }
            return true;
        }

    private:
        static std::uint64_t key(std::size_t a, std::size_t b) {
            if (a > b) std::swap(a, b);
            return (std::uint64_t(a) << 32) | std::uint64_t(b);
        }
        CollisionFilterDesc m_filter;
        std::unordered_set<std::uint64_t> m_pairs;
    };

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
    JPH::Ref<OwnerFilter> m_filter;
    std::vector<Joint> m_joints;
    std::vector<PhysicsReport> m_events;
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
