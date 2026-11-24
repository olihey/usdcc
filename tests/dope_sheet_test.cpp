// Key editing (docs/ANIMATION.md phase 3): the Dope Sheet's rows, picking keys by click and by
// box, moving and scaling them by dragging, deleting, copying and pasting, each one undo
// step; and dragging, deleting, copying and pasting a key mark on the timeline's ruler. Runs
// in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/anim_edit.h"
#include "usdcc/usd/dope_sheet_view_panel.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/timeline_bar.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>

#include <QApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
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

void mouse(QWidget* widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, position, widget->mapToGlobal(position), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
}

void key(QWidget* widget, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers);
    QApplication::sendEvent(widget, &event);
}

bool isNear(double a, double b) { return std::abs(a - b) < 1e-6; }

// A box with its translate keyed at 0 and 10 and its size at 10 and 20.
struct Scene {
    StageManager stages;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    StageRefPtr stage = stages.createStage();
    UsdGeomCube cube = UsdGeomCube::Define(stage->usdStage(), SdfPath("/Box"));
    UsdAttribute translate;
    UsdAttribute size;

    Scene() {
        UsdGeomXformOp op = cube.AddTranslateOp();
        op.Set(GfVec3d(0, 0, 0), 0.0);
        op.Set(GfVec3d(4, 0, 0), 10.0);
        translate = op.GetAttr();
        size = cube.GetSizeAttr();
        size.Set(1.0, 10.0);
        size.Set(2.0, 20.0);
        stages.setSelectedPaths(stage, {SdfPath("/Box")});
    }
};

// The centre of a row's key at `time`, in the view.
QPointF keyPoint(DopeSheetViewPanel& panel, int row, double time) {
    const std::pair<int, int> span = *panel.rowSpan(row);
    const QPoint top = panel.view()->mapFromGlobal(panel.tree()->viewport()->mapToGlobal(QPoint(0, span.first)));
    return QPointF(panel.view()->axis().pixelAt(time), top.y() + (span.second - span.first) / 2.0);
}

int rowOf(const DopeSheetViewPanel& panel, const char* attribute) {
    for (int i = 0; i < int(panel.rows().size()); ++i) {
        const DopeSheetViewPanel::Row& row = panel.rows()[i];
        if (!row.isPrim && row.attributes.front().GetName() == attribute) return i;
    }
    return -1;
}
}  // namespace

USDCC_TEST(dope_sheet_rows) {
    TestSettings settings;
    Scale scale;
    Scene scene;
    DopeSheetViewPanel panel(&scene.stages, scene.stage, scene.undo);
    panel.resize(900, 400);
    panel.show();
    QTest::qWait(200);
    settle();
    CHECK(panel.rows().size() == 3, "a prim row and a row per animated attribute: %d", int(panel.rows().size()));
    CHECK(panel.rows()[0].isPrim && panel.rows()[0].keys == std::vector<double>({0.0, 10.0, 20.0}),
          "the prim row has every attribute's keys");
    const int sizeRow = rowOf(panel, "size");
    CHECK(sizeRow > 0 && panel.rows()[sizeRow].keys == std::vector<double>({10.0, 20.0}), "the size row its own");
    CHECK(panel.rowSpan(0) && panel.rowSpan(sizeRow), "rows have a place in the tree");
    const TimeRange shown = panel.view()->axis().range();
    CHECK(shown.start < 0.0 && shown.end > 20.0 && panel.ruler()->range() == shown,
          "the keys are framed, and the ruler shows the same times (%g to %g)", shown.start, shown.end);

    scene.stages.setSelectedPaths(scene.stage, {});
    QTest::qWaitFor([&] { return panel.rows().empty(); });  // the panel's timer may be late
    CHECK(panel.rows().empty(), "nothing selected: no rows");
    panel.setShowAll(true);
    CHECK(panel.rows().size() == 3, "Show All Animated: every animated prim");
    const QJsonObject state = panel.saveSessionState();
    DopeSheetViewPanel restored(&scene.stages, scene.stage, scene.undo);
    restored.restoreSessionState(state);
    CHECK(restored.showsAll(), "kept in a session");
}

