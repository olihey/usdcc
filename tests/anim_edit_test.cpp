// Keyframing (docs/ANIMATION.md phase 2): anim_edit's rule for where an edit goes (§4.5),
// localizing a weaker layer's animation, a sublayer's offset, removing keys and animation,
// splines, a gizmo drag on an animated prim as one undo step, and reparenting.
#include "test_harness.h"


#include "usdcc/usd/anim_edit.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"


#include <pxr/base/ts/knot.h>
#include <pxr/base/ts/spline.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QUndoStack>

#include <cmath>
#include <set>

using namespace PXR_NS;
namespace anim = usdcc::usd::anim;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;

namespace {
bool isClose(double a, double b) { return std::abs(a - b) < 1e-9; }

double at(const UsdAttribute& attribute, double time) {
    double value = 0.0;
    attribute.Get(&value, time);
    return value;
}

std::size_t samplesIn(const SdfLayerHandle& layer, const SdfPath& path) { return layer->ListTimeSamplesForPath(path).size(); }

// Auto-key back off whatever a test did.
struct AutoKeyOff {
    ~AutoKeyOff() { anim::setAutoKey(false); }
};
}  // namespace

// §4.5, case by case.
USDCC_TEST(anim_edit_rule) {
    AutoKeyOff reset;
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();
    size.Set(1.0);

    // Not animated, auto-key off: the default, as before.
    CHECK(anim::setValue(size, VtValue(2.0), UsdTimeCode(10.0), false), "set");
    CHECK(!anim::isAnimated(size) && isClose(at(size, 10.0), 2.0) && isClose(at(size, 99.0), 2.0), "a static edit sets the default");
    CHECK(anim::keyState(size, 10.0) == anim::KeyState::Static, "static");

    // Not animated, auto-key on: a key at the time.
    CHECK(anim::setValue(size, VtValue(3.0), UsdTimeCode(10.0), true), "set with auto-key");
    CHECK(anim::isAnimated(size) && size.GetNumTimeSamples() == 1 && isClose(at(size, 10.0), 3.0), "auto-key sets a key");
    CHECK(anim::keyState(size, 10.0) == anim::KeyState::KeyedHere && anim::keyState(size, 5.0) == anim::KeyState::Animated,
          "keyed here, animated elsewhere");

    // Animated: a key, auto-key or not.
    CHECK(anim::setValue(size, VtValue(5.0), UsdTimeCode(20.0), false), "an animated attribute");
    CHECK(size.GetNumTimeSamples() == 2 && isClose(at(size, 20.0), 5.0) && isClose(at(size, 15.0), 4.0),
          "gets a key even with auto-key off, and plays between them");
    CHECK(anim::keyTimes({size}) == std::vector<double>({10.0, 20.0}), "its keys");

    // At the default time: always the default.
    CHECK(anim::setValue(size, VtValue(7.0), UsdTimeCode::Default(), true) && size.GetNumTimeSamples() == 2,
          "the default time sets the default");

    // setKey keys the value it has; removeKey takes one away.
    CHECK(anim::setKey(size, 15.0) && size.GetNumTimeSamples() == 3 && isClose(at(size, 15.0), 4.0), "Set Key keeps the value");
    CHECK(anim::removeKey(size, 15.0) && size.GetNumTimeSamples() == 2, "Remove Key");
    CHECK(!anim::removeKey(size, 12.0), "nothing to remove between keys");

    // Remove Animation keeps the pose at the time.
    CHECK(anim::removeAnimation(size, 20.0), "Remove Animation");
    CHECK(!anim::isAnimated(size) && isClose(at(size, 0.0), 5.0), "the value at that time stays, for all times");

    // The last key gone, the value it had stays.
    anim::setValue(size, VtValue(8.0), UsdTimeCode(3.0), true);
    CHECK(anim::removeKey(size, 3.0) && !anim::isAnimated(size) && isClose(at(size, 50.0), 8.0), "the last key's value stays");

    // Auto-key, app-wide.
    int changes = 0;
    auto connection = anim::autoKeyChanged().connect([&changes](bool) { ++changes; });
    anim::setAutoKey(true);
    anim::setAutoKey(true);
    CHECK(anim::autoKey() && changes == 1, "auto-key on, said once");
}

