// The Layers panel (docs/LAYER_EDITOR.md §2, §3): its rows, setting the edit
// target from the Target column, muting and unmuting as undo steps, muting
// the target moving it to the root layer (and undo moving it back), and a
// muted in-memory layer keeping its edits; adding, removing and reordering
// layers. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/layer_editor_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>
#include <set>

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
    CHECK(panel.itemFor(animId) && panel.itemFor(animId)->data(LayerEditorViewPanel::Mute, Qt::UserRole + 1).toBool() &&
              !panel.itemFor(animId)->icon(LayerEditorViewPanel::Mute).isNull() &&
              !panel.itemFor(animId)->data(LayerEditorViewPanel::Mute, Qt::CheckStateRole).isValid(),
          "the panel shows it muted, with an icon, no checkbox");
    {
        // As big as the Outliner's eye: 16 px, every row tall enough for it.
        const QModelIndex muteCell = tree->indexFromItem(panel.itemFor(animId), LayerEditorViewPanel::Mute);
        CHECK(tree->visualRect(muteCell).height() >= 16 && tree->visualRect(muteCell).width() >= 16,
              "the speaker's cell fits 16 px: [%dx%d]", tree->visualRect(muteCell).width(),
              tree->visualRect(muteCell).height());
    }
    stack->undo();
    settle();
    CHECK(!s->IsLayerMuted(animId) && s->GetPrimAtPath(SdfPath("/Tweak")), "undo: unmuted, its edit back");
    CHECK(stage->editTargetLayer() == animLayer, "and the target back on it");

    // A click on the Mute column's speaker mutes it; another unmutes it.
    CHECK(!panel.itemFor(animId)->data(LayerEditorViewPanel::Mute, Qt::UserRole + 1).toBool(), "shown unmuted");
    Q_EMIT tree->itemClicked(panel.itemFor(animId), LayerEditorViewPanel::Mute);
    settle();
    CHECK(s->IsLayerMuted(animId), "a click on the speaker mutes it");
    CHECK(panel.itemFor(animId)->data(LayerEditorViewPanel::Mute, Qt::UserRole + 1).toBool(), "shown muted");
    Q_EMIT tree->itemClicked(panel.itemFor(animId), LayerEditorViewPanel::Mute);
    settle();
    CHECK(!s->IsLayerMuted(animId), "another click unmutes it");
    Q_EMIT tree->itemClicked(panel.itemFor(s->GetRootLayer()->GetIdentifier()), LayerEditorViewPanel::Mute);
    settle();
    CHECK(!s->IsLayerMuted(s->GetRootLayer()->GetIdentifier()) &&
              panel.itemFor(s->GetRootLayer()->GetIdentifier())->icon(LayerEditorViewPanel::Mute).isNull(),
          "the root layer has no speaker, and a click there does nothing");
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
    {
        panel.refresh();
        QMenu menu;
        panel.populateLayerMenu(&menu, memory);
        bool show = false;
        bool prims = false;
        for (QAction* action : menu.actions()) {
            show = show || (action->text() == "Show USDA" && action->isEnabled());
            prims = prims || (action->text() == "Show Prims" && action->isEnabled());
        }
        CHECK(show && prims, "Show USDA and Show Prims for an in-memory layer too");
    }
    QStandardPaths::setTestModeEnabled(false);
}