USDCC_TEST(dope_sheet_edits_keys) {
    TestSettings settings;
    Scale scale;
    Scene scene;
    DopeSheetViewPanel panel(&scene.stages, scene.stage, scene.undo);
    panel.resize(900, 400);
    panel.show();
    QTest::qWait(200);
    settle();
    QUndoStack* stack = scene.undo->stack(scene.stage);
    DopeSheetView* view = panel.view();
    const int sizeRow = rowOf(panel, "size");

    // A click picks the size row's key at 20; a drag of 5 frames moves it, one undo step.
    const QPointF at20 = keyPoint(panel, sizeRow, 20.0);
    const auto hit = view->keyAt(at20);
    CHECK(hit && hit->first == sizeRow && isNear(hit->second, 20.0), "the key is under its point");
    const int steps = stack->count();
    mouse(view, QEvent::MouseButtonPress, at20, Qt::LeftButton, Qt::LeftButton);
    CHECK(panel.pickedKeys().size() == 1 && panel.isPicked(sizeRow, 20.0), "picked by a click");
    const QPointF to25(view->axis().pixelAt(25.0), at20.y());
    mouse(view, QEvent::MouseMove, to25, Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, to25, Qt::LeftButton, Qt::NoButton);
    CHECK(anim::keyTimes({scene.size}) == std::vector<double>({10.0, 25.0}) && stack->count() == steps + 1,
          "dragged to 25, one undo step");
    double value = 0.0;
    scene.size.Get(&value, 25.0);
    CHECK(isNear(value, 2.0) && panel.isPicked(sizeRow, 25.0), "with its value, still picked");
    stack->undo();
    CHECK(anim::keyTimes({scene.size}) == std::vector<double>({10.0, 20.0}), "undone");
    panel.refreshKeys();

    // A box over the prim row from 5 to 15 picks every attribute's key at 10.
    const QPointF from(view->axis().pixelAt(5.0), keyPoint(panel, 0, 10.0).y() - 3.0);
    const QPointF to(view->axis().pixelAt(15.0), keyPoint(panel, 0, 10.0).y() + 3.0);
    mouse(view, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    CHECK(panel.pickedKeys().size() == 2 && panel.isPicked(0, 10.0), "the box picks both keys at 10: %d",
          int(panel.pickedKeys().size()));

    // Scaling: pick everything (0, 10, 20), drag the right handle from 20 to 40: about 0, twice as long.
    panel.pickAll();
    CHECK(panel.pickedKeys().size() == 4 && view->scaleHandle(true), "all four keys, with scale handles");
    const QPointF handle = view->scaleHandle(true)->center();
    const int before = stack->index();
    mouse(view, QEvent::MouseButtonPress, handle, Qt::LeftButton, Qt::LeftButton);
    const QPointF to40(view->axis().pixelAt(40.0), handle.y());
    mouse(view, QEvent::MouseMove, to40, Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, to40, Qt::LeftButton, Qt::NoButton);
    CHECK(anim::keyTimes({scene.translate}) == std::vector<double>({0.0, 20.0}) &&
              anim::keyTimes({scene.size}) == std::vector<double>({20.0, 40.0}) && stack->index() == before + 1,
          "scaled about the first key, one undo step");

    // Copy the size keys, paste them at 100; then delete what's picked.
    panel.clearPicked();
    panel.pickKeysIn({sizeRow}, 0.0, 50.0, false);
    view->setFocus();
    key(view, Qt::Key_C, Qt::ControlModifier);
    CHECK(!anim::keyClipboard().empty(), "copied");
    scene.stage->setTime(100.0);
    key(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(anim::keyTimes({scene.size}) == std::vector<double>({20.0, 40.0, 100.0, 120.0}), "pasted at the frame shown");
    panel.clearPicked();
    panel.pickKeysIn({rowOf(panel, "size")}, 90.0, 130.0, false);
    key(view, Qt::Key_Delete);
    CHECK(anim::keyTimes({scene.size}) == std::vector<double>({20.0, 40.0}) && panel.pickedKeys().empty(), "deleted");
}

// The ruler's key marks: dragged, deleted, copied and pasted, for the selection.
USDCC_TEST(dope_sheet_ruler_keys) {
    TestSettings settings;
    Scale scale;
    Scene scene;
    PlaybackController playback(&scene.stages);
    TimelineBar bar(&playback, scene.undo);
    bar.resize(1000, 52);
    bar.show();
    QTest::qWait(200);
    settle();
    TimeRuler* ruler = bar.ruler();
    CHECK(ruler->keys() == std::vector<double>({0.0, 10.0, 20.0}), "the selection's keys");
    QUndoStack* stack = scene.undo->stack(scene.stage);
    const int steps = stack->count();
    const double y = ruler->height() - 4.0;
    const QPointF at10(ruler->axis().pixelAt(10.0), y);
    CHECK(ruler->keyAt(at10) && isNear(*ruler->keyAt(at10), 10.0), "the mark at 10");
    mouse(ruler, QEvent::MouseButtonPress, at10, Qt::LeftButton, Qt::LeftButton);
    const QPointF at14(ruler->axis().pixelAt(14.0), y);
    mouse(ruler, QEvent::MouseMove, at14, Qt::NoButton, Qt::LeftButton);
    mouse(ruler, QEvent::MouseButtonRelease, at14, Qt::LeftButton, Qt::NoButton);
    CHECK(anim::keyTimes({scene.translate}) == std::vector<double>({0.0, 14.0}) &&
              anim::keyTimes({scene.size}) == std::vector<double>({14.0, 20.0}) && stack->count() == steps + 1,
          "dragging a mark moves every key of the selection there, one step");
    CHECK(ruler->selectedKey() && isNear(*ruler->selectedKey(), 14.0), "and it stays picked");

    ruler->setFocus();
    key(ruler, Qt::Key_C, Qt::ControlModifier);
    scene.stage->setTime(50.0);
    key(ruler, Qt::Key_V, Qt::ControlModifier);
    CHECK(anim::keyTimes({scene.translate}) == std::vector<double>({0.0, 14.0, 50.0}) &&
              anim::keyTimes({scene.size}) == std::vector<double>({14.0, 20.0, 50.0}),
          "copied and pasted at the frame shown");
    key(ruler, Qt::Key_Delete);
    CHECK(anim::keyTimes({scene.size}) == std::vector<double>({20.0, 50.0}) &&
              anim::keyTimes({scene.translate}) == std::vector<double>({0.0, 50.0}),
          "Delete removes the picked mark's keys");
}