// A weaker layer's animation is copied into the edit target before the first key, so one
// key doesn't flatten it.
USDCC_TEST(anim_edit_localizes) {
    const SdfLayerRefPtr weak = SdfLayer::CreateAnonymous("weak.usda");
    const SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    root->InsertSubLayerPath(weak->GetIdentifier());
    const UsdStageRefPtr stage = UsdStage::Open(root);
    stage->SetEditTarget(UsdEditTarget(weak));
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();
    size.Set(1.0, 0.0);
    size.Set(3.0, 20.0);
    stage->SetEditTarget(UsdEditTarget(root));

    CHECK(anim::setValue(size, VtValue(9.0), UsdTimeCode(10.0), false), "a key in the stronger layer");
    CHECK(samplesIn(root, size.GetPath()) == 3, "the weaker layer's two keys came along: %zu", samplesIn(root, size.GetPath()));
    CHECK(isClose(at(size, 0.0), 1.0) && isClose(at(size, 20.0), 3.0) && isClose(at(size, 10.0), 9.0), "and still play: %g %g %g", at(size, 0.0), at(size, 20.0), at(size, 10.0));
    CHECK(samplesIn(weak, size.GetPath()) == 2, "the weaker layer as it was");
    CHECK(!anim::localizeAnimation(size), "once only");
    // Remove Animation: the edit target's samples go, and its default (stronger than the
    // weaker layer's samples) holds the pose; the weaker layer is left as it is.
    CHECK(anim::removeAnimation(size, 10.0) && samplesIn(root, size.GetPath()) == 0 && samplesIn(weak, size.GetPath()) == 2,
          "Remove Animation takes the edit target's samples only");
    CHECK(!anim::isAnimated(size) && isClose(at(size, 0.0), 9.0) && isClose(at(size, 20.0), 9.0), "and holds the pose at 10");
}

// Through UsdAttribute, so the edit target's offset applies: a sublayer 10 frames later
// keys stage time 15 at its own 5.
USDCC_TEST(anim_edit_sublayer_offset) {
    const SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("sub.usda");
    const SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    root->InsertSubLayerPath(sub->GetIdentifier());
    root->SetSubLayerOffset(SdfLayerOffset(10.0), 0);
    const UsdStageRefPtr stage = UsdStage::Open(root);
    stage->SetEditTarget(stage->GetEditTargetForLocalLayer(sub));
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();

    CHECK(anim::setValue(size, VtValue(4.0), UsdTimeCode(15.0), true), "keyed at stage time 15");
    const std::set<double> times = sub->ListTimeSamplesForPath(size.GetPath());
    CHECK(times.size() == 1 && isClose(*times.begin(), 5.0), "at the sublayer's 5: %g", times.empty() ? -1.0 : *times.begin());
    CHECK(anim::keyTimes({size}) == std::vector<double>({15.0}) && anim::keyState(size, 15.0) == anim::KeyState::KeyedHere,
          "and reads back in stage time");
}

// A spline-driven attribute gets a knot, not a sample.
USDCC_TEST(anim_edit_splines) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();
    TsSpline spline(TfType::Find<double>());
    for (const auto& [time, value] : {std::pair{0.0, 1.0}, std::pair{20.0, 3.0}}) {
        TsKnot knot(TfType::Find<double>());
        knot.SetTime(time);
        knot.SetValue(value);
        spline.SetKnot(knot);
    }
    size.SetSpline(spline);
    CHECK(anim::isAnimated(size) && anim::keyTimes({size}) == std::vector<double>({0.0, 20.0}), "a spline's knots are its keys");
    CHECK(anim::setValue(size, VtValue(5.0), UsdTimeCode(10.0), false), "keyed");
    CHECK(size.GetNumTimeSamples() == 0 && size.GetSpline().GetKnots().size() == 3 && isClose(at(size, 10.0), 5.0),
          "as a third knot, no samples");
    CHECK(anim::removeKey(size, 10.0) && size.GetSpline().GetKnots().size() == 2, "and removed again");
}

