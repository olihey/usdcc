#include "usdcc/usd/physics_overlay.h"

#include "usdcc/usd/physics_desc.h"
#include "usdcc/usd/physics_mass.h"

#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

using physics::Axis;
using physics::BodyDesc;
using physics::JointDesc;
using physics::Pose;
using physics::ShapeDesc;
using physics::ShapeKind;
using usdcc::tools::GizmoGeometry;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxMeshLines = 20000;  // a triangle mesh's edges, at most
const GfVec4f kDynamic(0.35f, 0.9f, 0.45f, 1.0f);
const GfVec4f kKinematic(0.35f, 0.6f, 1.0f, 1.0f);
const GfVec4f kStatic(0.95f, 0.6f, 0.2f, 1.0f);
const GfVec4f kJointLink(0.95f, 0.9f, 0.3f, 1.0f);

GfVec3d unit(Axis axis) {
    switch (axis) {
        case Axis::X: return GfVec3d(1, 0, 0);
        case Axis::Y: return GfVec3d(0, 1, 0);
        default: return GfVec3d(0, 0, 1);
    }
}

// Lines in a shape's own frame, in metres, then placed in the stage's world.
struct Lines {
    std::vector<std::pair<GfVec3d, GfVec3d>> segments;

    void add(const GfVec3d& a, const GfVec3d& b) { segments.emplace_back(a, b); }
    // An arc round `centre` in the plane of `u` and `v`, from angle `from` to `to`.
    void arc(const GfVec3d& centre, const GfVec3d& u, const GfVec3d& v, double radius, double from, double to, int steps) {
        GfVec3d previous = centre + radius * (std::cos(from) * u + std::sin(from) * v);
        for (int i = 1; i <= steps; ++i) {
            const double angle = from + (to - from) * i / steps;
            const GfVec3d point = centre + radius * (std::cos(angle) * u + std::sin(angle) * v);
            add(previous, point);
            previous = point;
        }
    }
    void circle(const GfVec3d& centre, const GfVec3d& u, const GfVec3d& v, double radius) {
        arc(centre, u, v, radius, 0.0, 2.0 * kPi, 24);
    }
};

Lines shapeLines(const ShapeDesc& shape) {
    Lines lines;
    const GfVec3d a = unit(shape.axis);
    const GfVec3d u = unit(shape.axis == Axis::X ? Axis::Y : (shape.axis == Axis::Y ? Axis::Z : Axis::X));
    const GfVec3d v = GfCross(a, u);
    switch (shape.kind) {
        case ShapeKind::Box: {
            const GfVec3d& h = shape.halfExtents;
            auto corner = [&h](int i) { return GfVec3d(i & 1 ? h[0] : -h[0], i & 2 ? h[1] : -h[1], i & 4 ? h[2] : -h[2]); };
            for (int i = 0; i < 8; ++i) {
                for (int bit : {1, 2, 4}) {
                    if (!(i & bit)) lines.add(corner(i), corner(i | bit));
                }
            }
            break;
        }
        case ShapeKind::Sphere:
            lines.circle(GfVec3d(0.0), GfVec3d(1, 0, 0), GfVec3d(0, 1, 0), shape.radius);
            lines.circle(GfVec3d(0.0), GfVec3d(0, 1, 0), GfVec3d(0, 0, 1), shape.radius);
            lines.circle(GfVec3d(0.0), GfVec3d(0, 0, 1), GfVec3d(1, 0, 0), shape.radius);
            break;
        case ShapeKind::Cylinder:
        case ShapeKind::Capsule: {
            const double r = shape.radius, h = shape.halfHeight;
            lines.circle(a * h, u, v, r);
            lines.circle(-a * h, u, v, r);
            for (const GfVec3d& side : {u, -u, v, -v}) lines.add(side * r + a * h, side * r - a * h);
            if (shape.kind == ShapeKind::Capsule) {
                for (const GfVec3d& across : {u, v}) {
                    lines.arc(a * h, across, a, r, 0.0, kPi, 12);
                    lines.arc(-a * h, across, -a, r, 0.0, kPi, 12);
                }
            }
            break;
        }
        case ShapeKind::Cone: {
            const double r = shape.radius, h = shape.halfHeight;
            lines.circle(-a * h, u, v, r);
            for (const GfVec3d& side : {u, -u, v, -v}) lines.add(side * r - a * h, a * h);
            break;
        }
        case ShapeKind::Plane: {
            const double half = 10.0;  // 20 m of an infinite plane
            const GfVec3d corners[] = {(u + v) * half, (u - v) * half, (-u - v) * half, (-u + v) * half};
            for (int i = 0; i < 4; ++i) lines.add(corners[i], corners[(i + 1) % 4]);
            lines.add(GfVec3d(0.0), a);  // its normal, a metre long
            break;
        }
        case ShapeKind::ConvexHull:
        case ShapeKind::TriangleMesh: {
            const std::vector<int> triangles =
                shape.kind == ShapeKind::ConvexHull ? physics::convexHullTriangles(shape.points) : shape.triangles;
            std::set<std::pair<int, int>> edges;
            for (size_t i = 0; i + 2 < triangles.size() && edges.size() < kMaxMeshLines; i += 3) {
                for (int k = 0; k < 3; ++k) {
                    const int p = triangles[i + k], q = triangles[i + (k + 1) % 3];
                    edges.insert({std::min(p, q), std::max(p, q)});
                }
            }
            for (const auto& [p, q] : edges) lines.add(GfVec3d(shape.points[p]), GfVec3d(shape.points[q]));
            break;
        }
    }
    return lines;
}

