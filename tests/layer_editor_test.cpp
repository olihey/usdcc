// The Layers panel (docs/LAYER_EDITOR.md §2, §3): its rows, setting the edit
// target from the Target column, muting and unmuting as undo steps, muting
// the target moving it to the root layer (and undo moving it back), and a
// muted in-memory layer keeping its edits; adding, removing and reordering
// layers. Runs in usdcc_ui_tests (offscreen).
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
#include <QFile>
#include <QMenu>
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

// Adding, removing and reordering layers (docs/LAYER_EDITOR.md §4).
USDCC_TEST(layer_editor_add_remove_move) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    {
        QFile file(dir.filePath(QStringLiteral("shot.usda")));
        if (file.open(QIODevice::WriteOnly)) file.write("#usda 1.0\n");
        QFile base(dir.filePath(QStringLiteral("base.usda")));
        if (base.open(QIODevice::WriteOnly)) base.write("#usda 1.0\n");
    }
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const UsdStageRefPtr& s = stage->usdStage();
    const SdfLayerHandle root = s->GetRootLayer();
    const std::string rootId = root->GetIdentifier();
    QUndoStack* stack = undo->stack(stage);
    namespace actions = usdcc::usd::layer_actions;
    auto sublayers = [&]() { return std::vector<std::string>(root->GetSubLayerPaths()); };

    // A new file: written, added with a relative path, one undo step on the root layer.
    QString error;
    const std::string anim =
        actions::addNewLayerFile(*undo, stage, rootId, dir.filePath(QStringLiteral("anim.usda")).toStdString(), &error);
    CHECK(!anim.empty() && QFile::exists(dir.filePath(QStringLiteral("anim.usda"))), "a new file, written: %s",
          qPrintable(error));
    CHECK(sublayers() == std::vector<std::string>({"./anim.usda"}), "added as ./anim.usda");
    CHECK(stack->undoText() == "Add anim.usda", "an undo step: [%s]", qPrintable(stack->undoText()));
    stack->undo();
    CHECK(sublayers().empty(), "undo takes it out");
    stack->redo();
    CHECK(sublayers().size() == 1 && s->HasLocalLayer(SdfLayer::Find(anim)), "redo puts it back");

    // Existing and in-memory ones go on top; one already there is refused.
    const std::string base =
        actions::addExistingLayer(*undo, stage, rootId, dir.filePath(QStringLiteral("base.usda")).toStdString(), &error);
    CHECK(!base.empty() && sublayers().front() == "./base.usda", "an existing file, as the strongest");
    CHECK(actions::addExistingLayer(*undo, stage, rootId, dir.filePath(QStringLiteral("base.usda")).toStdString(),
                                    &error)
                  .empty() &&
              error.contains("already"),
          "not twice: [%s]", qPrintable(error));
    const std::string memory = actions::addInMemoryLayer(*undo, stage, rootId, &error);
    const SdfLayerHandle memoryLayer = SdfLayer::Find(memory);
    CHECK(memoryLayer && memoryLayer->GetDisplayName() == "Untitled 1" && sublayers().size() == 3,
          "an in-memory one, Untitled 1");
    CHECK(actions::addInMemoryLayer(*undo, stage, memory, &error) != "" &&
              SdfLayer::Find(memory)->GetSubLayerPaths().size() == 1,
          "a sublayer of an in-memory layer");
    stack->undo();

    // Removing the target (with edits only it has): the target moves to the
    // parent, and undo brings back the layer with its edits.
    stage->setEditTargetLayer(memoryLayer);
    s->DefinePrim(SdfPath("/OnlyHere"));
    CHECK(actions::removeLayer(*undo, stage, memory), "remove it");
    CHECK(stage->editTargetLayer() == root && !s->GetPrimAtPath(SdfPath("/OnlyHere")),
          "the target moved to the root; its prim gone");
    stack->undo();
    CHECK(s->GetPrimAtPath(SdfPath("/OnlyHere")), "undo: back, with its prim");

    // Reorder: [Untitled 1, base, anim] -> anim first, keeping its offset.
    root->SetSubLayerOffset(SdfLayerOffset(10, 2), 2);
    CHECK(actions::moveLayer(*undo, stage, anim, 0), "move anim to the top");
    CHECK(sublayers().front() == "./anim.usda" && root->GetSubLayerOffset(0) == SdfLayerOffset(10, 2),
          "first, with its offset");
    CHECK(stack->undoText() == "Move anim.usda Up", "[%s]", qPrintable(stack->undoText()));
    stack->undo();
    CHECK(sublayers().back() == "./anim.usda", "undo: last again");
    CHECK(!actions::moveLayer(*undo, stage, rootId, 0) && !actions::removeLayer(*undo, stage, rootId),
          "the root layer can't be moved or removed");

    // The panel: + adds under the selection and, by default, edits the new layer.
    LayerEditorViewPanel panel(&stages, stage, undo);
    panel.show();
    const std::string added = panel.addLayer(LayerEditorViewPanel::AddKind::InMemory, rootId);
    settle();
    CHECK(!added.empty() && stage->editTargetLayer() == SdfLayer::Find(added), "a new layer becomes the target");
    CHECK(panel.itemFor(added) && panel.itemFor(added)->isSelected(), "and is selected");
    QStandardPaths::setTestModeEnabled(false);
}

