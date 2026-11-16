#include "usdcc/usd/physics_mass.h"

#include <pxr/base/gf/matrix3d.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

namespace usdcc::usd::physics {

using PXR_NS::GfMatrix3d;
using PXR_NS::GfVec3d;
using PXR_NS::GfVec3f;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A diagonal inertia given along the shape's own axis (Y), turned to `axis`.
GfMatrix3d alongAxis(double across, double along, Axis axis) {
    switch (axis) {
        case Axis::X: return GfMatrix3d(GfVec3d(along, across, across));
        case Axis::Z: return GfMatrix3d(GfVec3d(across, across, along));
        default: return GfMatrix3d(GfVec3d(across, along, across));
    }
}

GfVec3d axisVector(Axis axis) {
    switch (axis) {
        case Axis::X: return GfVec3d(1, 0, 0);
        case Axis::Z: return GfVec3d(0, 0, 1);
        default: return GfVec3d(0, 1, 0);
    }
}

GfMatrix3d outer(const GfVec3d& a, const GfVec3d& b) {
    return GfMatrix3d(a[0] * b[0], a[0] * b[1], a[0] * b[2], a[1] * b[0], a[1] * b[1], a[1] * b[2], a[2] * b[0],
                      a[2] * b[1], a[2] * b[2]);
}

// Volume, centre and inertia of a closed, outward triangle mesh at unit density: the sum
// of the signed tetrahedra each triangle makes with the origin (their covariance).
ShapeMass closedMesh(const std::vector<GfVec3f>& points, const std::vector<int>& triangles) {
    // The covariance of the unit tetrahedron (0, e1, e2, e3), per unit determinant.
    static const GfMatrix3d canonical = GfMatrix3d(2, 1, 1, 1, 2, 1, 1, 1, 2) * (1.0 / 120.0);
    double volume = 0.0;
    GfVec3d moment(0.0);
    GfMatrix3d covariance(0.0);
    for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const GfVec3d a(points[triangles[i]]), b(points[triangles[i + 1]]), c(points[triangles[i + 2]]);
        // Columns a, b, c; GfMatrix3d is row-major, so build it transposed and transpose.
        const GfMatrix3d columns = GfMatrix3d(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]).GetTranspose();
        const double det = PXR_NS::GfDot(a, PXR_NS::GfCross(b, c));
        volume += det / 6.0;
        moment += (det / 6.0) * (a + b + c) / 4.0;
        covariance += det * (columns * canonical * columns.GetTranspose());
    }
    ShapeMass mass;
    if (volume <= 1e-15) return mass;
    mass.volume = volume;
    mass.centerOfMass = moment / volume;
    // About the centre of mass, then from covariance to inertia: tr(C) I - C.
    const GfMatrix3d centred = covariance - volume * outer(mass.centerOfMass, mass.centerOfMass);
    const double trace = centred[0][0] + centred[1][1] + centred[2][2];
    mass.inertia = GfMatrix3d(trace) - centred;
    return mass;
}

}  // namespace