// Reload after the file changed outside the app: a clean text layer, one with
// unsaved edits, a new sublayer in the file, and a usdc layer rewritten on disk.
USDCC_TEST(layer_editor_reload_external) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    auto write = [&](const QString& name, const QByteArray& text) {
        QFile file(dir.filePath(name));
        const bool ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size();
        CHECK(ok, "write %s", qPrintable(name));
    };
    write(QStringLiteral("anim.usda"), "#usda 1.0\ndef \"OnDisk\"\n{\n}\n");
    write(QStringLiteral("extra.usda"), "#usda 1.0\ndef \"Extra\"\n{\n}\n");
    write(QStringLiteral("shot.usda"), "#usda 1.0\n(\n    subLayers = [@./anim.usda@]\n)\n");
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const UsdStageRefPtr& s = stage->usdStage();
    namespace actions = usdcc::usd::layer_actions;
    const SdfLayerHandle anim = SdfLayer::Find(dir.filePath(QStringLiteral("anim.usda")).toStdString());
    if (!anim) return;
    const std::string animId = anim->GetIdentifier();
    QUndoStack* stack = undo->stack(stage);
    LayerEditorViewPanel panel(&stages, stage, undo);
    panel.show();

    // A clean layer, its file changed: a new prim and a new sublayer.
    write(QStringLiteral("anim.usda"),
          "#usda 1.0\n(\n    subLayers = [@./extra.usda@]\n)\ndef \"OnDisk\"\n{\n}\ndef \"External\"\n{\n}\n");
    QString error;
    CHECK(actions::reloadLayer(*undo, stage, animId, &error), "reload: %s", qPrintable(error));
    CHECK(s->GetPrimAtPath(SdfPath("/External")) && s->GetPrimAtPath(SdfPath("/Extra")),
          "the file's new prim and its new sublayer's prim compose");
    settle();
    const SdfLayerHandle extra = SdfLayer::Find(dir.filePath(QStringLiteral("extra.usda")).toStdString());
    CHECK(extra && panel.itemFor(extra->GetIdentifier()) &&
              panel.itemFor(extra->GetIdentifier())->parent() == panel.itemFor(animId),
          "the panel shows the new sublayer under anim.usda");
    CHECK(stack->undoText() == "Reload anim.usda", "an undo step: [%s]", qPrintable(stack->undoText()));
    stack->undo();
    CHECK(!s->GetPrimAtPath(SdfPath("/External")) && !s->GetPrimAtPath(SdfPath("/Extra")), "undo: as it was");
    stack->redo();
    CHECK(s->GetPrimAtPath(SdfPath("/External")), "redo: the file's content again");

    // Unsaved edits, and the file changed again meanwhile: the file wins.
    stage->setEditTargetLayer(anim);
    undo->apply(stage, QStringLiteral("Add Tweak"), [&]() { return bool(s->DefinePrim(SdfPath("/Tweak"))); });
    write(QStringLiteral("anim.usda"), "#usda 1.0\ndef \"Second\"\n{\n}\n");
    CHECK(actions::reloadLayer(*undo, stage, animId, &error), "reload: %s", qPrintable(error));
    CHECK(!s->GetPrimAtPath(SdfPath("/Tweak")) && !s->GetPrimAtPath(SdfPath("/External")) &&
              s->GetPrimAtPath(SdfPath("/Second")) && !anim->IsDirty(),
          "the second version of the file, the edit gone");

    // A usdc layer rewritten on disk while the stage uses it.
    const QString crateFile = dir.filePath(QStringLiteral("cache.usdc"));
    {
        SdfLayerRefPtr crate = SdfLayer::CreateNew(crateFile.toStdString());
        SdfCreatePrimInLayer(crate, SdfPath("/Cached"))->SetSpecifier(SdfSpecifierDef);
        crate->Save();
    }
    const std::string cache =
        actions::addExistingLayer(*undo, stage, s->GetRootLayer()->GetIdentifier(), crateFile.toStdString(), &error);
    CHECK(!cache.empty() && s->GetPrimAtPath(SdfPath("/Cached")), "the usdc layer composes: %s", qPrintable(error));
    const QString newCrate = dir.filePath(QStringLiteral("cache_new.usdc"));
    {
        SdfLayerRefPtr crate = SdfLayer::CreateNew(newCrate.toStdString());
        SdfCreatePrimInLayer(crate, SdfPath("/Recached"))->SetSpecifier(SdfSpecifierDef);
        crate->Save();
    }
    // As a tool writing it would: over the old file, in place.
    bool replaced = false;
    {
        QFile source(newCrate);
        QFile target(crateFile);
        if (source.open(QIODevice::ReadOnly) && target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray bytes = source.readAll();
            replaced = target.write(bytes) == bytes.size();
        }
    }
    std::printf("  usdc overwritten while open: %s\n", replaced ? "yes" : "no (the file is in use)");
    if (replaced) {
        CHECK(actions::reloadLayer(*undo, stage, cache, &error), "reload the usdc: %s", qPrintable(error));
        CHECK(s->GetPrimAtPath(SdfPath("/Recached")) && !s->GetPrimAtPath(SdfPath("/Cached")),
              "the usdc's new content");
    }
    QStandardPaths::setTestModeEnabled(false);
}

