#pragma once

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <array>
#include <vector>

namespace usdcc::usd {

// Rotation and op-stack maths shared by the transform-stack editor and the
// gizmo tools (docs/ATTRIBUTE_EDITOR.md §6, §7.3). Qt-free.
//
// Conventions (USD's): row vectors, so a point is transformed as p * M, and an
// xformOpOrder [op0, op1, …, opN-1] composes to M(opN-1) · … · M(op1) · M(op0)
// — the op listed first is applied last.

// The three Euler rotate op types store angles per axis (x, y, z) in degrees;
// the type only says which axis is applied first: rotateZYX applies Z, then Y,
// then X. axisOrder() is that sequence ({2, 1, 0} for rotateZYX); empty for
// any other op type.
std::vector<int> axisOrder(PXR_NS::UsdGeomXformOp::Type type);
bool isEulerRotation(PXR_NS::UsdGeomXformOp::Type type);       // rotateXYZ … rotateZYX
int singleRotationAxis(PXR_NS::UsdGeomXformOp::Type type);      // rotateX/Y/Z -> 0/1/2, else -1
bool isRotation(PXR_NS::UsdGeomXformOp::Type type);             // any rotate op or orient

// The rotation an Euler op with these per-axis degrees performs.
PXR_NS::GfMatrix3d eulerToMatrix(const PXR_NS::GfVec3d& degreesByAxis, PXR_NS::UsdGeomXformOp::Type type);

// The per-axis degrees an Euler op of `type` needs to perform `rotation`
// (orthonormal, no scale). Every rotation has two such triples (plus whole
// turns of each angle); this returns the one closest to `hint` — the op's
// previous value — so a drag never makes the numbers jump. At gimbal lock
// (middle angle ±90°) the last-applied axis keeps its hinted angle.
PXR_NS::GfVec3d matrixToEuler(const PXR_NS::GfMatrix3d& rotation, PXR_NS::UsdGeomXformOp::Type type,
                              const PXR_NS::GfVec3d& hint);

// The product of ops[begin, end) in composition order (identity if empty).
PXR_NS::GfMatrix4d composeOps(const std::vector<PXR_NS::UsdGeomXformOp>& ops, size_t begin, size_t end,
                              PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default());

}  // namespace usdcc::usd