std::vector<int> convexHullTriangles(const std::vector<GfVec3f>& input) {
    const size_t n = input.size();
    if (n < 4) return {};
    std::vector<GfVec3d> p(input.begin(), input.end());
    GfVec3d low = p[0], high = p[0];
    for (const GfVec3d& q : p) {
        for (int k = 0; k < 3; ++k) {
            low[k] = std::min(low[k], q[k]);
            high[k] = std::max(high[k], q[k]);
        }
    }
    const double extent = (high - low).GetLength();
    if (extent <= 0.0) return {};
    const double eps = extent * 1e-9;

    // A first tetrahedron from four points spread as far as they go.
    size_t i0 = 0;
    for (size_t i = 1; i < n; ++i)
        if (p[i][0] < p[i0][0]) i0 = i;
    size_t i1 = i0;
    double best = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = (p[i] - p[i0]).GetLengthSq();
        if (d > best) best = d, i1 = i;
    }
    size_t i2 = i0;
    best = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = PXR_NS::GfCross(p[i1] - p[i0], p[i] - p[i0]).GetLengthSq();
        if (d > best) best = d, i2 = i;
    }
    size_t i3 = i0;
    best = 0.0;
    const GfVec3d baseNormal = PXR_NS::GfCross(p[i1] - p[i0], p[i2] - p[i0]);
    for (size_t i = 0; i < n; ++i) {
        const double d = std::abs(PXR_NS::GfDot(baseNormal, p[i] - p[i0]));
        if (d > best) best = d, i3 = i;
    }
    if (i1 == i0 || i2 == i0 || i3 == i0 || best <= eps * baseNormal.GetLength()) return {};

    struct Face {
        int a, b, c;
        GfVec3d normal;
        double offset;
        bool alive = true;
    };
    std::vector<Face> faces;
    const GfVec3d inside = (p[i0] + p[i1] + p[i2] + p[i3]) / 4.0;
    auto addFace = [&](int a, int b, int c) {
        GfVec3d normal = PXR_NS::GfCross(p[b] - p[a], p[c] - p[a]);
        const double length = normal.GetLength();
        if (length > 0.0) normal /= length;
        if (PXR_NS::GfDot(normal, inside - p[a]) > 0.0) {  // outward, away from the inside
            std::swap(b, c);
            normal = -normal;
        }
        faces.push_back({a, b, c, normal, PXR_NS::GfDot(normal, p[a])});
    };
    const int t[4] = {int(i0), int(i1), int(i2), int(i3)};
    addFace(t[0], t[1], t[2]);
    addFace(t[0], t[1], t[3]);
    addFace(t[0], t[2], t[3]);
    addFace(t[1], t[2], t[3]);

    for (size_t i = 0; i < n; ++i) {
        if (int(i) == t[0] || int(i) == t[1] || int(i) == t[2] || int(i) == t[3]) continue;
        std::vector<size_t> visible;
        for (size_t f = 0; f < faces.size(); ++f) {
            if (faces[f].alive && PXR_NS::GfDot(faces[f].normal, p[i]) - faces[f].offset > eps) visible.push_back(f);
        }
        if (visible.empty()) continue;
        // The horizon: edges of the visible faces whose other face isn't visible.
        std::map<std::pair<int, int>, int> edges;
        for (const size_t f : visible) {
            const Face& face = faces[f];
            for (const auto& [from, to] : {std::pair{face.a, face.b}, std::pair{face.b, face.c}, std::pair{face.c, face.a}})
                ++edges[{from, to}];
        }
        for (const size_t f : visible) faces[f].alive = false;
        for (const auto& [edge, count] : edges) {
            if (edges.count({edge.second, edge.first})) continue;
            // Keeps the visible face's winding, so the new face faces out too.
            GfVec3d normal = PXR_NS::GfCross(p[edge.second] - p[edge.first], p[i] - p[edge.first]);
            const double length = normal.GetLength();
            if (length <= 0.0) continue;
            normal /= length;
            faces.push_back({edge.first, edge.second, int(i), normal, PXR_NS::GfDot(normal, p[edge.first])});
        }
    }
    std::vector<int> triangles;
    for (const Face& face : faces) {
        if (!face.alive) continue;
        triangles.insert(triangles.end(), {face.a, face.b, face.c});
    }
    return triangles;
}

ShapeMass shapeMass(const ShapeDesc& shape) {
    ShapeMass mass;
    switch (shape.kind) {
        case ShapeKind::Sphere: {
            const double r = shape.radius;
            mass.volume = 4.0 / 3.0 * kPi * r * r * r;
            mass.inertia = GfMatrix3d(0.4 * mass.volume * r * r);
            break;
        }
        case ShapeKind::Box: {
            const GfVec3d& h = shape.halfExtents;
            mass.volume = 8.0 * h[0] * h[1] * h[2];
            mass.inertia = GfMatrix3d(GfVec3d(mass.volume / 3.0 * (h[1] * h[1] + h[2] * h[2]),
                                              mass.volume / 3.0 * (h[0] * h[0] + h[2] * h[2]),
                                              mass.volume / 3.0 * (h[0] * h[0] + h[1] * h[1])));
            break;
        }
        case ShapeKind::Cylinder: {
            const double r = shape.radius, h = 2.0 * shape.halfHeight;
            mass.volume = kPi * r * r * h;
            mass.inertia = alongAxis(mass.volume * (3.0 * r * r + h * h) / 12.0, mass.volume * r * r / 2.0, shape.axis);
            break;
        }
        case ShapeKind::Cone: {
            // UsdGeomCone: its base at -height/2 along the axis, its apex at +height/2.
            const double r = shape.radius, h = 2.0 * shape.halfHeight;
            mass.volume = kPi * r * r * h / 3.0;
            mass.inertia = alongAxis(mass.volume * (3.0 / 20.0 * r * r + 3.0 / 80.0 * h * h),
                                     mass.volume * 3.0 / 10.0 * r * r, shape.axis);
            mass.centerOfMass = axisVector(shape.axis) * (-shape.halfHeight / 2.0);
            break;
        }
        case ShapeKind::Capsule: {
            const double r = shape.radius, h = 2.0 * shape.halfHeight;
            const double cylinder = kPi * r * r * h, spheres = 4.0 / 3.0 * kPi * r * r * r;
            mass.volume = cylinder + spheres;
            const double along = cylinder * r * r / 2.0 + spheres * 0.4 * r * r;
            const double across = cylinder * (h * h / 12.0 + r * r / 4.0) +
                                  spheres * (0.4 * r * r + h * h / 4.0 + 3.0 * h * r / 8.0);
            mass.inertia = alongAxis(across, along, shape.axis);
            break;
        }
        case ShapeKind::ConvexHull: mass = closedMesh(shape.points, convexHullTriangles(shape.points)); break;
        case ShapeKind::TriangleMesh: {
            mass = closedMesh(shape.points, shape.triangles);
            // An open mesh encloses nothing: its hull stands in for it.
            if (mass.volume <= 0.0) mass = closedMesh(shape.points, convexHullTriangles(shape.points));
            break;
        }
        case ShapeKind::Plane: break;
    }
    return mass;
}

}  // namespace usdcc::usd::physics
