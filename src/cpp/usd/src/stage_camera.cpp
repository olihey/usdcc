#include "usdcc/usd/stage_camera.h"

#include "usdcc/usd/stage.h"
#include "usdcc/usd/value_type_info.h"
#include "usdcc/usd/xform_math.h"
#include "usdcc/usd/xform_stack.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

using PXR_NS::GfCamera;
using PXR_NS::GfMatrix3d;
using PXR_NS::GfMatrix4d;
using PXR_NS::GfVec3d;
using PXR_NS::SdfPath;
using PXR_NS::TfToken;
using PXR_NS::UsdAttribute;
using PXR_NS::UsdGeomCamera;
using PXR_NS::UsdGeomXformOp;
using PXR_NS::UsdPrim;
using PXR_NS::UsdTimeCode;

namespace {

// The ops as translate, an Euler rotate and scale, each optional, in that order.
struct TrsOps {
    UsdGeomXformOp translate;
    UsdGeomXformOp rotate;
    UsdGeomXformOp scale;
};
std::optional<TrsOps> trsOps(const std::vector<UsdGeomXformOp>& ops) {
    TrsOps trs;
    int last = 0;  // 1 translate, 2 rotate, 3 scale: each comes after the one before
    for (const UsdGeomXformOp& op : ops) {
        if (op.IsInverseOp()) return std::nullopt;
        const UsdGeomXformOp::Type type = op.GetOpType();
        const int role = type == UsdGeomXformOp::TypeTranslate ? 1 : isEulerRotation(type) ? 2 : type == UsdGeomXformOp::TypeScale ? 3 : 0;
        if (role == 0 || role <= last) return std::nullopt;
        last = role;
        (role == 1 ? trs.translate : role == 2 ? trs.rotate : trs.scale) = op;
    }
    return trs;
}

bool isOneMatrix(const std::vector<UsdGeomXformOp>& ops) {
    return ops.size() == 1 && ops.front().GetOpType() == UsdGeomXformOp::TypeTransform && !ops.front().IsInverseOp();
}

// The lens attributes Copy View to Camera writes.
std::vector<UsdAttribute> lensAttributes(const UsdGeomCamera& camera) {
    return {camera.GetProjectionAttr(), camera.GetFocalLengthAttr(), camera.GetHorizontalApertureAttr(),
            camera.GetVerticalApertureAttr()};
}

// Writes a float attribute only when it changes, so an unchanged lens leaves no opinion.
bool setFloat(const UsdAttribute& attr, double value) {
    float current = 0.0f;
    if (attr.Get(&current) && std::abs(static_cast<double>(current) - value) <= 1e-6 * std::max(1.0, std::abs(value))) return true;
    return attr.Set(static_cast<float>(value));
}

bool setVector(const UsdGeomXformOp& op, const GfVec3d& value) {
    const PXR_NS::VtValue built = makeNumericValue(op.GetAttr().GetTypeName(), {value[0], value[1], value[2]});
    return !built.IsEmpty() && op.GetAttr().Set(built);
}

GfVec3d currentVector(const UsdGeomXformOp& op, const GfVec3d& fallback) {
    PXR_NS::VtValue value;
    if (!op.GetAttr().Get(&value)) return fallback;
    const std::vector<double> numbers = numericComponents(value);
    return numbers.size() == 3 ? GfVec3d(numbers[0], numbers[1], numbers[2]) : fallback;
}

// The lengths of a matrix's first three rows: its scale, for a matrix without shear.
GfVec3d rowLengths(const GfMatrix4d& m) {
    GfVec3d lengths;
    for (int r = 0; r < 3; ++r) lengths[r] = GfVec3d(m[r][0], m[r][1], m[r][2]).GetLength();
    return lengths;
}

// Whether two camera-to-world matrices place a camera alike, whatever their scale.
bool samePlacement(const GfMatrix4d& a, const GfMatrix4d& b) {
    for (int r = 0; r < 3; ++r) {
        const GfVec3d rowA = GfVec3d(a[r][0], a[r][1], a[r][2]).GetNormalized();
        const GfVec3d rowB = GfVec3d(b[r][0], b[r][1], b[r][2]).GetNormalized();
        if ((rowA - rowB).GetLength() > 1e-4) return false;
    }
    const GfVec3d ta = a.ExtractTranslation(), tb = b.ExtractTranslation();
    return (ta - tb).GetLength() <= 1e-5 * std::max(1.0, tb.GetLength());
}

bool close(double a, double b) { return std::abs(a - b) <= 1e-4 * std::max(1.0, std::abs(b)); }

// Projection and lens, as cameraSeeingView() sets them.
bool writeLens(const UsdGeomCamera& camera, const GfCamera& lens) {
    const bool orthographic = lens.GetProjection() == GfCamera::Orthographic;
    const TfToken projection = orthographic ? PXR_NS::UsdGeomTokens->orthographic : PXR_NS::UsdGeomTokens->perspective;
    TfToken current;
    camera.GetProjectionAttr().Get(&current);
    if (current != projection && !camera.CreateProjectionAttr().Set(projection)) return false;
    if (!orthographic && !setFloat(camera.CreateFocalLengthAttr(), lens.GetFocalLength())) return false;
    return setFloat(camera.CreateHorizontalApertureAttr(), lens.GetHorizontalAperture()) &&
           setFloat(camera.CreateVerticalApertureAttr(), lens.GetVerticalAperture());
}

// Sets the ops of a translate / rotate / scale stack to `local`.
std::string writeTrs(const TrsOps& ops, const GfMatrix4d& local) {
    const GfVec3d translate = local.ExtractTranslation();
    GfVec3d scale(1.0);
    GfMatrix3d rotation(1.0);
    const GfMatrix3d upper = local.ExtractRotationMatrix();
    GfVec3d rows[3];
    for (int r = 0; r < 3; ++r) {
        rows[r] = GfVec3d(upper[r][0], upper[r][1], upper[r][2]);
        scale[r] = rows[r].GetLength();
        if (scale[r] < 1e-12) return "Its parents' transforms squash it flat.";
        rows[r] /= scale[r];
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            if (std::abs(PXR_NS::GfDot(rows[a], rows[b])) > 1e-6) return "Its parents' transforms shear it.";
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rotation[r][c] = rows[r][c];
    if (rotation.GetDeterminant() < 0.0) {  // a mirror: put it in the scale
        scale[0] = -scale[0];
        for (int c = 0; c < 3; ++c) rotation[0][c] = -rotation[0][c];
    }

    if (!ops.translate && translate.GetLength() > 1e-9) return "It has no translate op to move it with.";
    if (!ops.rotate) {
        double turned = 0.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) turned += std::abs(rotation[r][c] - (r == c ? 1.0 : 0.0));
        if (turned > 1e-9) return "It has no rotate op to turn it with.";
    }
    if (!ops.scale && (scale - GfVec3d(1.0)).GetLength() > 1e-6) return "It has no scale op for its parents' scale.";

    if (ops.translate && !setVector(ops.translate, translate)) return "Its translate op couldn't be set.";
    if (ops.rotate) {
        const GfVec3d hint = currentVector(ops.rotate, GfVec3d(0.0));
        if (!setVector(ops.rotate, matrixToEuler(rotation, ops.rotate.GetOpType(), hint))) return "Its rotate op couldn't be set.";
    }
    if (ops.scale && !setVector(ops.scale, scale)) return "Its scale op couldn't be set.";
    return {};
}

}  // namespace

