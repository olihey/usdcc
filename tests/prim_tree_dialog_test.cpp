// Show Prims (docs/SHOW_PRIMS.md): a look-only tree with the Outliner's icons on
// a private stage, its name filter, Load Payloads, instances, one dialog per
// file, a broken file, closing during a load, and a layer shared with an open
// stage. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QAbstractItemModel>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeView>

#include <functional>

using usdcc::usd::PrimTreeDialog;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void wait(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QApplication::processEvents();
}

bool waitForStage(const PrimTreeDialog& dialog) {
    QElapsedTimer timer;
    timer.start();
    while (!dialog.isStageLoaded() && timer.elapsed() < 20000) QApplication::processEvents();
    return dialog.isStageLoaded();
}

// The row showing a name, anywhere in the tree as it's loaded.
QModelIndex findRow(QAbstractItemModel* model, const QString& name, const QModelIndex& parent = QModelIndex()) {
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (index.data().toString() == name) return index;
        const QModelIndex below = findRow(model, name, index);
        if (below.isValid()) return below;
    }
    return {};
}
}  // namespace

USDCC_TEST(prim_tree_model_read_only) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    usdcc::usd::UsdPrimTreeModel model;
    model.setStage(stage);
    const QModelIndex world = model.index(0, 0);
    CHECK((model.flags(world) & Qt::ItemIsEditable) && model.data(world, Qt::CheckStateRole).isValid(),
          "the Outliner's rows: editable, with a checkbox");
    model.setReadOnly(true);
    CHECK(!(model.flags(world) & Qt::ItemIsEditable) && !(model.flags(world) & Qt::ItemIsUserCheckable) &&
              !model.data(world, Qt::CheckStateRole).isValid(),
          "read-only: neither");
    CHECK(!model.setData(world, QStringLiteral("Other"), Qt::EditRole) &&
              !model.setData(world, Qt::Unchecked, Qt::CheckStateRole) && stage->GetPrimAtPath(SdfPath("/World")).IsActive(),
          "and edits are refused");
    CHECK(!model.data(world, Qt::DecorationRole).value<QIcon>().isNull(), "the icon stays");
}

