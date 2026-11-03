// Editing composition arcs (docs/COMPOSITION_ARCS.md §4): prim_actions'
// addArc/changeArc/removeArc on references, payloads and inherits, each one
// undo step, keeping the entry's place and custom data; arcs from another
// layer stay read-only; and the Attribute Editor's Composition section edits
// them, its Prim field with the defaultPrim placeholder and the prim picker
// dialog. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/composition_arcs.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_picker_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

#include <algorithm>
#include <memory>

using usdcc::usd::ArcInfo;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_actions = usdcc::usd::prim_actions;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
std::vector<ArcInfo> arcsOf(const StageRefPtr& stage, const char* path, ArcInfo::Type type) {
    std::vector<ArcInfo> arcs = usdcc::usd::compositionArcs(stage->usdStage()->GetPrimAtPath(SdfPath(path)));
    arcs.erase(std::remove_if(arcs.begin(), arcs.end(), [type](const ArcInfo& a) { return a.type != type; }), arcs.end());
    return arcs;
}

void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}
}  // namespace

USDCC_TEST(composition_edit_actions) {
    StageManager stages;
    UndoManager undo;
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    s->DefinePrim(SdfPath("/Chair"), TfToken("Xform"));
    s->DefinePrim(SdfPath("/_class"), TfToken("Xform"));
    const SdfPath chair("/Chair");
    QUndoStack* stack = undo.stack(stage);

    // Add a reference; the file needn't exist to be authored.
    CHECK(prim_actions::addArc(undo, stage, chair, ArcInfo::Type::Reference, "./chair.usda", SdfPath()),
          "add a reference");
    std::vector<ArcInfo> refs = arcsOf(stage, "/Chair", ArcInfo::Type::Reference);
    CHECK(refs.size() == 1 && refs[0].assetPath == "./chair.usda" && refs[0].editable && refs[0].listPosition == "prepended",
          "it's there, editable, prepended");
    CHECK(stack->undoText() == "Add Reference to Chair", "one undo step: [%s]", qPrintable(stack->undoText()));

    // Change it: another file, a prim and an offset, in place, keeping its custom data.
    {
        SdfReference withData = SdfReference("./chair.usda");
        withData.SetCustomData(VtDictionary{{"note", VtValue(std::string("keep me"))}});
        s->GetEditTarget().GetPrimSpecForScenePath(chair)->GetReferenceList().ReplaceItemEdits(SdfReference("./chair.usda"),
                                                                                                withData);
    }
    refs = arcsOf(stage, "/Chair", ArcInfo::Type::Reference);
    ArcInfo changed = refs[0];
    changed.assetPath = "./table.usda";
    changed.primPath = SdfPath("/Table");
    changed.layerOffset = SdfLayerOffset(5.0, 2.0);
    CHECK(prim_actions::changeArc(undo, stage, chair, refs[0], changed), "change it");
    refs = arcsOf(stage, "/Chair", ArcInfo::Type::Reference);
    CHECK(refs.size() == 1 && refs[0].assetPath == "./table.usda" && refs[0].primPath == SdfPath("/Table") &&
              refs[0].layerOffset == SdfLayerOffset(5.0, 2.0) && refs[0].listPosition == "prepended",
          "changed in place");
    const SdfReferenceVector items =
        s->GetRootLayer()->GetPrimAtPath(chair)->GetReferenceList().GetPrependedItems();
    CHECK(items.size() == 1 && items[0].GetCustomData().count("note"), "its custom data kept");
    stack->undo();
    CHECK(arcsOf(stage, "/Chair", ArcInfo::Type::Reference)[0].assetPath == "./chair.usda", "undo: the old file");
    stack->redo();

    // Remove it.
    refs = arcsOf(stage, "/Chair", ArcInfo::Type::Reference);
    CHECK(prim_actions::removeArc(undo, stage, chair, refs[0]) && arcsOf(stage, "/Chair", ArcInfo::Type::Reference).empty(),
          "remove it");
    stack->undo();
    CHECK(arcsOf(stage, "/Chair", ArcInfo::Type::Reference).size() == 1, "undo brings it back");

    // An inherit; a payload.
    CHECK(prim_actions::addArc(undo, stage, chair, ArcInfo::Type::Inherit, "", SdfPath("/_class")), "add an inherit");
    std::vector<ArcInfo> inherits = arcsOf(stage, "/Chair", ArcInfo::Type::Inherit);
    ArcInfo moved = inherits.at(0);
    moved.primPath = SdfPath("/_other");
    CHECK(prim_actions::changeArc(undo, stage, chair, inherits[0], moved) &&
              arcsOf(stage, "/Chair", ArcInfo::Type::Inherit)[0].primPath == SdfPath("/_other"),
          "change its path");
    CHECK(!prim_actions::addArc(undo, stage, chair, ArcInfo::Type::Inherit, "", SdfPath("relative")),
          "a relative path is refused");
    CHECK(prim_actions::addArc(undo, stage, chair, ArcInfo::Type::Payload, "./heavy.usda", SdfPath()) &&
              arcsOf(stage, "/Chair", ArcInfo::Type::Payload).size() == 1,
          "add a payload");

    // An arc from a sublayer is read-only here.
    const SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("sub.usda");
    s->GetRootLayer()->GetSubLayerPaths().push_back(sub->GetIdentifier());
    SdfPrimSpecHandle subSpec = SdfCreatePrimInLayer(sub, SdfPath("/Lamp"));
    subSpec->SetSpecifier(SdfSpecifierDef);
    subSpec->GetReferenceList().Prepend(SdfReference("./lamp.usda"));
    const std::vector<ArcInfo> lamp = arcsOf(stage, "/Lamp", ArcInfo::Type::Reference);
    CHECK(lamp.size() == 1 && !lamp[0].editable && !lamp[0].nested, "a sublayer's reference: listed, read-only");
    ArcInfo other = lamp.at(0);
    other.assetPath = "./other.usda";
    CHECK(!prim_actions::changeArc(undo, stage, SdfPath("/Lamp"), lamp[0], other) &&
              !prim_actions::removeArc(undo, stage, SdfPath("/Lamp"), lamp[0]),
          "and can't be changed or removed from here");
}