// Reload: a file layer fresh from disk, its unsaved edits gone; undo brings
// them back, except for a layer that streams its data (usdc), whose reload
// clears the undo history instead.
USDCC_TEST(layer_editor_reload) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    {
        QFile anim(dir.filePath(QStringLiteral("anim.usda")));
        if (anim.open(QIODevice::WriteOnly)) anim.write("#usda 1.0\ndef \"OnDisk\"\n{\n}\n");
        QFile shot(dir.filePath(QStringLiteral("shot.usda")));
        if (shot.open(QIODevice::WriteOnly)) shot.write("#usda 1.0\n(\n    subLayers = [@./anim.usda@]\n)\n");
    }
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const UsdStageRefPtr& s = stage->usdStage();
    namespace actions = usdcc::usd::layer_actions;
    const SdfLayerHandle anim = SdfLayer::Find(dir.filePath(QStringLiteral("anim.usda")).toStdString());
    CHECK(anim, "anim.usda is open");
    if (!anim) return;
    const std::string animId = anim->GetIdentifier();
    QUndoStack* stack = undo->stack(stage);

    stage->setEditTargetLayer(anim);
    undo->apply(stage, QStringLiteral("Add Tweak"), [&]() { return bool(s->DefinePrim(SdfPath("/Tweak"))); });
    CHECK(anim->IsDirty() && s->GetPrimAtPath(SdfPath("/Tweak")), "an unsaved edit in anim.usda");

    QString error;
    CHECK(actions::reloadLayer(*undo, stage, animId, &error), "reload: %s", qPrintable(error));
    CHECK(!anim->IsDirty() && !s->GetPrimAtPath(SdfPath("/Tweak")) && s->GetPrimAtPath(SdfPath("/OnDisk")),
          "the edit is gone, the file's prim there, not dirty");
    CHECK(stack->undoText() == "Reload anim.usda", "an undo step: [%s]", qPrintable(stack->undoText()));
    stack->undo();
    CHECK(s->GetPrimAtPath(SdfPath("/Tweak")) && anim->IsDirty(), "undo: the edit back, dirty again");
    stack->undo();
    CHECK(!s->GetPrimAtPath(SdfPath("/Tweak")), "and the older step still undoes");
    stack->redo();
    stack->redo();
    CHECK(!s->GetPrimAtPath(SdfPath("/Tweak")), "redo reloads again");

    // A usdc layer: reloading can't be undone, so the history goes.
    const std::string crateFile = dir.filePath(QStringLiteral("cache.usdc")).toStdString();
    {
        SdfLayerRefPtr crate = SdfLayer::CreateNew(crateFile);
        SdfCreatePrimInLayer(crate, SdfPath("/Cached"))->SetSpecifier(SdfSpecifierDef);
        crate->Save();
    }
    const std::string cache = actions::addExistingLayer(*undo, stage, s->GetRootLayer()->GetIdentifier(), crateFile, &error);
    const std::vector<usdcc::usd::LayerEntry> tree = usdcc::usd::layerTree(s);
    const usdcc::usd::LayerEntry& cacheEntry = tree[1].children.front();
    CHECK(cacheEntry.identifier == cache && actions::canReload(cacheEntry) && !actions::reloadIsUndoable(cacheEntry),
          "a usdc layer: reloadable, not as an undo step");
    CHECK(stack->count() > 0 && actions::reloadLayer(*undo, stage, cache, &error) && stack->count() == 0,
          "its reload clears the undo history");

    // In-memory layers have nothing to reload from.
    const std::string memory = actions::addInMemoryLayer(*undo, stage, s->GetRootLayer()->GetIdentifier(), &error);
    CHECK(!actions::reloadLayer(*undo, stage, memory, &error), "an in-memory layer can't be reloaded");
    LayerEditorViewPanel panel(&stages, stage, undo);
    auto hasReload = [&](const std::string& identifier) {
        QMenu menu;
        panel.populateLayerMenu(&menu, identifier);
        for (QAction* action : menu.actions()) {
            if (action->text() == "Reload") return true;
        }
        return false;
    };
    CHECK(hasReload(animId) && !hasReload(memory), "Reload in the menu of a file layer only");
    QStandardPaths::setTestModeEnabled(false);
}
