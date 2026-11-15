// The viewport's Delete key (docs/PRIM_DELETE.md): the native window gets the key, and
// its panel deletes the stage's selection as the Outliner does. Runs in usdcc_ui_tests.
#include "test_harness.h"

#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/usd/usd/stage.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QUndoStack>

#include <memory>

using usdcc::usd::HydraViewportWindow;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::ViewportViewPanel;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void press(HydraViewportWindow* window, Qt::Key key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
}
}  // namespace

USDCC_TEST(viewport_delete_key) {
    StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& usd = stage->usdStage();
    for (const char* path : {"/World", "/World/A", "/World/B"}) usd->DefinePrim(SdfPath(path), TfToken("Xform"));
    auto panel = std::make_unique<ViewportViewPanel>(&stages, stage, toolSet, undo);
    HydraViewportWindow* window = panel->hydraWindow();

    stages.setSelectedPaths(stage, {SdfPath("/World/B")});
    press(window, Qt::Key_A);
    CHECK(usd->GetPrimAtPath(SdfPath("/World/B")) && undo->stack(stage)->count() == 0, "another key deletes nothing");

    press(window, Qt::Key_Delete);
    CHECK(!usd->GetPrimAtPath(SdfPath("/World/B")) && usd->GetPrimAtPath(SdfPath("/World/A")), "Delete deletes the selection");
    CHECK(stages.selectedPaths(stage).empty(), "and drops it from the selection");
    CHECK(undo->stack(stage)->count() == 1 && undo->stack(stage)->text(0) == QStringLiteral("Delete B"), "one undo step");

    press(window, Qt::Key_Delete);
    CHECK(undo->stack(stage)->count() == 1, "with nothing selected, nothing happens");
}