USDCC_TEST(composition_section_in_attribute_editor) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdPrim chair = stage->usdStage()->DefinePrim(SdfPath("/Chair"), TfToken("Xform"));
    chair.GetReferences().AddReference(SdfReference("./chair.usda"));

    usdcc::usd::AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(500, 800);
    panel.show();
    stages.setSelectedPaths(stage, {SdfPath("/Chair")});
    settle();

    bool title = false;
    for (QWidget* widget : panel.findChildren<QWidget*>()) {
        if (auto* label = qobject_cast<QLabel*>(widget); label && label->text() == "Reference") title = true;
    }
    CHECK(title, "a Reference row in the Composition section");
    QLineEdit* file = nullptr;
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->text() == "./chair.usda") file = edit;
    }
    CHECK(file, "showing its file");
    if (!file) return;
    file->setText(QStringLiteral("./table.usda"));
    Q_EMIT file->editingFinished();
    settle();
    const std::vector<ArcInfo> refs = arcsOf(stage, "/Chair", ArcInfo::Type::Reference);
    CHECK(refs.size() == 1 && refs[0].assetPath == "./table.usda", "editing the field changes the reference: [%s]",
          refs.empty() ? "none" : refs[0].assetPath.c_str());
    QUndoStack* stack = undo->stack(stage);
    CHECK(stack && stack->undoText() == "Change Reference on Chair", "as one undo step: [%s]",
          stack ? qPrintable(stack->undoText()) : "?");

    // One actions button per arc, its menu Remove, Open as Stage, Reveal.
    QToolButton* actions = nullptr;
    for (QToolButton* button : panel.findChildren<QToolButton*>()) {
        if (button->toolTip() == "Reference actions") actions = button;
    }
    QStringList entries;
    if (actions && actions->menu()) {
        for (QAction* action : actions->menu()->actions()) entries << action->text();
    }
    CHECK(entries == QStringList({"Remove", "Open as Stage", "Reveal in Explorer"}), "the arc's actions: [%s]",
          qPrintable(entries.join(", ")));

    // The Prim field: "defaultPrim" while empty, and its own actions.
    bool placeholder = false;
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->placeholderText() == "defaultPrim" && edit->text().isEmpty()) placeholder = true;
    }
    CHECK(placeholder, "an empty Prim says defaultPrim");
    QToolButton* primActions = nullptr;
    for (QToolButton* button : panel.findChildren<QToolButton*>()) {
        if (button->toolTip() == "Prim actions") primActions = button;
    }
    entries.clear();
    if (primActions && primActions->menu()) {
        for (QAction* action : primActions->menu()->actions()) entries << action->text();
    }
    CHECK(entries == QStringList({"Select Prim from Stage..."}), "no prim set: only Select: [%s]",
          qPrintable(entries.join(", ")));
    QStandardPaths::setTestModeEnabled(false);
}

USDCC_TEST(prim_picker_dialog) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/Creature"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Creature/Body"), TfToken("Mesh"));
    stage->DefinePrim(SdfPath("/Creature/Groom"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/Creature/Groom/Hair"), TfToken("BasisCurves"));
    stage->DefinePrim(SdfPath("/Lights"), TfToken("Scope"));
    stage->SetDefaultPrim(stage->GetPrimAtPath(SdfPath("/Creature")));

    usdcc::usd::PrimPickerDialog empty(stage, SdfPath());
    CHECK(empty.visiblePrimCount() == 5, "every prim: [%d]", empty.visiblePrimCount());
    CHECK(empty.selectedPath() == SdfPath("/Creature"), "no prim set: the default prim selected");

    usdcc::usd::PrimPickerDialog dialog(stage, SdfPath("/Creature/Groom"));
    CHECK(dialog.selectedPath() == SdfPath("/Creature/Groom"), "the current prim selected");
    dialog.setFilter(QStringLiteral("hair"));
    CHECK(dialog.visiblePrimCount() == 3, "filter: the match and its ancestors: [%d]", dialog.visiblePrimCount());
    dialog.selectPath(SdfPath("/Creature/Groom/Hair"));
    CHECK(dialog.selectedPath() == SdfPath("/Creature/Groom/Hair"), "a filtered prim can be picked");
    dialog.setFilter(QString());
    CHECK(dialog.visiblePrimCount() == 5, "no filter: all again");
    QStandardPaths::setTestModeEnabled(false);
}