// A gizmo drag on an animated prim keys it, and is one undo step; undo takes the key back.
USDCC_TEST(anim_edit_gizmo_drag) {
    StageManager manager;
    UndoManager undo;
    const StageRefPtr stage = manager.createStage();
    const UsdGeomCube cube = UsdGeomCube::Define(stage->usdStage(), SdfPath("/Cube"));
    UsdGeomXformOp translate = cube.AddTranslateOp();
    translate.Set(GfVec3d(0, 0, 0), 0.0);
    translate.Set(GfVec3d(0, 0, 0), 20.0);
    QUndoStack* stack = undo.stack(stage);
    const int before = stack->count();

    // What MoveTool's drag does at frame 10, many moves in one session.
    undo.beginEdit(stage, "Move");
    for (int step = 1; step <= 5; ++step) {
        anim::setValue(translate.GetAttr(), VtValue(GfVec3d(step, 0, 0)), UsdTimeCode(10.0), false);
    }
    CHECK(undo.endEdit(stage) && stack->count() == before + 1, "one undo step");
    GfVec3d moved;
    translate.Get(&moved, 10.0);
    CHECK(moved == GfVec3d(5, 0, 0) && translate.GetAttr().GetNumTimeSamples() == 3, "a key at 10, where it was dragged to");
    GfVec3d elsewhere;
    translate.Get(&elsewhere, 0.0);
    CHECK(elsewhere == GfVec3d(0, 0, 0), "the animation elsewhere stays");
    stack->undo();
    CHECK(translate.GetAttr().GetNumTimeSamples() == 2 && anim::keyState(translate.GetAttr(), 10.0) == anim::KeyState::Animated,
          "undo takes the key back: %zu", translate.GetAttr().GetNumTimeSamples());
}

// ---- Key editing (docs/ANIMATION.md phase 3) ----------------------------------------------

namespace {
// size keyed 1 at 0, 2 at 10 and 3 at 20.
UsdAttribute keyedSize(const UsdStageRefPtr& stage, const char* path = "/Cube") {
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath(path)).GetSizeAttr();
    size.Set(1.0, 0.0);
    size.Set(2.0, 10.0);
    size.Set(3.0, 20.0);
    return size;
}
}  // namespace

// Moving keys keeps their values; one landing on a key that stays replaces it.
USDCC_TEST(anim_edit_move_keys) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = keyedSize(stage);
    CHECK(anim::moveKeys(anim::keysAt({size}, 10.0), 5.0), "moved");
    CHECK(anim::keyTimes({size}) == std::vector<double>({0.0, 15.0, 20.0}) && isClose(at(size, 15.0), 2.0),
          "the key at 10 is at 15, with its value");
    CHECK(anim::moveKeys({{size, 15.0}}, 5.0) && anim::keyTimes({size}) == std::vector<double>({0.0, 20.0}) &&
              isClose(at(size, 20.0), 2.0),
          "one landing on another replaces it");
    CHECK(!anim::moveKeys({{size, 7.0}}, 3.0), "a time that isn't a key moves nothing");
    // Two keys moved past each other's times at once, both kept.
    CHECK(anim::moveKeys({{size, 0.0}, {size, 20.0}}, 20.0) && anim::keyTimes({size}) == std::vector<double>({20.0, 40.0}) &&
              isClose(at(size, 20.0), 1.0) && isClose(at(size, 40.0), 2.0),
          "moved together, read first");
}

