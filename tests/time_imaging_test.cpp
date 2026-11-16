// What a viewport draws at a time (docs/ANIMATION.md phase 1): UsdImaging's scene indices
// at one time and then another, pulled the way a render syncs a prim. Time samples and a
// spline both move the prim; a prim with neither stays where it is.
#include "test_harness.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/ts/knot.h>
#include <pxr/base/ts/spline.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
struct Scene {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdImagingSceneIndices indices;

    void start() {
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        indices = UsdImagingCreateSceneIndices(info);
    }

    // Where the prim is, in the world, at `time`: what Hydra is handed for it.
    GfVec3d positionAt(const char* path, UsdTimeCode time) {
        indices.stageSceneIndex->SetTime(time);
        indices.stageSceneIndex->ApplyPendingUpdates();
        const HdSceneIndexPrim prim = indices.finalSceneIndex->GetPrim(SdfPath(path));
        const HdMatrixDataSourceHandle matrix = HdXformSchema::GetFromParent(prim.dataSource).GetMatrix();
        return matrix ? matrix->GetTypedValue(0.0f).ExtractTranslation() : GfVec3d(std::nan(""));
    }
};

bool isNear(double a, double b) { return std::abs(a - b) < 1e-6; }
}  // namespace

USDCC_TEST(time_imaging_time_samples) {
    Scene scene;
    const UsdGeomXformOp translate = UsdGeomXform::Define(scene.stage, SdfPath("/Sampled")).AddTranslateOp();
    translate.Set(GfVec3d(0, 0, 0), UsdTimeCode(0.0));
    translate.Set(GfVec3d(10, 0, 0), UsdTimeCode(10.0));
    UsdGeomXform::Define(scene.stage, SdfPath("/Still")).AddTranslateOp().Set(GfVec3d(3, 0, 0));
    scene.start();

    CHECK(isNear(scene.positionAt("/Sampled", UsdTimeCode(0.0))[0], 0.0), "at its first sample");
    CHECK(isNear(scene.positionAt("/Sampled", UsdTimeCode(5.0))[0], 5.0), "halfway between two: halfway (%g)",
          scene.positionAt("/Sampled", UsdTimeCode(5.0))[0]);
    CHECK(isNear(scene.positionAt("/Sampled", UsdTimeCode(10.0))[0], 10.0), "at its last");
    CHECK(isNear(scene.positionAt("/Sampled", UsdTimeCode(25.0))[0], 10.0), "after it: held");
    // A numeric time is what the viewport always draws at: a default alone still shows.
    CHECK(isNear(scene.positionAt("/Still", UsdTimeCode(0.0))[0], 3.0) && isNear(scene.positionAt("/Still", UsdTimeCode(7.0))[0], 3.0),
          "a prim with only a default is there at every time");
}

// docs/ANIMATION.md §7.1: a spline-driven attribute reaches Hydra as time-varying.
USDCC_TEST(time_imaging_spline) {
    Scene scene;
    const UsdGeomXformOp translateX = UsdGeomXform::Define(scene.stage, SdfPath("/Curve")).AddTranslateXOp();
    TsSpline spline(TfType::Find<double>());
    for (const auto& [time, value] : {std::pair{0.0, 0.0}, std::pair{10.0, 20.0}}) {
        TsDoubleKnot knot;
        knot.SetTime(time);
        knot.SetValue(value);
        knot.SetNextInterpolation(TsInterpLinear);
        CHECK(spline.SetKnot(knot), "the knot at %g goes in", time);
    }
    CHECK(translateX.GetAttr().SetSpline(spline), "the spline is authored");
    CHECK(translateX.GetAttr().HasSpline() && translateX.GetAttr().ValueMightBeTimeVarying(), "USD says it varies");
    scene.start();

    CHECK(isNear(scene.positionAt("/Curve", UsdTimeCode(0.0))[0], 0.0), "at the first knot (%g)",
          scene.positionAt("/Curve", UsdTimeCode(0.0))[0]);
    CHECK(isNear(scene.positionAt("/Curve", UsdTimeCode(5.0))[0], 10.0), "halfway along a linear segment (%g)",
          scene.positionAt("/Curve", UsdTimeCode(5.0))[0]);
    CHECK(isNear(scene.positionAt("/Curve", UsdTimeCode(10.0))[0], 20.0), "at the last knot (%g)",
          scene.positionAt("/Curve", UsdTimeCode(10.0))[0]);
}
