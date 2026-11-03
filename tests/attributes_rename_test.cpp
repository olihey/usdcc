// Renaming from the Attribute Editor's Name row: one undo step, the selection
// follows the new path, a name that isn't valid or is taken is refused and
// the field goes back, and an instance proxy's name is read-only. Runs in
// usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUndoStack>

#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

// The Name row's field: the line edit showing `name`.
QLineEdit* nameField(AttributesViewPanel& panel, const QString& name) {
    for (QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->text() == name) return edit;
    }
    return nullptr;
}

bool rename(AttributesViewPanel& panel, const QString& from, const QString& to) {
    QLineEdit* field = nameField(panel, from);
    if (!field) return false;
    field->setText(to);
    Q_EMIT field->editingFinished();
    settle();
    return true;
}
}  // namespace

USDCC_TEST(attributes_rename_prim) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    s->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    s->DefinePrim(SdfPath("/World/Chair"), TfToken("Cube"));
    s->DefinePrim(SdfPath("/World/Table"), TfToken("Cube"));
    s->DefinePrim(SdfPath("/src"), TfToken("Xform"));
    s->DefinePrim(SdfPath("/src/geo"), TfToken("Cube"));
    const UsdPrim inst = s->DefinePrim(SdfPath("/inst"), TfToken("Xform"));
    inst.GetReferences().AddInternalReference(SdfPath("/src"));
    inst.SetInstanceable(true);

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(400, 600);
    panel.show();
    stages.setSelectedPaths(stage, {SdfPath("/World/Chair")});
    settle();

    CHECK(rename(panel, "Chair", "Stool"), "the Name row is an editable field showing the name");
    CHECK(s->GetPrimAtPath(SdfPath("/World/Stool")) && !s->GetPrimAtPath(SdfPath("/World/Chair")), "renamed");
    const std::vector<SdfPath> selected = stages.selectedPaths(stage);
    CHECK(selected.size() == 1 && selected.front() == SdfPath("/World/Stool"), "the selection follows the new path");
    CHECK(nameField(panel, "Stool"), "the panel shows the renamed prim");
    QUndoStack* stack = undo->stack(stage);
    CHECK(stack && stack->count() == 1 && stack->undoText() == "Rename Chair to Stool", "one undo step: [%s]",
          stack ? qPrintable(stack->undoText()) : "?");

    CHECK(rename(panel, "Stool", "Table"), "a sibling's name");
    CHECK(s->GetPrimAtPath(SdfPath("/World/Stool")) && stack->count() == 1, "is refused");
    CHECK(nameField(panel, "Stool"), "and the field shows the old name again");
    CHECK(rename(panel, "Stool", "not valid") && s->GetPrimAtPath(SdfPath("/World/Stool")) && nameField(panel, "Stool"),
          "so is a name that isn't valid");

    stack->undo();
    CHECK(s->GetPrimAtPath(SdfPath("/World/Chair")), "undo brings the old name back");

    stages.setSelectedPaths(stage, {SdfPath("/inst/geo")});
    settle();
    QLineEdit* proxy = nameField(panel, "geo");
    CHECK(proxy && proxy->isReadOnly(), "an instance proxy's name is read-only");
    QStandardPaths::setTestModeEnabled(false);
}