USDCC_TEST(anim_edit_scale_keys) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = keyedSize(stage);
    CHECK(anim::scaleKeys({{size, 0.0}, {size, 10.0}, {size, 20.0}}, 0.0, 2.0) &&
              anim::keyTimes({size}) == std::vector<double>({0.0, 20.0, 40.0}) && isClose(at(size, 40.0), 3.0),
          "about 0 by 2: 0, 20, 40");
    CHECK(anim::scaleKeys({{size, 0.0}, {size, 20.0}, {size, 40.0}}, 40.0, 0.25) &&
              anim::keyTimes({size}) == std::vector<double>({30.0, 35.0, 40.0}),
          "about the last by a quarter: 30, 35, 40");
    CHECK(anim::scaleKeys({{size, 30.0}, {size, 35.0}}, 30.0, 0.3) &&
              anim::keyTimes({size}) == std::vector<double>({30.0, 32.0, 40.0}),
          "new times snap to whole frames");
}

// Removing keys, copying and pasting them, on the same attribute and on another prim's.
USDCC_TEST(anim_edit_copy_paste_remove_keys) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = keyedSize(stage);
    const anim::CopiedKeys copied = anim::copyKeys({{size, 10.0}, {size, 20.0}});
    CHECK(copied.attributes.size() == 1 && copied.attributes[0].keys.size() == 2 &&
              isClose(copied.attributes[0].keys[1].offset, 10.0),
          "copied with times from the earliest");
    CHECK(anim::pasteKeys(copied, stage, 50.0) == 2 &&
              anim::keyTimes({size}) == std::vector<double>({0.0, 10.0, 20.0, 50.0, 60.0}) && isClose(at(size, 60.0), 3.0),
          "pasted at 50 onto the attribute they came from");

    const UsdAttribute other = UsdGeomCube::Define(stage, SdfPath("/Other")).GetSizeAttr();
    other.Set(7.0);
    CHECK(anim::pasteKeys(copied, stage, 5.0, {SdfPath("/Other")}) == 2 &&
              anim::keyTimes({other}) == std::vector<double>({5.0, 15.0}) && isClose(at(other, 5.0), 2.0),
          "onto another prim's attribute of the same name");

    CHECK(anim::removeKeys({{size, 50.0}, {size, 60.0}}) && anim::keyTimes({size}) == std::vector<double>({0.0, 10.0, 20.0}),
          "removed");
    CHECK(anim::removeKeys(anim::keysAt({other}, 5.0)) && anim::removeKeys(anim::keysAt({other}, 15.0)) &&
              !anim::isAnimated(other) && isClose(at(other, 0.0), 3.0),
          "the last key gone, its value stays as the default");
}

// A spline's knots move with their tangents; keys of a sublayer move in stage time.
USDCC_TEST(anim_edit_move_knots_and_offsets) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();
    TsSpline spline(TfType::Find<double>());
    for (const auto& [time, value] : {std::pair{0.0, 1.0}, std::pair{20.0, 3.0}}) {
        TsKnot knot(TfType::Find<double>());
        knot.SetTime(time);
        knot.SetValue(value);
        knot.SetPostTanWidth(4.0);
        spline.SetKnot(knot);
    }
    size.SetSpline(spline);
    CHECK(anim::moveKeys({{size, 20.0}}, 10.0) && size.GetNumTimeSamples() == 0, "a knot moved, still a spline");
    TsKnot moved;
    CHECK(size.GetSpline().GetKnot(30.0, &moved) && isClose(moved.GetPostTanWidth(), 4.0) && isClose(at(size, 30.0), 3.0),
          "at 30, with its value and tangent");

    const SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("sub.usda");
    const SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    root->InsertSubLayerPath(sub->GetIdentifier());
    root->SetSubLayerOffset(SdfLayerOffset(10.0), 0);
    const UsdStageRefPtr offset = UsdStage::Open(root);
    offset->SetEditTarget(offset->GetEditTargetForLocalLayer(sub));
    const UsdAttribute shifted = keyedSize(offset);  // stage times 0, 10, 20: the sublayer's -10, 0, 10
    CHECK(anim::moveKeys({{shifted, 20.0}}, 5.0) && anim::keyTimes({shifted}) == std::vector<double>({0.0, 10.0, 25.0}),
          "in stage time");
    const std::set<double> times = sub->ListTimeSamplesForPath(shifted.GetPath());
    CHECK(times.count(15.0) == 1, "the sublayer's own 15");
}

