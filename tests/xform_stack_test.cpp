// XformStack: add (unique suffixes, positions, identity values), move,
// remove (with the attribute-deletion rules), pivot, rename, reset,
// resetXformStack, bake and convert — each keeping or changing the composed
// transform exactly as it should.
#include "test_harness.h"

#include "usdcc/usd/xform_stack.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <cmath>

using namespace PXR_NS;
using usdcc::usd::XformStack;

namespace {

std::vector<std::string> names(const XformStack& s) {
    std::vector<std::string> out;
    for (const auto& op : s.ops()) out.push_back(op.GetOpName().GetString());
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

GfMatrix4d local(const UsdGeomXformable& x) {
    GfMatrix4d m(1.0);
    bool resets = false;
    x.GetLocalTransformation(&m, &resets);
    return m;
}

bool sameMatrix(const GfMatrix4d& a, const GfMatrix4d& b, double eps = 1e-4) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::abs(a[i][j] - b[i][j]) > eps) return false;
    return true;
}

}  // namespace

USDCC_TEST(xform_stack) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform xf = UsdGeomXform::Define(stage, SdfPath("/X"));
    XformStack stack(xf.GetPrim());
    CHECK(stack.isValid(), "valid on an Xform");
    CHECK(!XformStack(stage->DefinePrim(SdfPath("/Typeless"))).isValid(), "invalid on a non-xformable prim");

    // Add: positions, unique suffixes, identity values.
    CHECK(stack.add(UsdGeomXformOp::TypeTranslate, 0).value_or(99) == 0, "add translate");
    CHECK(stack.add(UsdGeomXformOp::TypeScale, 99).value_or(99) == 1, "add scale at the end (clamped)");
    CHECK(stack.add(UsdGeomXformOp::TypeRotateZYX, 1).value_or(99) == 1, "add rotateZYX in the middle");
    const auto second = stack.add(UsdGeomXformOp::TypeScale, 3);
    CHECK(second.value_or(99) == 3, "second scale at the end");
    CHECK(joined(names(stack)) == "xformOp:translate, xformOp:rotateZYX, xformOp:scale, xformOp:scale:scale2",
          "order: %s", joined(names(stack)).c_str());
    CHECK(sameMatrix(local(xf), GfMatrix4d(1.0)), "new ops are identity");
    CHECK(XformStack::opLabel(stack.ops()[3]) == "Scale (scale2)", "label: '%s'", XformStack::opLabel(stack.ops()[3]).c_str());
    CHECK(XformStack::opLabel(stack.ops()[1]) == "Rotate ZYX", "label: '%s'", XformStack::opLabel(stack.ops()[1]).c_str());
    CHECK(stack.add(UsdGeomXformOp::TypeOrient, 0).has_value() && stack.add(UsdGeomXformOp::TypeTransform, 0).has_value(),
          "add orient and transform");
    CHECK(sameMatrix(local(xf), GfMatrix4d(1.0)), "orient and transform start at identity");
    stack.remove(0);
    stack.remove(0);

    // Values, then move: the composed transform changes as expected.
    stack.ops()[0].Set(GfVec3d(1, 2, 3));
    stack.ops()[2].Set(GfVec3d(2, 2, 2));  // XformStack::add() makes double ops
    const GfMatrix4d before = local(xf);
    CHECK(stack.move(0, 3), "move translate to the end");
    CHECK(joined(names(stack)) == "xformOp:rotateZYX, xformOp:scale, xformOp:scale:scale2, xformOp:translate",
          "after move: %s", joined(names(stack)).c_str());
    CHECK(!sameMatrix(local(xf), before), "moving translate after scale changes the result (translate now scaled)");
    CHECK(stack.move(3, 0), "move back");
    CHECK(sameMatrix(local(xf), before), "and back again");
    CHECK(!stack.move(0, 9) && !stack.move(5, 0), "out of range moves refused");

    // Remove: the attribute goes too when nothing else needs it.
    CHECK(stack.remove(3), "remove scale2");
    CHECK(!xf.GetPrim().HasAttribute(TfToken("xformOp:scale:scale2")), "its attribute is deleted");

    // Pivot around the rotate op: composed transform unchanged (pivot starts at 0).
    const GfMatrix4d beforePivot = local(xf);
    CHECK(stack.addPivot(1), "add pivot around rotateZYX");
    CHECK(joined(names(stack)) ==
              "xformOp:translate, xformOp:translate:pivot, xformOp:rotateZYX, !invert!xformOp:translate:pivot, "
              "xformOp:scale",
          "pivot sandwich: %s", joined(names(stack)).c_str());
    CHECK(sameMatrix(local(xf), beforePivot), "pivot keeps the transform");
    CHECK(XformStack::opLabel(stack.ops()[3]) == "Translate (pivot) \xE2\x81\xBB\xC2\xB9", "inverse label: '%s'",
          XformStack::opLabel(stack.ops()[3]).c_str());

    // Removing the forward pivot keeps the attribute: the inverse still uses it.
    CHECK(stack.remove(1), "remove forward pivot");
    CHECK(xf.GetPrim().HasAttribute(TfToken("xformOp:translate:pivot")), "shared attribute kept for the inverse");
    CHECK(stack.remove(2), "remove the inverse");
    CHECK(!xf.GetPrim().HasAttribute(TfToken("xformOp:translate:pivot")), "now deleted");

    // Rename: value, time samples and order references follow.
    stack.addPivot(1);
    stack.ops()[1].Set(GfVec3f(0.5f, 0, 0));
    stack.ops()[1].GetAttr().Set(GfVec3f(1, 0, 0), UsdTimeCode(10));
    const GfMatrix4d beforeRename = local(xf);
    CHECK(stack.rename(1, TfToken("hinge")), "rename pivot -> hinge");
    CHECK(joined(names(stack)) ==
              "xformOp:translate, xformOp:translate:hinge, xformOp:rotateZYX, !invert!xformOp:translate:hinge, "
              "xformOp:scale",
          "renamed incl. inverse: %s", joined(names(stack)).c_str());
    CHECK(!xf.GetPrim().HasAttribute(TfToken("xformOp:translate:pivot")), "old attribute gone");
    CHECK(sameMatrix(local(xf), beforeRename), "rename keeps the transform");
    CHECK(xf.GetPrim().GetAttribute(TfToken("xformOp:translate:hinge")).GetNumTimeSamples() == 1, "time samples moved");
    CHECK(!stack.rename(1, TfToken("bad name")), "invalid suffix refused");
    CHECK(!stack.rename(0, TfToken("hinge")), "a name that's taken is refused (xformOp:translate:hinge exists)");

    // Reset to identity.
    CHECK(stack.resetToIdentity(4), "reset scale");
    GfVec3d s(0.0);
    stack.ops()[4].Get(&s);
    CHECK(s == GfVec3d(1, 1, 1), "scale reset to 1");
    CHECK(!stack.resetToIdentity(3), "an inverse op has no value to reset");

    // resetXformStack.
    CHECK(stack.setResetsXformStack(true) && stack.resetsXformStack(), "reset xform stack on");
    CHECK(stack.ops().size() == 5, "ops kept");

    // Bake: one transform op, same matrix, reset flag kept.
    stack.ops()[2].Set(GfVec3d(10, 20, 30));
    stack.ops()[4].Set(GfVec3d(1, 2, 3));
    const GfMatrix4d beforeBake = local(xf);
    CHECK(stack.bakeToMatrix(), "bake");
    CHECK(joined(names(stack)) == "xformOp:transform", "baked: %s", joined(names(stack)).c_str());
    CHECK(sameMatrix(local(xf), beforeBake), "bake keeps the transform");
    CHECK(stack.resetsXformStack(), "bake keeps resetXformStack");
    CHECK(!xf.GetPrim().HasAttribute(TfToken("xformOp:rotateZYX")), "old op attributes deleted");

    // Convert back to TRS.
    CHECK(stack.canConvertToTRS(), "no shear: convertible");
    CHECK(stack.convertToTRS(), "convert");
    CHECK(joined(names(stack)) == "xformOp:translate, xformOp:rotateXYZ, xformOp:scale", "TRS: %s",
          joined(names(stack)).c_str());
    CHECK(sameMatrix(local(xf), beforeBake), "convert keeps the transform");

    // Shear can't be converted.
    UsdGeomXform sheared = UsdGeomXform::Define(stage, SdfPath("/Sheared"));
    GfMatrix4d shear(1.0);
    shear[1][0] = 0.5;
    sheared.AddTransformOp().Set(shear);
    CHECK(!XformStack(sheared.GetPrim()).canConvertToTRS(), "sheared matrix is not convertible");

    // An op defined in a weaker layer: removing it keeps the attribute.
    SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    root->InsertSubLayerPath(weak->GetIdentifier());
    UsdStageRefPtr layered = UsdStage::Open(root);
    layered->SetEditTarget(UsdEditTarget(weak));
    UsdGeomXform lx = UsdGeomXform::Define(layered, SdfPath("/L"));
    lx.AddTranslateOp().Set(GfVec3d(1, 0, 0));
    layered->SetEditTarget(UsdEditTarget(root));
    XformStack ls(lx.GetPrim());
    CHECK(ls.remove(0), "remove an op defined in a weaker layer");
    CHECK(ls.ops().empty(), "out of the order");
    CHECK(lx.GetPrim().HasAttribute(TfToken("xformOp:translate")), "attribute kept: the weaker layer defines it");
}