struct Placer {
    const physics::PhysicsSceneDesc& scene;
    GizmoGeometry& out;

    // A point in metres, relative to the scene's origin, in the stage's world.
    GfVec3d toStage(const GfVec3d& metres) const { return physics::toStagePosition(scene, metres); }

    // `lines` in `frame`, itself in `parent` (a body's pose), or in the scene's for none.
    void place(const Lines& lines, const Pose& frame, const Pose* parent, const GfVec4f& colour) {
        const GfRotation shapeRotation(frame.rotation);
        const GfRotation bodyRotation(parent ? parent->rotation : GfQuatd(1.0));
        const GfVec3d bodyPosition = parent ? parent->position : GfVec3d(0.0);
        auto world = [&](const GfVec3d& p) {
            return toStage(bodyPosition + bodyRotation.TransformDir(frame.position + shapeRotation.TransformDir(p)));
        };
        for (const auto& [a, b] : lines.segments) out.lines.push_back({world(a), world(b), colour});
    }

    // A joint's frame on one side: three short axes, red, green, blue.
    GfVec3d triad(const Pose& frame, const std::optional<physics::BodyId>& body) {
        const Pose* parent = body ? &scene.bodies[*body].pose : nullptr;
        const double length = 0.15;
        const GfVec4f colours[] = {GfVec4f(1.0f, 0.3f, 0.3f, 1.0f), GfVec4f(0.3f, 1.0f, 0.3f, 1.0f),
                                   GfVec4f(0.3f, 0.5f, 1.0f, 1.0f)};
        for (int k = 0; k < 3; ++k) {
            Lines axis;
            axis.add(GfVec3d(0.0), unit(Axis(k)) * length);
            place(axis, frame, parent, colours[k]);
        }
        // Where it is, for the line to the other side's frame.
        const GfRotation bodyRotation(parent ? parent->rotation : GfQuatd(1.0));
        const GfVec3d bodyPosition = parent ? parent->position : GfVec3d(0.0);
        return toStage(bodyPosition + bodyRotation.TransformDir(frame.position));
    }
};

}  // namespace

GizmoGeometry buildPhysicsOverlay(const UsdStageRefPtr& stage, UsdTimeCode time) {
    GizmoGeometry geometry;
    if (!stage) return geometry;
    std::vector<physics::PhysicsReport> reports;
    const physics::PhysicsSceneDesc scene = physics::parseScene(stage, time, reports);
    Placer placer{scene, geometry};
    for (const ShapeDesc& shape : scene.statics) placer.place(shapeLines(shape), shape.local, nullptr, kStatic);
    for (const BodyDesc& body : scene.bodies) {
        for (const ShapeDesc& shape : body.shapes) {
            placer.place(shapeLines(shape), shape.local, &body.pose, body.kinematic ? kKinematic : kDynamic);
        }
    }
    for (const JointDesc& joint : scene.joints) {
        const GfVec3d a = placer.triad(joint.frame0, joint.body0);
        const GfVec3d b = placer.triad(joint.frame1, joint.body1);
        if ((a - b).GetLength() > 1e-9) geometry.lines.push_back({a, b, kJointLink});
    }
    return geometry;
}

PhysicsOverlay::PhysicsOverlay() = default;

PhysicsOverlay::~PhysicsOverlay() { TfNotice::Revoke(m_key); }

const GizmoGeometry& PhysicsOverlay::geometry(const UsdStageRefPtr& stage, UsdTimeCode time) {
    if (stage != m_stage) {
        TfNotice::Revoke(m_key);
        m_stage = stage;
        m_dirty = true;
        if (stage) {
            m_key = TfNotice::Register(TfCreateWeakPtr(this), &PhysicsOverlay::onChanged, UsdStageWeakPtr(stage));
        }
    }
    const double at = time.IsDefault() ? 0.0 : time.GetValue();
    if (m_dirty || !m_time || *m_time != at) {
        m_geometry = buildPhysicsOverlay(stage, time);
        m_time = at;
        m_dirty = false;
    }
    return m_geometry;
}

void PhysicsOverlay::onChanged(const UsdNotice::ObjectsChanged&, const UsdStageWeakPtr&) { m_dirty = true; }

}  // namespace usdcc::usd
