#include "usdcc/tools/gizmo_shapes.h"

#include <cmath>

namespace usdcc::tools::shapes {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Any two vectors perpendicular to `axis` and to each other, forming a
// right-handed basis with it. Used to build rings/cones/quads around an
// arbitrary axis without special-casing which world axis it is.
void perpendicularBasis(const PXR_NS::GfVec3d& axis, PXR_NS::GfVec3d* u, PXR_NS::GfVec3d* v) {
    const PXR_NS::GfVec3d helper = std::abs(axis[1]) < 0.99 ? PXR_NS::GfVec3d(0.0, 1.0, 0.0) : PXR_NS::GfVec3d(1.0, 0.0, 0.0);
    *u = PXR_NS::GfCross(axis, helper).GetNormalized();
    *v = PXR_NS::GfCross(axis, *u).GetNormalized();
}
}  // namespace

void appendAxisLine(GizmoGeometry& geo, const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& axis, double length,
                     const PXR_NS::GfVec4f& color) {
    geo.lines.push_back({origin, origin + axis * length, color});
}

void appendCone(GizmoGeometry& geo, const PXR_NS::GfVec3d& apex, const PXR_NS::GfVec3d& axis, double height,
                double radius, const PXR_NS::GfVec4f& color) {
    PXR_NS::GfVec3d u, v;
    perpendicularBasis(axis, &u, &v);
    const PXR_NS::GfVec3d base = apex - axis * height;
    constexpr int kSegments = 10;
    for (int i = 0; i < kSegments; ++i) {
        const double a0 = 2.0 * kPi * i / kSegments;
        const double a1 = 2.0 * kPi * (i + 1) / kSegments;
        const PXR_NS::GfVec3d p0 = base + (u * std::cos(a0) + v * std::sin(a0)) * radius;
        const PXR_NS::GfVec3d p1 = base + (u * std::cos(a1) + v * std::sin(a1)) * radius;
        geo.triangles.push_back({apex, p0, p1, color});
        geo.triangles.push_back({base, p1, p0, color});
    }
}

void appendBox(GizmoGeometry& geo, const PXR_NS::GfVec3d& center, double halfSize, const PXR_NS::GfVec4f& color) {
    const PXR_NS::GfVec3d corners[8] = {
        center + PXR_NS::GfVec3d(-halfSize, -halfSize, -halfSize), center + PXR_NS::GfVec3d(halfSize, -halfSize, -halfSize),
        center + PXR_NS::GfVec3d(halfSize, halfSize, -halfSize),   center + PXR_NS::GfVec3d(-halfSize, halfSize, -halfSize),
        center + PXR_NS::GfVec3d(-halfSize, -halfSize, halfSize),  center + PXR_NS::GfVec3d(halfSize, -halfSize, halfSize),
        center + PXR_NS::GfVec3d(halfSize, halfSize, halfSize),    center + PXR_NS::GfVec3d(-halfSize, halfSize, halfSize),
    };
    constexpr int kFaces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    for (const auto& face : kFaces) {
        geo.triangles.push_back({corners[face[0]], corners[face[1]], corners[face[2]], color});
        geo.triangles.push_back({corners[face[0]], corners[face[2]], corners[face[3]], color});
    }
}

void appendPlaneQuad(GizmoGeometry& geo, const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& uAxis,
                      const PXR_NS::GfVec3d& vAxis, double inset, double size, const PXR_NS::GfVec4f& color) {
    const PXR_NS::GfVec3d p0 = origin + uAxis * inset + vAxis * inset;
    const PXR_NS::GfVec3d p1 = origin + uAxis * (inset + size) + vAxis * inset;
    const PXR_NS::GfVec3d p2 = origin + uAxis * (inset + size) + vAxis * (inset + size);
    const PXR_NS::GfVec3d p3 = origin + uAxis * inset + vAxis * (inset + size);
    geo.triangles.push_back({p0, p1, p2, color});
    geo.triangles.push_back({p0, p2, p3, color});
}

void appendRing(GizmoGeometry& geo, const PXR_NS::GfVec3d& origin, const PXR_NS::GfVec3d& axis, double radius,
                 const PXR_NS::GfVec4f& color) {
    PXR_NS::GfVec3d u, v;
    perpendicularBasis(axis, &u, &v);
    constexpr int kSegments = 48;
    for (int i = 0; i < kSegments; ++i) {
        const double a0 = 2.0 * kPi * i / kSegments;
        const double a1 = 2.0 * kPi * (i + 1) / kSegments;
        const PXR_NS::GfVec3d p0 = origin + (u * std::cos(a0) + v * std::sin(a0)) * radius;
        const PXR_NS::GfVec3d p1 = origin + (u * std::cos(a1) + v * std::sin(a1)) * radius;
        geo.lines.push_back({p0, p1, color});
    }
}

}  // namespace usdcc::tools::shapes
