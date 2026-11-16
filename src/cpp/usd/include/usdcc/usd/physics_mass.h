#pragma once

// A collider's volume and inertia at unit density (docs/PHYSICS.md §5.1): what
// UsdPhysicsRigidBodyAPI::ComputeMassProperties() asks of each collider, so that a body's
// mass follows the schema's precedence (explicit mass, then density, then the material's,
// then 1000 kg/m³) and not a solver's own. Closed form for the primitives; summed signed
// tetrahedra for a convex hull or a closed triangle mesh.
//
// Qt-free, solver-free.

#include "usdcc/usd/physics_desc.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/vec3d.h>

namespace usdcc::usd::physics {

struct ShapeMass {
    double volume = 0.0;          // m³
    PXR_NS::GfMatrix3d inertia;   // at unit density (kg/m³), about centerOfMass, in the shape's frame: m⁵
    PXR_NS::GfVec3d centerOfMass{0.0};  // m, in the shape's frame
};

// A plane has no volume: zero, as for anything that can't have mass.
ShapeMass shapeMass(const ShapeDesc& shape);

// The convex hull of `points`: its triangles, three indices each, outward. Empty for
// fewer than four points not all in a plane.
std::vector<int> convexHullTriangles(const std::vector<PXR_NS::GfVec3f>& points);

}  // namespace usdcc::usd::physics