// Saving (docs/LAYER_EDITOR.md §5) and the layer properties (§6).
USDCC_TEST(layer_editor_save_and_properties) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    {
        QFile anim(dir.filePath(QStringLiteral("anim.usda")));
        if (anim.open(QIODevice::WriteOnly)) anim.write("#usda 1.0\n");
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
    const SdfLayerHandle root = s->GetRootLayer();
    const SdfLayerHandle anim = SdfLayer::Find(dir.filePath(QStringLiteral("anim.usda")).toStdString());
    if (!anim) return;
    const std::string animId = anim->GetIdentifier();
    QUndoStack* stack = undo->stack(stage);

    LayerEditorViewPanel panel(&stages, stage, undo);
    panel.resize(500, 700);
    panel.show();
    int saved = 0;
    auto savedConnection = stage->layersSaved.connect([&saved]() { ++saved; });

    // Save one layer.
    stage->setEditTargetLayer(anim);
    s->DefinePrim(SdfPath("/Walk"));
    CHECK(actions::unsavedLayers(stage).size() == 1 && actions::unsavedLayers(stage)[0] == anim,
          "anim.usda is unsaved");
    CHECK(panel.saveLayer(animId) && !anim->IsDirty() && saved == 1, "Save: clean, with the notice");
    CHECK(SdfLayer::OpenAsAnonymous(anim->GetRealPath())->GetPrimAtPath(SdfPath("/Walk")), "and in the file");
    settle();
    CHECK(!panel.itemFor(animId)->text(0).contains("*"), "the panel drops its *");

    // Save As on a file layer: a copy; the stage keeps the original.
    const QString copy = dir.filePath(QStringLiteral("copy.usda"));
    CHECK(!panel.saveLayerAs(animId, copy).empty() && QFile::exists(copy) && s->HasLocalLayer(anim) &&
              std::vector<std::string>(root->GetSubLayerPaths()) == std::vector<std::string>({"./anim.usda"}),
          "Save As a file layer writes a copy");

    // Save As on an in-memory layer: the file takes its place, and the target.
    QString error;
    const std::string memory = actions::addInMemoryLayer(*undo, stage, root->GetIdentifier(), &error);
    stage->setEditTargetLayer(SdfLayer::Find(memory));
    s->DefinePrim(SdfPath("/Tweak"));
    const std::string file = panel.saveLayerAs(memory, dir.filePath(QStringLiteral("tweaks.usda")));
    CHECK(!file.empty() && root->GetSubLayerPaths()[0] == std::string("./tweaks.usda"),
          "the file replaces it in the sublayers: [%s]", std::string(root->GetSubLayerPaths()[0]).c_str());
    CHECK(stage->editTargetLayer() == SdfLayer::Find(file) && s->GetPrimAtPath(SdfPath("/Tweak")),
          "and is the edit target, with its prim");
    stack->undo();
    CHECK(root->GetSubLayerPaths()[0] == memory, "undo: the in-memory layer back in its place");
    stack->redo();

    // Save All: every unsaved file layer of the stage.
    s->DefinePrim(SdfPath("/More"));
    settle();
    QToolButton* saveAllButton = nullptr;
    for (auto* button : panel.findChildren<QToolButton*>()) {
        if (button->toolTip() == "Save all unsaved layers") saveAllButton = button;
    }
    CHECK(saveAllButton && saveAllButton->isEnabled() && !actions::unsavedLayers(stage).empty(),
          "something unsaved: the toolbar's Save All is on");
    if (saveAllButton) saveAllButton->click();
    settle();
    CHECK(actions::unsavedLayers(stage).empty() && saveAllButton && !saveAllButton->isEnabled(),
          "Save All leaves nothing unsaved, and greys out");
    CHECK(usdcc::usd::confirmUnsavedLayers(&stages, {stage}, nullptr), "nothing unsaved: closing needn't ask");

    // Properties: the selected layer's, each edit one undo step.
    panel.refresh();
    panel.itemFor(animId)->setSelected(true);
    panel.findChild<QTreeWidget*>()->setCurrentItem(panel.itemFor(animId));
    usdcc::usd::properties::PropertyRow* defaultPrim = nullptr;
    usdcc::usd::properties::PropertyRow* timeScale = nullptr;
    for (auto* row : panel.findChildren<usdcc::usd::properties::PropertyRow*>()) {
        if (row->labelText() == "Default Prim") defaultPrim = row;
        if (row->labelText() == "Time Scale") timeScale = row;
    }
    CHECK(defaultPrim && timeScale, "Default Prim and (for a sublayer) Time Scale rows");
    if (!defaultPrim || !timeScale) return;
    CHECK(defaultPrim->editor()->isReadOnly(), "Default Prim is read-only");
    timeScale->editor()->setValue(VtValue(2.0));
    Q_EMIT timeScale->editor()->editFinished();
    CHECK(root->GetSubLayerOffset(1).GetScale() == 2.0, "Time Scale sets its offset in the parent: [%g]",
          root->GetSubLayerOffset(1).GetScale());
    QStandardPaths::setTestModeEnabled(false);
}