// A move done in an undo session is one step, and undoes.
USDCC_TEST(anim_edit_move_keys_undo) {
    StageManager manager;
    UndoManager undo;
    const StageRefPtr stage = manager.createStage();
    const UsdAttribute size = keyedSize(stage->usdStage());
    QUndoStack* stack = undo.stack(stage);
    const int before = stack->count();
    CHECK(undo.apply(stage, "Move Keys", [&]() { return anim::moveKeys(anim::keysAt({size}, 10.0), 3.0); }) &&
              stack->count() == before + 1,
          "one step");
    stack->undo();
    CHECK(anim::keyTimes({size}) == std::vector<double>({0.0, 10.0, 20.0}) && isClose(at(size, 10.0), 2.0), "undone");
}

// ---- Curves (docs/ANIMATION.md phase 4) ---------------------------------------------------

// Convert to Curve goes through every sample; Bake to Samples comes back to the same values.
USDCC_TEST(anim_edit_convert_and_bake) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();
    size.Set(1.0, 0.0);
    size.Set(3.0, 10.0);
    size.Set(2.0, 20.0);
    CHECK(anim::canHaveCurve(size) && !anim::hasCurve(size), "a double can have a curve");
    CHECK(anim::convertToCurve(size) && anim::hasCurve(size) && size.GetNumTimeSamples() == 0, "converted, no samples left");
    CHECK(isClose(at(size, 0.0), 1.0) && isClose(at(size, 10.0), 3.0) && isClose(at(size, 20.0), 2.0),
          "through every sample's value");
    CHECK(anim::keyTimes({size}) == std::vector<double>({0.0, 10.0, 20.0}), "its knots are the keys");
    const double between = at(size, 5.0);
    CHECK(between > 1.0 && between < 3.0, "a curve between them: %g", between);
    CHECK(anim::bakeToSamples(size) && !anim::hasCurve(size) && size.GetNumTimeSamples() == 21, "baked: a sample a frame");
    CHECK(isClose(at(size, 10.0), 3.0) && std::abs(at(size, 5.0) - between) < 1e-9, "the same values: %g", at(size, 5.0));

    const UsdAttribute visibility = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetVisibilityAttr();
    CHECK(!anim::canHaveCurve(visibility), "a token can't");
    const UsdAttribute extent = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetExtentAttr();
    CHECK(!anim::canHaveCurve(extent), "nor an array");
}

// Interpolation, extrapolation and an inner loop on a curve; knots moved by a drag.
USDCC_TEST(anim_edit_curve_settings) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();
    size.Set(1.0, 0.0);
    size.Set(3.0, 10.0);
    size.Set(2.0, 20.0);
    anim::convertToCurve(size);
    CHECK(anim::setKnotInterpolation(size, {0.0}, TsInterpHeld) && isClose(at(size, 5.0), 1.0), "held after 0");
    CHECK(anim::setKnotInterpolation(size, {0.0}, TsInterpLinear) && isClose(at(size, 5.0), 2.0), "linear after 0");
    CHECK(anim::setKnotTangents(size, {10.0}, anim::Tangents::Flat), "flat at 10");
    TsKnot knot;
    double slope = 1.0;
    CHECK(size.GetSpline().GetKnot(10.0, &knot) && knot.GetPostTanSlope(&slope) && isClose(slope, 0.0), "no slope");
    CHECK(anim::setExtrapolation(size, true, TsExtrapolation(TsExtrapLoopRepeat)) &&
              std::abs(at(size, 25.0) - (at(size, 5.0) + 1.0)) < 1e-9,
          "after the last knot it repeats, offset so the ends meet: %g", at(size, 25.0));
    CHECK(anim::setExtrapolation(size, false, TsExtrapolation(TsExtrapHeld)) && isClose(at(size, -5.0), 1.0), "held before");
    CHECK(anim::setInnerLoop(size, 0.0, 10.0, 0, 1) && size.GetSpline().HasInnerLoops(), "an inner loop");
    CHECK(anim::setInnerLoop(size, 0.0, 0.0, 0, 0) && !size.GetSpline().HasInnerLoops(), "and none");

    const TsSpline moved = anim::movedKnots(size.GetSpline(), {10.0}, 2.0, 0.5);
    TsKnot shifted;
    double value = 0.0;
    CHECK(moved.GetKnot(12.0, &shifted) && shifted.GetValue(&value) && isClose(value, 3.5) && !moved.GetKnot(10.0, &knot),
          "a knot moved 2 frames and half a unit up");
}

