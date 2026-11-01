// UndoManager: discrete apply() edits and edit sessions (one undo step per
// drag, cancel restores), including a real gizmo drag through ScaleTool.
#include "test_harness.h"

#include "usdcc/tools/scale_tool.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <QUndoStack>

using namespace PXR_NS;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;

namespace {

double sizeOf(const StageRefPtr& stage) {
    double v = 0.0;
    UsdGeomCube(stage->usdStage()->GetPrimAtPath(SdfPath("/Cube"))).GetSizeAttr().Get(&v);
    return v;
}

void setSize(const StageRefPtr& stage, double v) {
    UsdGeomCube(stage->usdStage()->GetPrimAtPath(SdfPath("/Cube"))).GetSizeAttr().Set(v);
}

StageRefPtr newStage(StageManager& manager) {
    StageRefPtr stage = manager.createStage();
    UsdGeomCube::Define(stage->usdStage(), SdfPath("/Cube")).GetSizeAttr().Set(1.0);
    return stage;
}

}  // namespace

USDCC_TEST(undo_apply_and_sessions) {
    StageManager manager;
    UndoManager undo;

    {
        StageRefPtr stage = newStage(manager);
        QUndoStack* stack = undo.stack(stage);
        CHECK(undo.apply(stage, "Set size", [&]() { setSize(stage, 2.0); return true; }), "apply succeeds");
        CHECK(stack->count() == 1, "apply pushes one step (count=%d)", stack->count());
        stack->undo();
        CHECK(sizeOf(stage) == 1.0, "undo restores (size=%g)", sizeOf(stage));
        stack->redo();
        CHECK(sizeOf(stage) == 2.0, "redo reapplies (size=%g)", sizeOf(stage));

        CHECK(!undo.apply(stage, "Failing", [&]() { setSize(stage, 9.0); return false; }), "failed apply reports false");
        CHECK(sizeOf(stage) == 2.0, "failed apply is rolled back (size=%g)", sizeOf(stage));
        CHECK(stack->count() == 1, "failed apply pushes nothing");
    }

    {
        // A drag: many live values, one undo step.
        StageRefPtr stage = newStage(manager);
        QUndoStack* stack = undo.stack(stage);
        undo.beginEdit(stage, "Drag");
        CHECK(undo.isEditing(stage), "session open");
        for (int i = 1; i <= 50; ++i) {
            setSize(stage, 1.0 + i * 0.1);
        }
        CHECK(undo.endEdit(stage), "session with changes reports true");
        CHECK(!undo.isEditing(stage), "session closed");
        CHECK(stack->count() == 1, "50 sets -> one step (count=%d)", stack->count());
        CHECK(stack->text(0) == "Drag", "step is named after the session");
        stack->undo();
        CHECK(sizeOf(stage) == 1.0, "undo returns to the value before the drag (size=%g)", sizeOf(stage));
    }

    {
        StageRefPtr stage = newStage(manager);
        QUndoStack* stack = undo.stack(stage);
        undo.beginEdit(stage, "No-op");
        CHECK(!undo.endEdit(stage), "unchanged session reports false");
        CHECK(stack->count() == 0, "unchanged session pushes nothing");

        undo.beginEdit(stage, "Cancelled");
        setSize(stage, 5.0);
        undo.cancelEdit(stage);
        CHECK(sizeOf(stage) == 1.0, "cancel restores (size=%g)", sizeOf(stage));
        CHECK(stack->count() == 0, "cancel pushes nothing");
        CHECK(!undo.isEditing(stage), "cancel closes the session");
    }

    {
        // apply() inside a session is folded into it.
        StageRefPtr stage = newStage(manager);
        QUndoStack* stack = undo.stack(stage);
        undo.beginEdit(stage, "Outer");
        setSize(stage, 3.0);
        undo.apply(stage, "Inner", [&]() { setSize(stage, 4.0); return true; });
        CHECK(stack->count() == 0, "nothing pushed while the outer session is open");
        undo.endEdit(stage);
        CHECK(stack->count() == 1 && stack->text(0) == "Outer", "one step, the outer session's");
        stack->undo();
        CHECK(sizeOf(stage) == 1.0, "undo removes both (size=%g)", sizeOf(stage));
    }

    {
        // Stacks are per stage.
        StageRefPtr a = newStage(manager);
        StageRefPtr b = newStage(manager);
        undo.apply(a, "A", [&]() { setSize(a, 2.0); return true; });
        CHECK(undo.stack(a) != undo.stack(b), "separate stacks");
        CHECK(undo.stack(b)->count() == 0, "edit on A not in B's stack");
    }
}

USDCC_TEST(undo_gizmo_drag) {
    // What the viewport does: the press reports startsEdit, the drag edits,
    // release closes the session. The press itself must not author anything.
    StageManager manager;
    UndoManager undo;
    StageRefPtr stage = newStage(manager);
    const UsdStageRefPtr usd = stage->usdStage();

    GfFrustum frustum;
    GfMatrix4d look;
    look.SetLookAt(GfVec3d(6, 5, 8), GfVec3d(0.0), GfVec3d(0, 1, 0));
    frustum.SetPerspective(60.0, 1.0, 0.1, 1000.0);
    frustum.SetPositionAndRotationFromMatrix(look.GetInverse());
    usdcc::tools::ToolContext ctx;
    ctx.stage = usd;
    ctx.selection = {SdfPath("/Cube")};
    ctx.viewMatrix = frustum.ComputeViewMatrix();
    ctx.projMatrix = frustum.ComputeProjectionMatrix();
    ctx.cameraPos = frustum.GetPosition();
    auto ndc = [&](const GfVec3d& p) {
        const GfVec3d n = (ctx.viewMatrix * ctx.projMatrix).Transform(p);
        return GfVec2d(n[0], n[1]);
    };

    usdcc::tools::ScaleTool scale;
    const double gizmoScale = (ctx.cameraPos - GfVec3d(0.0)).GetLength() * 0.15;
    const GfVec3d onX(0.5 * gizmoScale, 0, 0);
    std::string before;
    usd->GetRootLayer()->ExportToString(&before);
    const auto press = scale.mousePress(ctx, ndc(onX), frustum.ComputePickRay(ndc(onX)));
    CHECK(press.handled && press.startsEdit, "press on a handle starts an edit");
    std::string afterPress;
    usd->GetRootLayer()->ExportToString(&afterPress);
    CHECK(afterPress == before, "the press authored nothing");

    undo.beginEdit(stage, "Scale");
    for (int i = 1; i <= 5; ++i) {
        const GfVec3d p = onX + GfVec3d(0.4 * i, 0, 0);
        scale.mouseMove(ctx, ndc(p), frustum.ComputePickRay(ndc(p)));
    }
    scale.mouseRelease();
    CHECK(undo.endEdit(stage), "the drag changed the stage");
    bool resets = false;
    CHECK(UsdGeomXformable(usd->GetPrimAtPath(SdfPath("/Cube"))).GetOrderedXformOps(&resets).size() == 1,
          "drag created a scale op");
    undo.stack(stage)->undo();
    CHECK(UsdGeomXformable(usd->GetPrimAtPath(SdfPath("/Cube"))).GetOrderedXformOps(&resets).empty(),
          "one undo removes the whole drag, op included");

    const auto miss = scale.mousePress(ctx, GfVec2d(0.9, -0.9), frustum.ComputePickRay(GfVec2d(0.9, -0.9)));
    CHECK(!miss.startsEdit, "a selection click starts no edit");
    scale.mouseRelease();
}