// The properties follow the selected layer when it's clicked: a click makes
// a row current before it selects it (to the session layer and back).
// File > Save Stage As: the root layer, unsaved edits included, as a copy;
// relative paths rewritten for the new folder.
USDCC_TEST(layer_actions_save_stage_as) {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath(QStringLiteral("out"));
    {
        QFile anim(dir.filePath(QStringLiteral("anim.usda")));
        if (anim.open(QIODevice::WriteOnly)) anim.write("#usda 1.0\n");
        QFile shot(dir.filePath(QStringLiteral("shot.usda")));
        if (shot.open(QIODevice::WriteOnly)) {
            shot.write("#usda 1.0\n(\n    subLayers = [@./anim.usda@, @/abs/elsewhere.usda@]\n)\n"
                       "def \"Chair\" (references = @./anim.usda@) {}\n");
        }
    }
    StageManager stages;
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const SdfLayerHandle root = stage->usdStage()->GetRootLayer();
    stage->setEditTargetLayer(root);
    stage->usdStage()->DefinePrim(SdfPath("/Unsaved"));
    namespace actions = usdcc::usd::layer_actions;
    QString error;

    // Into another folder: ./anim.usda becomes ../anim.usda; absolute stays.
    const QString other = dir.filePath(QStringLiteral("out/shot_v2.usda"));
    CHECK(actions::saveStageAs(stage, other.toStdString(), &error), "saved: %s", qPrintable(error));
    const SdfLayerRefPtr written = SdfLayer::OpenAsAnonymous(other.toStdString());
    CHECK(written && written->GetPrimAtPath(SdfPath("/Unsaved")), "with the unsaved edit");
    if (written) {
        const std::vector<std::string> subs = written->GetSubLayerPaths();
        CHECK(subs == std::vector<std::string>({"../anim.usda", "/abs/elsewhere.usda"}),
              "sublayers rewritten for the new folder: [%s]", subs.empty() ? "" : subs[0].c_str());
        const SdfPrimSpecHandle chair = written->GetPrimAtPath(SdfPath("/Chair"));
        std::vector<SdfReference> refs;
        if (chair) refs = chair->GetReferenceList().GetAppliedItems();
        CHECK(refs.size() == 1 && refs[0].GetAssetPath() == "../anim.usda", "and the reference");
    }
    CHECK(root->IsDirty() && stage->usdStage()->GetRootLayer() == root, "the stage keeps its own, unsaved, root");

    // The same folder: the paths as they were.
    const QString beside = dir.filePath(QStringLiteral("shot_v3.usdc"));
    CHECK(actions::saveStageAs(stage, beside.toStdString(), &error), "saved as .usdc: %s", qPrintable(error));
    const SdfLayerRefPtr binary = SdfLayer::OpenAsAnonymous(beside.toStdString());
    CHECK(binary && binary->GetSubLayerPaths()[0] == std::string("./anim.usda"), "same folder: paths unchanged");

    // A name USD has no format for.
    CHECK(!actions::saveStageAs(stage, dir.filePath(QStringLiteral("shot.txt")).toStdString(), &error) &&
              !error.isEmpty(),
          "an unknown extension is refused");
}

USDCC_TEST(layer_editor_properties_follow_clicks) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    LayerEditorViewPanel panel(&stages, stage, undo);
    panel.show();
    auto* tree = panel.findChild<QTreeWidget*>();
    auto identifierShown = [&]() {
        for (auto* row : panel.findChildren<usdcc::usd::properties::PropertyRow*>()) {
            if (row->labelText() != "Identifier") continue;
            const VtValue value = row->editor()->value();
            return value.IsHolding<std::string>() ? value.UncheckedGet<std::string>() : std::string();
        }
        return std::string();
    };
    auto click = [&](const std::string& identifier) {
        QTreeWidgetItem* item = panel.itemFor(identifier);
        tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);  // as a mouse press does
        tree->clearSelection();
        item->setSelected(true);
    };
    const std::string root = s->GetRootLayer()->GetIdentifier();
    const std::string session = s->GetSessionLayer()->GetIdentifier();
    click(root);
    CHECK(identifierShown() == root, "the root layer's properties");
    click(session);
    CHECK(identifierShown() == session, "then the session layer's: [%s]", identifierShown().c_str());
    click(root);
    CHECK(identifierShown() == root, "and back to the root layer's: [%s]", identifierShown().c_str());
    QStandardPaths::setTestModeEnabled(false);
}

