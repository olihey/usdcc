// xform_math: Euler round trips for all six rotation orders (including gimbal
// lock and continuity with the hint), and op composition matching USD's own.
#include "test_harness.h"

#include "usdcc/usd/xform_math.h"

#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <cmath>
#include <random>

using namespace PXR_NS;
using namespace usdcc::usd;

namespace {

double maxDiff(const GfMatrix3d& a, const GfMatrix3d& b) {
    double d = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) d = std::max(d, std::abs(a[i][j] - b[i][j]));
    return d;
}

double maxDiff4(const GfMatrix4d& a, const GfMatrix4d& b) {
    double d = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) d = std::max(d, std::abs(a[i][j] - b[i][j]));
    return d;
}

const UsdGeomXformOp::Type kOrders[] = {UsdGeomXformOp::TypeRotateXYZ, UsdGeomXformOp::TypeRotateXZY,
                                        UsdGeomXformOp::TypeRotateYXZ, UsdGeomXformOp::TypeRotateYZX,
                                        UsdGeomXformOp::TypeRotateZXY, UsdGeomXformOp::TypeRotateZYX};

}  // namespace

USDCC_TEST(xform_math) {
    // eulerToMatrix agrees with USD's own op transform for every order.
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform xf = UsdGeomXform::Define(stage, SdfPath("/X"));
    const GfVec3d angles(25.0, -40.0, 70.0);
    for (UsdGeomXformOp::Type type : kOrders) {
        UsdGeomXformOp op = xf.AddXformOp(type, UsdGeomXformOp::PrecisionDouble,
                                          TfToken("t" + std::to_string(static_cast<int>(type))));
        op.Set(angles);
        const GfMatrix3d usd = op.GetOpTransform(UsdTimeCode::Default()).ExtractRotationMatrix();
        CHECK(maxDiff(usd, eulerToMatrix(angles, type)) < 1e-9, "%s: eulerToMatrix matches USD",
              UsdGeomXformOp::GetOpTypeToken(type).GetText());
    }

    // Round trips with random angles, every order.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> angle(-179.0, 179.0);
    std::uniform_real_distribution<double> middle(-85.0, 85.0);
    for (UsdGeomXformOp::Type type : kOrders) {
        const char* name = UsdGeomXformOp::GetOpTypeToken(type).GetText();
        const std::vector<int> order = axisOrder(type);
        int bad = 0;
        for (int n = 0; n < 200; ++n) {
            GfVec3d a(angle(rng), angle(rng), angle(rng));
            a[order[1]] = middle(rng);  // stay clear of gimbal lock here
            const GfMatrix3d m = eulerToMatrix(a, type);
            const GfVec3d back = matrixToEuler(m, type, a);
            if (maxDiff(eulerToMatrix(back, type), m) > 1e-7 || (back - a).GetLength() > 1e-5) ++bad;
        }
        CHECK(bad == 0, "%s: 200 random round trips (%d bad)", name, bad);

        // With a hint far from the angles, the result still reproduces the matrix.
        const GfVec3d a(10.0, 20.0, 30.0);
        const GfVec3d back = matrixToEuler(eulerToMatrix(a, type), type, GfVec3d(500.0, -300.0, 720.0));
        CHECK(maxDiff(eulerToMatrix(back, type), eulerToMatrix(a, type)) < 1e-7, "%s: arbitrary hint", name);

        // Gimbal lock: middle angle at +-90. The matrix is reproduced and the
        // last-applied axis keeps its hinted angle.
        for (double mid : {90.0, -90.0}) {
            GfVec3d g(30.0, 30.0, 30.0);
            g[order[1]] = mid;
            g[order[2]] = 15.0;
            const GfMatrix3d m = eulerToMatrix(g, type);
            const GfVec3d r = matrixToEuler(m, type, g);
            CHECK(maxDiff(eulerToMatrix(r, type), m) < 1e-6, "%s: gimbal %g reproduces the matrix", name, mid);
            CHECK(std::abs(r[order[2]] - 15.0) < 1e-6, "%s: gimbal %g keeps the hinted last axis (%g)", name, mid,
                  r[order[2]]);
        }

        // Continuity: small steps never jump.
        GfVec3d hint(0.0);
        double worst = 0.0;
        GfMatrix3d acc(1.0);
        for (int step = 0; step < 720; ++step) {
            acc = GfMatrix3d(GfRotation(GfVec3d(0.3, 1.0, 0.2).GetNormalized(), 1.0)) * acc;
            const GfVec3d e = matrixToEuler(acc, type, hint);
            worst = std::max(worst, (e - hint).GetLength());
            hint = e;
        }
        // Near gimbal lock the angles legitimately move fast; a flip to the
        // other triple would be a jump of the order of 180 degrees.
        CHECK(worst < 60.0, "%s: no flips over two full turns (largest step %g)", name, worst);
    }

    CHECK(singleRotationAxis(UsdGeomXformOp::TypeRotateY) == 1 && singleRotationAxis(UsdGeomXformOp::TypeScale) == -1,
          "single-axis detection");
    CHECK(isRotation(UsdGeomXformOp::TypeOrient) && !isRotation(UsdGeomXformOp::TypeTranslate), "isRotation");

    // composeOps over the whole stack equals USD's local transformation.
    UsdGeomXform stack = UsdGeomXform::Define(stage, SdfPath("/Stack"));
    stack.AddTranslateOp().Set(GfVec3d(1, 2, 3));
    stack.AddRotateZYXOp().Set(GfVec3f(10, 20, 30));
    stack.AddTranslateOp(UsdGeomXformOp::PrecisionFloat, TfToken("pivot")).Set(GfVec3f(0.5f, 0, 0));
    stack.AddScaleOp().Set(GfVec3f(2, 1, 3));
    stack.AddTranslateOp(UsdGeomXformOp::PrecisionFloat, TfToken("pivot"), true);
    bool resets = false;
    const std::vector<UsdGeomXformOp> ops = stack.GetOrderedXformOps(&resets);
    GfMatrix4d local;
    stack.GetLocalTransformation(&local, &resets);
    CHECK(maxDiff4(composeOps(ops, 0, ops.size()), local) < 1e-6, "composeOps matches GetLocalTransformation");
    // Split around op 2: inner · op · outer == local.
    const GfMatrix4d split =
        composeOps(ops, 3, ops.size()) * ops[2].GetOpTransform(UsdTimeCode::Default()) * composeOps(ops, 0, 2);
    CHECK(maxDiff4(split, local) < 1e-6, "inner * op * outer == local");
}