std::optional<GfCamera> stageCameraAt(const UsdPrim& prim, UsdTimeCode time) {
    const UsdGeomCamera camera(prim);
    if (!camera) return std::nullopt;
    return camera.GetCamera(time);
}

std::vector<SdfPath> stageCameraPaths(const PXR_NS::UsdStagePtr& stage) {
    std::vector<SdfPath> paths;
    if (!stage) return paths;
    for (const UsdPrim& prim : stage->Traverse(PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimDefaultPredicate))) {
        if (prim.IsA<UsdGeomCamera>()) paths.push_back(prim.GetPath());
    }
    return paths;
}

std::string whyNotSetCamera(const UsdPrim& prim) {
    const UsdGeomCamera camera(prim);
    if (!camera) return "It isn't a camera.";
    if (prim.IsInstanceProxy()) return "It's part of an instance, which can't be edited here.";
    if (!editTargetReaches(prim)) return "It's outside the variant being edited.";
    if (camera.TransformMightBeTimeVarying()) return "Its transform is animated.";
    for (const UsdAttribute& attr : lensAttributes(camera)) {
        if (attr && attr.ValueMightBeTimeVarying()) return "Its lens is animated.";
    }
    bool resets = false;
    const std::vector<UsdGeomXformOp> ops = camera.GetOrderedXformOps(&resets);
    if (!ops.empty() && !isOneMatrix(ops) && !trsOps(ops)) {
        return "Its transform ops would have to change: it takes one matrix, or translate, rotate and scale.";
    }
    return {};
}

