// Curves (docs/ANIMATION.md phase 4): the Curve Editor's curves, Convert to Curve and Bake to
// Samples, a knot dragged and a tangent dragged (each one undo step), interpolation, and a
// translate split into X, Y and Z. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/anim_edit.h"
#include "usdcc/usd/curve_editor_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/ts/knot.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>

#include <QApplication>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>
#include <vector>

using namespace usdcc::usd;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 8; ++i) QApplication::processEvents();
}

struct Scale {
    QTemporaryDir folder;
    usdcc::ui::UiScaleController controller{*static_cast<QApplication*>(QCoreApplication::instance()),
                                            folder.filePath(QStringLiteral("ui.ini"))};
};

void mouse(QWidget* widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, widget->mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void dragIn(QWidget* widget, const QPointF& from, const QPointF& to) {
    mouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(widget, QEvent::MouseMove, (from + to) / 2.0, Qt::NoButton, Qt::LeftButton);
    mouse(widget, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    mouse(widget, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

double valueAt(const UsdAttribute& attribute, double time) {
    double value = 0.0;
    attribute.Get(&value, time);
    return value;
}

bool isNear(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) < tolerance; }
}  // namespace

USDCC_TEST(curve_editor_curves) {
    TestSettings settings;
    Scale scale;
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdGeomCube cube = UsdGeomCube::Define(stage->usdStage(), SdfPath("/Box"));
    UsdGeomXformOp translate = cube.AddTranslateOp();
    translate.Set(GfVec3d(0, 0, 0), 0.0);
    translate.Set(GfVec3d(4, 0, 0), 20.0);
    const UsdAttribute size = cube.GetSizeAttr();
    size.Set(1.0, 0.0);
    size.Set(3.0, 10.0);
    size.Set(2.0, 20.0);
    stages.setSelectedPaths(stage, {SdfPath("/Box")});

    CurveEditorViewPanel panel(&stages, stage, undo);
    panel.resize(900, 500);
    panel.show();
    QTest::qWait(200);
    settle();
    CHECK(panel.curves().size() == 1 && panel.otherAttributes().size() == 1, "size is a curve; the translate is listed, not drawn");
    CHECK(!panel.curves()[0].spline && panel.curves()[0].keys.size() == 3, "its samples are its keys");

    QUndoStack* stack = undo->stack(stage);
    int index = stack->index();
    CHECK(panel.convertToCurve(size.GetPath().GetString()) && anim::hasCurve(size) && stack->index() == index + 1,
          "Convert to Curve, one undo step");
    CHECK(panel.curves().size() == 1 && panel.curves()[0].spline, "now a spline");

    // Drag the knot at 10 up by a unit: one undo step.
    CurveView* view = panel.view();
    const QPointF knot = view->pointAt(10.0, 3.0);
    CHECK(view->keyAt(knot) && isNear(view->keyAt(knot)->second, 10.0), "the knot under its point");
    index = stack->index();
    const QPointF up = view->pointAt(10.0, 4.0);
    dragIn(view, knot, up);
    CHECK(isNear(valueAt(size, 10.0), 4.0, 0.05) && stack->index() == index + 1, "dragged to 4, one undo step: %g",
          valueAt(size, 10.0));
    CHECK(panel.isPicked(0, 10.0), "and picked");

    // Its tangent handle after it, dragged to a steeper slope.
    settle();
    const std::optional<QPointF> handle = view->handlePoint(0, 10.0, true);
    CHECK(handle.has_value(), "a picked knot has a tangent handle");
    if (handle) {
        TsKnot before;
        size.GetSpline().GetKnot(10.0, &before);
        double slopeBefore = 0.0;
        before.GetPostTanSlope(&slopeBefore);
        index = stack->index();
        const double handleTime = view->axis().timeAt(handle->x());
        dragIn(view, *handle, view->pointAt(handleTime, valueAt(size, 10.0) + 2.0));
        TsKnot after;
        size.GetSpline().GetKnot(10.0, &after);
        double slopeAfter = 0.0;
        after.GetPostTanSlope(&slopeAfter);
        CHECK(slopeAfter > slopeBefore + 0.01 && stack->index() == index + 1, "a steeper tangent, one undo step: %g -> %g",
              slopeBefore, slopeAfter);
    }

    CHECK(panel.setInterpolation(TsInterpHeld) && isNear(valueAt(size, 15.0), valueAt(size, 10.0)), "held after the knot");
    CHECK(panel.setExtrapolation(true, TsExtrapLoopRepeat) &&
              isNear(valueAt(size, 30.0), valueAt(size, 10.0) + (valueAt(size, 20.0) - valueAt(size, 0.0)), 1e-6),
          "repeats after the last knot");
    CHECK(panel.bakeToSamples(size.GetPath().GetString()) && !anim::hasCurve(size) && size.GetNumTimeSamples() == 21,
          "baked back to samples");

    // The translate split: three curves more.
    CHECK(panel.splitOp(translate.GetAttr().GetPath().GetString(), /*ask=*/false), "split");
    QTest::qWait(200);
    settle();
    CHECK(panel.curves().size() == 4 && panel.otherAttributes().empty(), "now one curve per axis: %d",
          int(panel.curves().size()));
}