// The properties fold to their header, and the panel's session state (and a
// duplicate) keeps that, with the sizes to unfold to.
USDCC_TEST(layer_editor_properties_collapse) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    LayerEditorViewPanel panel(&stages, stage, undo);
    panel.resize(500, 600);
    panel.show();
    QApplication::processEvents();
    QSplitter* splitter = nullptr;  // the tree's, not the panel's own
    for (auto* candidate : panel.findChildren<QSplitter*>()) {
        if (candidate->orientation() == Qt::Vertical && candidate->count() == 2) splitter = candidate;
    }
    auto* scroll = splitter ? splitter->widget(1)->findChild<QScrollArea*>() : nullptr;
    CHECK(splitter && scroll && !panel.isPropertiesCollapsed() && scroll->isVisible(), "open at first");
    if (!splitter || !scroll) return;
    splitter->setSizes({300, 250});
    const QList<int> open = splitter->sizes();

    QToolButton* header = nullptr;
    for (auto* button : panel.findChildren<QToolButton*>()) {
        if (button->text() == "Layer Properties") header = button;
    }
    CHECK(header, "a Layer Properties header");
    if (!header) return;
    header->click();
    QApplication::processEvents();
    CHECK(panel.isPropertiesCollapsed() && !scroll->isVisible(), "a click folds it");
    CHECK(splitter->sizes()[1] < 60 && splitter->sizes()[0] > open[0], "to its header; the tree takes the room: [%d, %d]",
          splitter->sizes()[0], splitter->sizes()[1]);

    const QJsonObject state = panel.saveSessionState();
    CHECK(state.value("propertiesCollapsed").toBool(), "the session state says folded");
    std::unique_ptr<usdcc::ui::ViewPanel> copy(panel.duplicate());
    auto* copyPanel = static_cast<LayerEditorViewPanel*>(copy.get());
    CHECK(copyPanel->isPropertiesCollapsed(), "a duplicate is folded too");

    header->click();
    QApplication::processEvents();
    CHECK(!panel.isPropertiesCollapsed() && scroll->isVisible() && splitter->sizes() == open,
          "unfolded to the sizes it had: [%d, %d]", splitter->sizes()[0], splitter->sizes()[1]);
    QStandardPaths::setTestModeEnabled(false);
}