std::string setCameraFromView(const UsdPrim& prim, const GfCamera& camera, UsdTimeCode time) {
    if (std::string problem = whyNotSetCamera(prim); !problem.empty()) return problem;
    const UsdGeomCamera usdCamera(prim);
    bool resets = false;
    const std::vector<UsdGeomXformOp> ops = usdCamera.GetOrderedXformOps(&resets);
    const GfMatrix4d parent = resets ? GfMatrix4d(1.0) : usdCamera.ComputeParentToWorldTransform(time);

    // The camera keeps its own scale: it changes nothing it sees.
    GfMatrix4d current(1.0);
    bool currentResets = false;
    usdCamera.GetLocalTransformation(&current, &currentResets, time);
    GfVec3d scale = rowLengths(current);
    for (int i = 0; i < 3; ++i) {
        if (!(scale[i] > 1e-12)) scale[i] = 1.0;
    }
    const GfMatrix4d world = GfMatrix4d(1.0).SetScale(scale) * camera.GetTransform();
    const GfMatrix4d local = world * parent.GetInverse();

    if (ops.empty()) {
        if (!XformStack(prim).setLocalTransform(local)) return "Its transform couldn't be set.";
    } else if (isOneMatrix(ops)) {
        if (!ops.front().Set(local)) return "Its transform couldn't be set.";
    } else if (std::string problem = writeTrs(*trsOps(ops), local); !problem.empty()) {
        return problem;
    }
    if (!writeLens(usdCamera, camera)) return "Its lens couldn't be set.";

    // What the stage makes of it: a stronger layer's opinions win over the edit target's.
    const GfCamera result = usdCamera.GetCamera(time);
    const bool orthographic = camera.GetProjection() == GfCamera::Orthographic;
    const bool same = samePlacement(result.GetTransform(), camera.GetTransform()) &&
                      result.GetProjection() == camera.GetProjection() &&
                      (orthographic || close(result.GetFocalLength(), camera.GetFocalLength())) &&
                      close(result.GetHorizontalAperture(), camera.GetHorizontalAperture()) &&
                      close(result.GetVerticalAperture(), camera.GetVerticalAperture());
    if (!same) return "A layer stronger than the edit target holds the camera's transform or lens, so the change wouldn't show.";
    return {};
}

SdfPath defineCameraFromView(const PXR_NS::UsdStagePtr& stage, const SdfPath& parent, const GfCamera& camera,
                             UsdTimeCode time) {
    if (!stage) return {};
    const UsdPrim parentPrim = stage->GetPrimAtPath(parent);
    if (!parentPrim) return {};
    SdfPath path = parent.AppendChild(TfToken("Camera"));
    // GetPrimAtPath finds an inactive prim too, and its name is just as taken.
    for (int i = 1; stage->GetPrimAtPath(path); ++i) path = parent.AppendChild(TfToken("Camera" + std::to_string(i)));
    const UsdGeomCamera usdCamera = UsdGeomCamera::Define(stage, path);
    if (!usdCamera) return {};

    PXR_NS::UsdGeomXformCache xforms(time);
    const GfMatrix4d parentToWorld = parent.IsAbsoluteRootPath() ? GfMatrix4d(1.0) : xforms.GetLocalToWorldTransform(parentPrim);
    if (!XformStack(usdCamera.GetPrim()).setLocalTransform(camera.GetTransform() * parentToWorld.GetInverse())) return {};
    if (!writeLens(usdCamera, camera)) return {};
    const PXR_NS::GfRange1f clipping = camera.GetClippingRange();
    usdCamera.CreateClippingRangeAttr().Set(PXR_NS::GfVec2f(clipping.GetMin(), clipping.GetMax()));
    return path;
}

}  // namespace usdcc::usd