USDCC_TEST(prim_tree_dialog) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    auto write = [&dir](const char* name, const char* text) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    };
    write("heavy.usda",
          "#usda 1.0\n(\n    defaultPrim = \"Heavy\"\n)\ndef Xform \"Heavy\"\n{\n    def Cube \"HeavyPart\"\n    {\n    }\n}\n");
    write("part.usda", "#usda 1.0\n(\n    defaultPrim = \"Part\"\n)\ndef Xform \"Part\"\n{\n    def Cube \"InsideInstance\"\n    {\n    }\n}\n");
    write("chair.usda",
          "#usda 1.0\n\ndef Xform \"Chair\"\n{\n"
          "    def Cube \"Seat\"\n    {\n        def Sphere \"Cushion\"\n        {\n        }\n    }\n"
          "    def Cube \"Back\"\n    {\n    }\n"
          "    def Xform \"Load\" (\n        payload = @./heavy.usda@\n    )\n    {\n    }\n"
          "    def \"Inst\" (\n        instanceable = true\n        references = @./part.usda@\n    )\n    {\n    }\n"
          "}\n\ndef Scope \"Looks\"\n{\n}\n");
    write("broken.usda", "#usda 1.0\ndef this is not usda {{{\n");
    write("wood.png", "");
    const QString chair = dir.filePath(QStringLiteral("chair.usda"));

    StageManager stages;
    PrimTreeDialog dialog(chair);
    dialog.show();
    CHECK(dialog.statusLabel()->text().contains("Loading"), "says it's loading");
    CHECK(waitForStage(dialog) && dialog.stage(), "the stage loads");
    wait(20);
    CHECK(stages.stages().empty() && !stages.currentStage(), "StageManager never hears of it");

    QAbstractItemModel* model = dialog.treeView()->model();
    const QModelIndex chairRow = findRow(model, "Chair");
    CHECK(chairRow.isValid() && dialog.treeView()->isExpanded(chairRow), "the root prims start expanded");
    CHECK(!dialog.treeView()->isExpanded(findRow(model, "Seat")), "the rest collapsed");
    CHECK(!chairRow.data(Qt::DecorationRole).value<QIcon>().isNull(), "with the Outliner's icons");
    CHECK(!chairRow.data(Qt::CheckStateRole).isValid() && !(chairRow.flags() & Qt::ItemIsEditable),
          "nothing to check or edit");
    CHECK(dialog.treeView()->contextMenuPolicy() != Qt::CustomContextMenu, "no menu");
    CHECK(dialog.statusLabel()->text().contains("1 payload not loaded"), "the status counts the unloaded payload: [%s]",
          qPrintable(dialog.statusLabel()->text()));

    // The filter: a match and its ancestors, ignoring case; then back to how it was.
    dialog.treeView()->expand(findRow(model, "Seat"));
    dialog.setFilter(QStringLiteral("cush"));
    CHECK(findRow(model, "Cushion").isValid() && findRow(model, "Seat").isValid() && findRow(model, "Chair").isValid(),
          "a match keeps its ancestors");
    CHECK(!findRow(model, "Back").isValid() && !findRow(model, "Looks").isValid(), "the rest go");
    CHECK(dialog.treeView()->isExpanded(findRow(model, "Seat")), "and the match shows");
    CHECK(dialog.statusLabel()->text().startsWith("1 match"), "one match: [%s]", qPrintable(dialog.statusLabel()->text()));
    CHECK(dialog.statusLabel()->text().contains("instances"), "the status says instances aren't searched yet");
    dialog.setFilter(QStringLiteral("nothing"));
    CHECK(model->rowCount() == 0 && dialog.statusLabel()->text().startsWith("0 matches"), "no match: no rows");
    // Esc clears the filter first.
    dialog.reject();
    CHECK(dialog.isVisible() && dialog.findChild<QLineEdit*>()->text().isEmpty() && findRow(model, "Back").isValid(),
          "Esc clears it, the dialog stays");
    CHECK(dialog.treeView()->isExpanded(findRow(model, "Seat")) && !dialog.treeView()->isExpanded(findRow(model, "Cushion")),
          "what was open is open again, and no more");

    // Instance contents load when expanded, and can match then.
    const QModelIndex inst = findRow(model, "Inst");
    CHECK(inst.isValid() && !findRow(model, "InsideInstance").isValid(), "an instance's contents aren't loaded yet");
    dialog.treeView()->expand(inst);
    wait(20);
    CHECK(findRow(model, "InsideInstance").isValid(), "expanding loads them");
    dialog.setFilter(QStringLiteral("InsideInstance"));
    CHECK(findRow(model, "InsideInstance").isValid(), "and the filter finds them");
    dialog.setFilter(QString());

    // Load Payloads, keeping what's open.
    QPushButton* load = nullptr;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == "Load Payloads") load = button;
    }
    CHECK(load && load->isEnabled() && !findRow(model, "HeavyPart").isValid(), "the payload's prims aren't there");
    if (load) load->click();
    wait(50);
    CHECK(findRow(model, "HeavyPart").isValid(), "Load Payloads adds them");
    CHECK(load && !load->isEnabled() && !dialog.statusLabel()->text().contains("payload"), "and nothing's left to load");
    CHECK(dialog.treeView()->isExpanded(findRow(model, "Seat")), "the tree keeps what was open");

    // One dialog per file; not a texture; closing while it loads.
    PrimTreeDialog* first = usdcc::usd::showPrimTree(dir.filePath(QStringLiteral("part.usda")), nullptr);
    CHECK(first && first->isVisible(), "Show Prims opens a dialog");
    CHECK(usdcc::usd::showPrimTree(dir.filePath(QStringLiteral("part.usda")), nullptr) == first, "the same file brings it forward");
    CHECK(!usdcc::usd::showPrimTree(dir.filePath(QStringLiteral("wood.png")), nullptr), "not a texture");
    QPointer<PrimTreeDialog> closing(first);
    if (first) first->close();
    wait(100);
    CHECK(!closing, "closing deletes it");

    // A file that can't load.
    PrimTreeDialog broken(dir.filePath(QStringLiteral("broken.usda")));
    CHECK(waitForStage(broken) && !broken.stage() && broken.statusLabel()->text().contains("Couldn't load"),
          "a broken file says why: [%s]", qPrintable(broken.statusLabel()->text()));

    // Open as a stage of the app too: separate stages, shared layers.
    const StageRefPtr appStage = stages.openStage(chair.toStdString());
    PrimTreeDialog shared(chair);
    shared.show();
    CHECK(waitForStage(shared) && shared.stage()->usdStage() != appStage->usdStage(), "a stage of its own");
    CHECK(stages.selectedPaths(appStage).empty() && stages.stages().size() == 1, "the app's stage is left alone");
    appStage->usdStage()->DefinePrim(SdfPath("/EditedInApp"));
    wait(50);
    CHECK(findRow(shared.treeView()->model(), "EditedInApp").isValid(), "an edit to the shared layer updates the tree");
    QStandardPaths::setTestModeEnabled(false);
}