// Up Axis and Meters per Unit: a sublayer authoring other values than the
// stage's gets a warning; Default Prim is read-only, with no button.
USDCC_TEST(layer_editor_metrics_and_default_prim) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    auto write = [&](const QString& name, const QByteArray& text) {
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    };
    write(QStringLiteral("zup.usda"),
          "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 1\n    upAxis = \"Z\"\n)\n"
          "def \"World\"\n{\n    def \"Asset\"\n    {\n    }\n}\n");
    write(QStringLiteral("same.usda"), "#usda 1.0\n(\n    metersPerUnit = 0.01\n    upAxis = \"Y\"\n)\n");
    write(QStringLiteral("plain.usda"), "#usda 1.0\n");
    write(QStringLiteral("shot.usda"),
          "#usda 1.0\n(\n    metersPerUnit = 0.01\n    upAxis = \"Y\"\n"
          "    subLayers = [@./zup.usda@, @./same.usda@, @./plain.usda@]\n)\n");
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    LayerEditorViewPanel panel(&stages, stage, undo);
    panel.resize(500, 900);
    panel.show();
    auto* tree = panel.findChild<QTreeWidget*>();
    using usdcc::usd::properties::PropertyRow;
    auto select = [&](const QString& file) {
        const SdfLayerHandle layer = SdfLayer::Find(dir.filePath(file).toStdString());
        QTreeWidgetItem* item = layer ? panel.itemFor(layer->GetIdentifier()) : nullptr;
        if (item) {
            tree->clearSelection();
            tree->setCurrentItem(item);
        }
        return item != nullptr;
    };
    auto row = [&](const QString& label) -> PropertyRow* {
        for (auto* r : panel.findChildren<PropertyRow*>()) {
            if (r->labelText() == label) return r;
        }
        return nullptr;
    };
    auto shown = [&](const QString& label) {
        PropertyRow* r = row(label);
        const VtValue v = r ? r->editor()->value() : VtValue();
        return v.IsHolding<std::string>() ? QString::fromStdString(v.UncheckedGet<std::string>())
               : v.IsHolding<double>()    ? QString::number(v.UncheckedGet<double>())
                                          : QString();
    };
    auto warning = [&](const QString& label) {
        PropertyRow* r = row(label);
        if (!r) return QString();
        for (auto* l : r->findChildren<QLabel*>()) {
            if (!l->isHidden() && l->text().startsWith("The stage")) return l->text();
        }
        return QString();
    };

    CHECK(select("zup.usda"), "select zup.usda");
    CHECK(shown("Up Axis") == "Z" && shown("Meters per Unit") == "1", "its own values: [%s] [%s]",
          qPrintable(shown("Up Axis")), qPrintable(shown("Meters per Unit")));
    CHECK(warning("Up Axis") == "The stage is Y up", "an up axis warning: [%s]", qPrintable(warning("Up Axis")));
    CHECK(warning("Meters per Unit") == "The stage uses 0.01 (centimeters)", "a units warning: [%s]",
          qPrintable(warning("Meters per Unit")));
    for (const QString& label : {QStringLiteral("Up Axis"), QStringLiteral("Meters per Unit")}) {
        bool amber = false;
        for (auto* l : row(label)->findChildren<QLabel*>()) {
            if (l->text().startsWith("The stage")) amber = l->property("caution").toBool() && !l->property("unresolved").toBool();
        }
        CHECK(amber, "%s: a caution (amber), not an error (red)", qPrintable(label));
    }
    CHECK(row("Up Axis")->editor()->isReadOnly() && row("Meters per Unit")->editor()->isReadOnly(), "both read-only");
    CHECK(shown("Default Prim") == "World" && row("Default Prim")->editor()->isReadOnly(), "Default Prim World, read-only");

    CHECK(row("Default Prim")->findChildren<QToolButton*>().size() == row("Up Axis")->findChildren<QToolButton*>().size(),
          "and no button beside it, as Up Axis has none");

    // The stage's own values, and none at all: no warning.
    CHECK(select("same.usda") && warning("Up Axis").isEmpty() && warning("Meters per Unit").isEmpty(),
          "the same values: no warning");
    CHECK(select("plain.usda") && warning("Up Axis").isEmpty() && warning("Meters per Unit").isEmpty() &&
              shown("Up Axis") == "Y" && shown("Meters per Unit") == "0.01",
          "unauthored: the fallback, no warning: [%s] [%s]", qPrintable(shown("Up Axis")),
          qPrintable(shown("Meters per Unit")));
    CHECK(select("shot.usda") && warning("Up Axis").isEmpty(), "the root layer: no warning");

    // The stage turns Z up: the warnings follow.
    stage->usdStage()->GetRootLayer()->GetPseudoRoot()->SetInfo(UsdGeomTokens->upAxis, VtValue(TfToken("Z")));
    settle();
    CHECK(select("zup.usda") && warning("Up Axis").isEmpty(), "the stage Z up now: no up axis warning");
    CHECK(select("same.usda") && warning("Up Axis") == "The stage is Z up", "but same.usda gets one: [%s]",
          qPrintable(warning("Up Axis")));
    QStandardPaths::setTestModeEnabled(false);
}