// A translate and a scale split into X, Y and Z: the same matrix at every time.
USDCC_TEST(anim_edit_split_ops) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdGeomXformable cube(UsdGeomCube::Define(stage, SdfPath("/Cube")));
    UsdGeomXformOp translate = cube.AddTranslateOp(UsdGeomXformOp::PrecisionDouble, TfToken("pivot"));
    translate.Set(GfVec3d(1, 2, 3), 0.0);
    translate.Set(GfVec3d(4, 5, 6), 10.0);
    cube.AddRotateYOp().Set(30.0f);
    UsdGeomXformOp scale = cube.AddScaleOp();
    scale.Set(GfVec3f(2, 3, 4));
    const auto matrixAt = [&cube](double time) {
        GfMatrix4d matrix;
        bool resets = false;
        cube.GetLocalTransformation(&matrix, &resets, time);
        return matrix;
    };
    const GfMatrix4d at0 = matrixAt(0.0), at5 = matrixAt(5.0), at10 = matrixAt(10.0);

    std::string why;
    const std::vector<UsdGeomXformOp> made = anim::splitXformOp(translate, &why);
    CHECK(made.size() == 3 && made[0].GetOpName() == TfToken("xformOp:translateX:pivot"), "translate split: %s", why.c_str());
    CHECK(anim::splitXformOp(scale, &why).size() == 3, "scale split: %s", why.c_str());
    bool resets = false;
    const std::vector<UsdGeomXformOp> ops = cube.GetOrderedXformOps(&resets);
    CHECK(ops.size() == 7 && ops[3].GetOpType() == UsdGeomXformOp::TypeRotateY, "in place of the old ops: %zu",
          ops.size());
    CHECK(GfIsClose(matrixAt(0.0), at0, 1e-9) && GfIsClose(matrixAt(5.0), at5, 1e-9) && GfIsClose(matrixAt(10.0), at10, 1e-9),
          "the same matrix at every time");
    CHECK(made[1].GetAttr().GetNumTimeSamples() == 2 && anim::canHaveCurve(made[1].GetAttr()), "each axis keyed, and can be a curve");
    CHECK(anim::splitXformOp(ops[3], &why).empty() && !why.empty(), "a rotate doesn't split: %s", why.c_str());
}

// A bake's whole animation at once (Physics' Bake Simulation): what was in the edit target
// goes, nothing is copied from a weaker layer, and the edit target's offset applies.
USDCC_TEST(anim_edit_set_samples) {
    const SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("sub.usda");
    const SdfLayerRefPtr root = SdfLayer::CreateAnonymous("root.usda");
    root->InsertSubLayerPath(sub->GetIdentifier());
    root->SetSubLayerOffset(SdfLayerOffset(10.0), 0);
    const UsdStageRefPtr stage = UsdStage::Open(root);
    stage->SetEditTarget(stage->GetEditTargetForLocalLayer(sub));
    const UsdAttribute size = UsdGeomCube::Define(stage, SdfPath("/Cube")).GetSizeAttr();
    size.Set(7.0, 50.0);  // what was there before

    CHECK(anim::setSamples(size, {11.0, 12.0}, {VtValue(1.0), VtValue(2.0)}), "set");
    const std::set<double> times = sub->ListTimeSamplesForPath(size.GetPath());
    CHECK(times == std::set<double>({1.0, 2.0}), "the old sample gone, the new at the sublayer's 1 and 2");
    CHECK(isClose(at(size, 11.0), 1.0) && isClose(at(size, 12.0), 2.0), "and they read back at stage 11 and 12");
    CHECK(!anim::setSamples(size, {1.0}, {}), "times and values must pair up");
}
