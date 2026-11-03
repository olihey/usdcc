// The Layers panel (docs/LAYER_EDITOR.md §2, §3): its rows, setting the edit
// target from the Target column, muting and unmuting as undo steps, muting
// the target moving it to the root layer (and undo moving it back), and a
// muted in-memory layer keeping its edits. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/layer_editor_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>

using usdcc::usd::LayerEditorViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// Past the panel's coalescing timer.
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 250) QApplication::processEvents();
}
}  // namespace

USDCC_TEST(layer_editor_panel) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    SdfLayerRefPtr anim = SdfLayer::CreateAnonymous("anim.usda");
    s->GetRootLayer()->GetSubLayerPaths().push_back(anim->GetIdentifier());
    const std::string animId = anim->GetIdentifier();
    const SdfLayerHandle animLayer = anim;
    anim.Reset();  // the stage (and the panel's mute command) hold it from here

    LayerEditorViewPanel panel(&stages, stage, undo);
    panel.resize(500, 400);
    panel.show();
    auto* tree = panel.findChild<QTreeWidget*>();
    CHECK(tree && tree->topLevelItemCount() == 2, "the session layer and the root layer");
    QTreeWidgetItem* animItem = panel.itemFor(animId);
    CHECK(animItem && animItem->parent() == panel.itemFor(s->GetRootLayer()->GetIdentifier()) &&
              animItem->text(0).contains("(in memory)"),
          "the sublayer under the root, in memory");
    if (!animItem) return;

    // The Target column sets the edit target.
    Q_EMIT tree->itemClicked(animItem, LayerEditorViewPanel::Target);
    settle();
    CHECK(stage->editTargetLayer() == animLayer, "clicking Target makes it the edit target");
    CHECK(!panel.itemFor(animId)->icon(LayerEditorViewPanel::Target).isNull(), "and the mark moves there");
    s->DefinePrim(SdfPath("/Tweak"));
    CHECK(animLayer->GetPrimAtPath(SdfPath("/Tweak")), "edits go to it");

    // Mute the target: it moves to the root; the layer and its edits stay.
    QUndoStack* stack = undo->stack(stage);
    CHECK(usdcc::usd::layer_actions::setMuted(*undo, stage, animId, true), "mute");
    settle();
    CHECK(s->IsLayerMuted(animId) && stage->editTargetLayer() == s->GetRootLayer(), "muted; the target is the root");
    CHECK(!s->GetPrimAtPath(SdfPath("/Tweak")), "its prim is gone from the stage");
    CHECK(stack && stack->undoText() == "Mute anim.usda", "an undo step: [%s]", stack ? qPrintable(stack->undoText()) : "");
    CHECK(panel.itemFor(animId) && panel.itemFor(animId)->checkState(LayerEditorViewPanel::Mute) == Qt::Checked,
          "the panel shows it muted");
    stack->undo();
    settle();
    CHECK(!s->IsLayerMuted(animId) && s->GetPrimAtPath(SdfPath("/Tweak")), "undo: unmuted, its edit back");
    CHECK(stage->editTargetLayer() == animLayer, "and the target back on it");

    // The Mute checkbox.
    panel.itemFor(animId)->setCheckState(LayerEditorViewPanel::Mute, Qt::Checked);
    settle();
    CHECK(s->IsLayerMuted(animId), "the checkbox mutes it");
    CHECK(!usdcc::usd::layer_actions::setMuted(*undo, stage, s->GetRootLayer()->GetIdentifier(), true),
          "the root layer can't be muted");
    QStandardPaths::setTestModeEnabled(false);
}