// Flatten to File and Flatten Sublayers: offsets applied, muted layers left
// out, a reference in a sublayer still found from where the result is.
USDCC_TEST(layer_actions_flatten) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    for (const char* sub : {"layers", "props", "out"}) QDir(dir.path()).mkpath(QString::fromLatin1(sub));
    auto write = [&](const QString& name, const QByteArray& text) {
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    };
    write(QStringLiteral("props/chair.usda"), "#usda 1.0\n(\n    defaultPrim = \"Chair\"\n)\ndef \"Chair\"\n{\n    def \"Seat\"\n    {\n    }\n}\n");
    write(QStringLiteral("layers/deep.usda"), "#usda 1.0\ndef \"Deep\"\n{\n}\n");
    write(QStringLiteral("layers/muted.usda"), "#usda 1.0\ndef \"Muted\"\n{\n}\n");
    write(QStringLiteral("layers/anim.usda"),
          "#usda 1.0\n(\n    subLayers = [@./deep.usda@]\n)\n"
          "over \"World\"\n{\n    double x.timeSamples = {\n        0: 1,\n        5: 2,\n    }\n}\n"
          "def \"Chair\" (\n    references = @../props/chair.usda@\n)\n{\n}\n");
    write(QStringLiteral("shot.usda"),
          "#usda 1.0\n(\n    defaultPrim = \"World\"\n"
          "    subLayers = [@./layers/anim.usda@ (offset = 10), @./layers/muted.usda@]\n)\ndef \"World\"\n{\n}\n");
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const UsdStageRefPtr& s = stage->usdStage();
    const SdfLayerHandle root = s->GetRootLayer();
    const std::string rootId = root->GetIdentifier();
    const SdfLayerHandle anim = SdfLayer::Find(dir.filePath(QStringLiteral("layers/anim.usda")).toStdString());
    const SdfLayerHandle deep = SdfLayer::Find(dir.filePath(QStringLiteral("layers/deep.usda")).toStdString());
    const std::string mutedId = dir.filePath(QStringLiteral("layers/muted.usda")).toStdString();
    CHECK(anim && deep, "the sublayers are open");
    if (!anim || !deep) return;
    namespace actions = usdcc::usd::layer_actions;
    CHECK(actions::setMuted(*undo, stage, SdfLayer::Find(mutedId)->GetIdentifier(), true), "mute muted.usda");
    QUndoStack* stack = undo->stack(stage);
    QString error;

    // To a file in another folder; the stage stays as it is.
    const QString out = dir.filePath(QStringLiteral("out/flat.usda"));
    CHECK(actions::flattenToFile(stage, rootId, out.toStdString(), &error), "flatten to file: %s", qPrintable(error));
    CHECK(root->GetSubLayerPaths().size() == 2 && !root->IsDirty(), "the stage's root is untouched");
    const SdfLayerRefPtr flat = SdfLayer::OpenAsAnonymous(out.toStdString());
    CHECK(flat && flat->GetSubLayerPaths().empty() && flat->GetDefaultPrim() == TfToken("World"),
          "one layer, no sublayers, the root's metadata");
    if (flat) {
        CHECK(flat->GetPrimAtPath(SdfPath("/Deep")) && !flat->GetPrimAtPath(SdfPath("/Muted")),
              "the nested sublayer in, the muted one out");
        const std::set<double> times = flat->ListTimeSamplesForPath(SdfPath("/World.x"));
        CHECK(times == std::set<double>({10.0, 15.0}), "anim's offset applied to its samples");
        std::vector<SdfReference> refs;
        if (const SdfPrimSpecHandle chair = flat->GetPrimAtPath(SdfPath("/Chair"))) {
            refs = chair->GetReferenceList().GetAddedOrExplicitItems();
            if (refs.empty()) refs = chair->GetReferenceList().GetAppliedItems();
        }
        CHECK(refs.size() == 1 && refs[0].GetAssetPath() == "../props/chair.usda",
              "the reference rewritten for out/: [%s]", refs.empty() ? "" : refs[0].GetAssetPath().c_str());
    }
    const UsdStageRefPtr flatStage = UsdStage::Open(out.toStdString());
    CHECK(flatStage && flatStage->GetPrimAtPath(SdfPath("/Chair/Seat")), "and found from there");
    CHECK(!actions::flattenToFile(stage, rootId, out.toStdString(), &error), "not over a file open as a layer");

    // In place: the root takes its sublayers in; undo brings them back.
    stage->setEditTargetLayer(anim);
    CHECK(actions::flattenSublayers(*undo, stage, rootId, &error), "flatten sublayers: %s", qPrintable(error));
    CHECK(root->GetSubLayerPaths().empty() && stage->editTargetLayer() == root, "no sublayers; the target on the root");
    CHECK(s->GetPrimAtPath(SdfPath("/Deep")) && s->GetPrimAtPath(SdfPath("/Chair/Seat")) &&
              !s->GetPrimAtPath(SdfPath("/Muted")),
          "the stage composes the same");
    CHECK(stack->undoText() == "Flatten Sublayers of shot.usda", "an undo step: [%s]", qPrintable(stack->undoText()));
    {
        std::vector<SdfReference> refs;
        if (const SdfPrimSpecHandle chair = root->GetPrimAtPath(SdfPath("/Chair"))) {
            refs = chair->GetReferenceList().GetAddedOrExplicitItems();
        }
        CHECK(refs.size() == 1 && refs[0].GetAssetPath() == "./props/chair.usda", "the reference for the root's folder: [%s]",
              refs.empty() ? "" : refs[0].GetAssetPath().c_str());
    }
    stack->undo();
    CHECK(root->GetSubLayerPaths().size() == 2 && root->GetSubLayerOffset(0) == SdfLayerOffset(10) &&
              !root->GetPrimAtPath(SdfPath("/Chair")) && s->GetPrimAtPath(SdfPath("/Deep")),
          "undo: the sublayers back, with the offset");
    CHECK(s->IsLayerMuted(SdfLayer::Find(mutedId)->GetIdentifier()), "muted.usda still muted");

    // What can be flattened, and the menu.
    const std::vector<usdcc::usd::LayerEntry> tree = usdcc::usd::layerTree(s);
    CHECK(actions::canFlatten(tree[1]) && !actions::canFlatten(tree[1].children[0].children[0]),
          "the root can, deep.usda (no sublayers) can't");
    LayerEditorViewPanel panel(&stages, stage, undo);
    auto enabled = [&](const std::string& identifier, const QString& text) {
        QMenu menu;
        panel.populateLayerMenu(&menu, identifier);
        for (QAction* action : menu.actions()) {
            if (action->text() == text) return action->isEnabled();
        }
        return false;
    };
    CHECK(enabled(rootId, "Flatten Sublayers") && !enabled(deep->GetIdentifier(), "Flatten Sublayers") &&
              enabled(deep->GetIdentifier(), "Flatten to File..."),
          "the menu's Flatten Sublayers and Flatten to File...");
    QStandardPaths::setTestModeEnabled(false);
}

