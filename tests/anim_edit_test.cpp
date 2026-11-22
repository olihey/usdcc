// Keyframing (docs/ANIMATION.md phase 2): anim_edit's rule for where an edit goes (§4.5),
// localizing a weaker layer's animation, a sublayer's offset, removing keys and animation,
// splines, a gizmo drag on an animated prim as one undo step, and reparenting.
#include "test_harness.h"


#include "usdcc/usd/anim_edit.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"


#include <pxr/base/ts/knot.h>
#include <pxr/base/ts/spline.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QUndoStack>

#include <cmath>

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
