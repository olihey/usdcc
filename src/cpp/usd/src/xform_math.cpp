#include "usdcc/usd/xform_math.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/rotation.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {

GfMatrix3d axisRotation(int axis, double degrees) {
    GfVec3d a(0.0);
    a[axis] = 1.0;
    return GfMatrix3d(GfRotation(a, degrees));
}

double nearestTurn(double angle, double reference) { return angle - 360.0 * std::round((angle - reference) / 360.0); }

double maxDifference(const GfMatrix3d& a, const GfMatrix3d& b) {
    double d = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) d = std::max(d, std::abs(a[i][j] - b[i][j]));
    return d;
}

}  // namespace

std::vector<int> axisOrder(UsdGeomXformOp::Type type) {
    switch (type) {
        case UsdGeomXformOp::TypeRotateXYZ:
            return {0, 1, 2};
        case UsdGeomXformOp::TypeRotateXZY:
            return {0, 2, 1};
        case UsdGeomXformOp::TypeRotateYXZ:
            return {1, 0, 2};
        case UsdGeomXformOp::TypeRotateYZX:
            return {1, 2, 0};
        case UsdGeomXformOp::TypeRotateZXY:
            return {2, 0, 1};
        case UsdGeomXformOp::TypeRotateZYX:
            return {2, 1, 0};
        default:
            return {};
    }
}

bool isEulerRotation(UsdGeomXformOp::Type type) { return !axisOrder(type).empty(); }

int singleRotationAxis(UsdGeomXformOp::Type type) {
    switch (type) {
        case UsdGeomXformOp::TypeRotateX:
            return 0;
        case UsdGeomXformOp::TypeRotateY:
            return 1;
        case UsdGeomXformOp::TypeRotateZ:
            return 2;
        default:
            return -1;
    }
}

bool isRotation(UsdGeomXformOp::Type type) {
    return isEulerRotation(type) || singleRotationAxis(type) >= 0 || type == UsdGeomXformOp::TypeOrient;
}

GfMatrix3d eulerToMatrix(const GfVec3d& degreesByAxis, UsdGeomXformOp::Type type) {
    GfMatrix3d m(1.0);
    for (int axis : axisOrder(type)) {
        m = m * axisRotation(axis, degreesByAxis[axis]);  // first in the order is applied first
    }
    return m;
}

GfVec3d matrixToEuler(const GfMatrix3d& rotation, UsdGeomXformOp::Type type, const GfVec3d& hint) {
    const std::vector<int> order = axisOrder(type);
    if (order.empty()) return hint;
    const int i = order[0], j = order[1], k = order[2];
    const GfMatrix3d& m = rotation;

    // Angles by position in the order: M = Ri(t0) · Rj(t1) · Rk(t2), where
    // (row vectors) Rn(t) has [p][q] = sin t for p, q the two axes following n
    // cyclically. Even orders (XYZ, YZX, ZXY) and odd ones differ only in
    // signs; rather than trust a hand-derived sign table, both are computed
    // and kept only if they reproduce the matrix.
    std::vector<GfVec3d> candidates;  // per-position degrees
    for (const double parity : {1.0, -1.0}) {
        const double sin1 = std::clamp(-parity * m[i][k], -1.0, 1.0);
        const double cos1 = std::sqrt(std::max(0.0, 1.0 - sin1 * sin1));
        const double t1 = std::asin(sin1);
        double t0 = 0.0, t2 = 0.0;
        if (cos1 > 1e-6) {
            t0 = std::atan2(parity * m[j][k], m[k][k]);
            t2 = std::atan2(parity * m[i][j], m[i][i]);
        } else {
            // Gimbal lock: only a combination of t0 and t2 is determined. Keep
            // the last-applied axis at its hinted angle and solve for the
            // first exactly: Ri(t0) = M · Rk(t2)^-1 · Rj(t1)^-1.
            t2 = GfDegreesToRadians(hint[k]);
            const GfMatrix3d first = m * axisRotation(k, GfRadiansToDegrees(t2)).GetInverse() *
                                     axisRotation(j, GfRadiansToDegrees(t1)).GetInverse();
            const int p = (i + 1) % 3, q = (i + 2) % 3;
            t0 = std::atan2(first[p][q], first[p][p]);
        }
        const GfVec3d a(GfRadiansToDegrees(t0), GfRadiansToDegrees(t1), GfRadiansToDegrees(t2));
        candidates.push_back(a);
        candidates.push_back(GfVec3d(a[0] + 180.0, 180.0 - a[1], a[2] + 180.0));
    }

    GfVec3d best = hint;
    double bestDistance = std::numeric_limits<double>::max();
    bool found = false;
    for (const GfVec3d& byPosition : candidates) {
        GfVec3d byAxis(0.0);
        byAxis[i] = byPosition[0];
        byAxis[j] = byPosition[1];
        byAxis[k] = byPosition[2];
        if (maxDifference(eulerToMatrix(byAxis, type), m) > 1e-5) continue;
        for (int n = 0; n < 3; ++n) byAxis[n] = nearestTurn(byAxis[n], hint[n]);
        const double distance = (byAxis - hint).GetLengthSq();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = byAxis;
            found = true;
        }
    }
    return found ? best : hint;
}

GfMatrix4d composeOps(const std::vector<UsdGeomXformOp>& ops, size_t begin, size_t end, UsdTimeCode time) {
    GfMatrix4d m(1.0);
    for (size_t n = begin; n < end && n < ops.size(); ++n) {
        m = ops[n].GetOpTransform(time) * m;  // a later op in the list is applied earlier
    }
    return m;
}

}  // namespace usdcc::usd