// Mute Others: everything but the layer, the layers over it and its own
// sublayers; the outermost only; one undo step; the target moves to the root.
USDCC_TEST(layer_actions_mute_others) {
    // Its own scale controller for the panel below: without it the panel would
    // make a global one that outlives the test (scale_controller_test needs none).
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    // root: a (a1, a2), b (b1), c; session: s1
    std::vector<SdfLayerRefPtr> keep;
    auto add = [&](const SdfLayerHandle& parent, const char* name) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(name);
        parent->GetSubLayerPaths().push_back(layer->GetIdentifier());
        keep.push_back(layer);
        return std::string(layer->GetIdentifier());
    };
    const std::string a = add(s->GetRootLayer(), "a.usda");
    const std::string b = add(s->GetRootLayer(), "b.usda");
    const std::string c = add(s->GetRootLayer(), "c.usda");
    const std::string a1 = add(SdfLayer::Find(a), "a1.usda");
    const std::string a2 = add(SdfLayer::Find(a), "a2.usda");
    const std::string b1 = add(SdfLayer::Find(b), "b1.usda");
    const std::string s1 = add(s->GetSessionLayer(), "s1.usda");
    namespace actions = usdcc::usd::layer_actions;
    actions::setMuted(*undo, stage, c, true);
    stage->setEditTargetLayer(SdfLayer::Find(b1));
    QUndoStack* stack = undo->stack(stage);
    const int before = stack->count();

    CHECK(actions::canMuteOthers(stage, a1) && actions::muteOthers(*undo, stage, a1), "Mute Others of a1");
    CHECK(s->IsLayerMuted(a2) && s->IsLayerMuted(b) && s->IsLayerMuted(s1) && s->IsLayerMuted(c),
          "a2, b, the session's s1 muted; c stays muted");
    CHECK(!s->IsLayerMuted(a1) && !s->IsLayerMuted(a) && !s->IsLayerMuted(b1),
          "a1 and a play; b1 goes with b, not muted itself");
    CHECK(stage->editTargetLayer() == s->GetRootLayer(), "the target, in b1, moved to the root");
    CHECK(stack->count() == before + 1 && stack->undoText() == "Mute All but a1.usda", "one undo step: [%s]",
          qPrintable(stack->undoText()));
    CHECK(!actions::canMuteOthers(stage, a1), "nothing more to mute");
    stack->undo();
    CHECK(!s->IsLayerMuted(a2) && !s->IsLayerMuted(b) && !s->IsLayerMuted(s1) && s->IsLayerMuted(c),
          "undo: all back but c, muted before");
    CHECK(stage->editTargetLayer() == SdfLayer::Find(b1), "and the target back on b1");

    // On a muted layer: it plays again, the others go.
    CHECK(actions::muteOthers(*undo, stage, c), "Mute Others of c, itself muted");
    CHECK(!s->IsLayerMuted(c) && s->IsLayerMuted(a) && s->IsLayerMuted(b) && s->IsLayerMuted(s1) &&
              !s->IsLayerMuted(a1),
          "c unmuted; a, b and s1 muted (a1 goes with a)");
    CHECK(!actions::canMuteOthers(stage, s->GetRootLayer()->GetIdentifier()), "not for the root layer");

    // In the menu.
    LayerEditorViewPanel panel(&stages, stage, undo);
    QMenu menu;
    panel.populateLayerMenu(&menu, c);
    bool listed = false;
    for (QAction* action : menu.actions()) listed = listed || action->text() == "Mute Others";
    CHECK(listed, "Mute Others in a layer's menu");
    QStandardPaths::setTestModeEnabled(false);
}
