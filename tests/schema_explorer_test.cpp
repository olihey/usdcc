// The Schemas panel (docs/SCHEMAS.md §4): categories, typed schemas nested by
// inheritance, the details and properties of the chosen one, links between
// schemas, the filter and Show, Show Source, and its session state. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/schema_explorer_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_view.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <QUndoStack>

#include <cstdio>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>

using usdcc::usd::SchemaExplorerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;

namespace {
QTreeWidgetItem* topLevel(QTreeWidget* tree, const QString& text) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (tree->topLevelItem(i)->text(0) == text) return tree->topLevelItem(i);
    }
    return nullptr;
}

QTreeWidgetItem* property(QTreeWidget* tree, const QString& name) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (tree->topLevelItem(i)->text(0) == name) return tree->topLevelItem(i);
    }
    return nullptr;
}

QString path(QTreeWidgetItem* item) {
    QStringList names;
    for (; item; item = item->parent()) names.prepend(item->text(0));
    return names.join(QStringLiteral(" / "));
}

int visibleSchemas(QTreeWidget* tree) {
    int count = 0;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if (!(*it)->isHidden() && (*it)->data(0, Qt::UserRole).isValid() && !(*it)->data(0, Qt::UserRole).toString().isEmpty()) {
            bool shown = true;
            for (QTreeWidgetItem* p = (*it)->parent(); p; p = p->parent()) shown = shown && !p->isHidden();
            if (shown) ++count;
        }
    }
    return count;
}

// Until the panel's coalescing timer has counted the stage again.
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 400) QApplication::processEvents();
}

// Clicks the button with this text (without its &) on the message box that opens
// next, and keeps what it said.
void answerNextQuestion(const QString& button, QString* text = nullptr) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, button, text]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        poll->stop();
        poll->deleteLater();
        if (text) *text = box->text() + QLatin1Char('\n') + box->informativeText();
        for (QAbstractButton* candidate : box->buttons()) {
            if (candidate->text().remove(QLatin1Char('&')) == button) candidate->click();
        }
    });
    poll->start();
}

// This stage's row of `name`: a schema it uses, or one it names that USD doesn't know.
QTreeWidgetItem* stageRow(QTreeWidget* tree, const QString& name) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->text(0) != name) continue;
        if ((*it)->data(0, Qt::UserRole + 2).toBool() || (*it)->data(0, Qt::UserRole + 1).isValid()) return *it;
    }
    return nullptr;
}

QTreeWidgetItem* itemFor(QTreeWidget* tree, const QString& name) {
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == name) return *it;
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(schema_explorer_panel) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    SchemaExplorerViewPanel panel(&stages, nullptr, nullptr);
    panel.resize(1000, 700);
    panel.show();
    QTreeWidget* tree = panel.tree();

    CHECK(panel.schemas().size() >= 120, "every schema: [%zu]", panel.schemas().size());
    CHECK(topLevel(tree, "Geometry") && topLevel(tree, "Lighting") && topLevel(tree, "Physics") && topLevel(tree, "Core"),
          "a row per category");
    CHECK(path(itemFor(tree, "Mesh")) == "Geometry / Imageable / Xformable / Boundable / Gprim / PointBased / Mesh",
          "typed schemas nest by inheritance in their category: [%s]", qPrintable(path(itemFor(tree, "Mesh"))));
    CHECK(path(itemFor(tree, "PhysicsRigidBodyAPI")) == "Physics / PhysicsRigidBodyAPI", "API schemas flat in theirs");
    CHECK(itemFor(tree, "Mesh") && itemFor(tree, "Mesh")->text(1) == "Prim type" &&
              itemFor(tree, "Gprim")->text(1) == "Abstract type" && itemFor(tree, "CollectionAPI")->text(1) == "Multiple-apply API",
          "the kind column");
    CHECK(!panel.currentSchema() && panel.details()->toPlainText().contains("Choose a schema"), "nothing chosen yet");
    CHECK(!topLevel(tree, "This stage"), "no stage, no This stage");

    // Details and properties.
    CHECK(panel.selectSchema(QStringLiteral("RectLight")) && panel.currentSchema() &&
              panel.currentSchema()->name == "RectLight",
          "selectSchema()");
    const QString details = panel.details()->toPlainText();
    CHECK(details.contains("Prim type") && details.contains("usdLux") && details.contains("UsdLuxRectLight"), "kind, plugin, TfType");
    CHECK(details.contains("BoundableLightBase") && details.contains("LightAPI") && details.contains("generatedSchema.usda"),
          "inherits, built-in APIs, source: [%s]", qPrintable(details.left(400)));
    QTreeWidgetItem* intensity = property(panel.propertyTree(), "inputs:intensity");
    QTreeWidgetItem* width = property(panel.propertyTree(), "inputs:width");
    CHECK(intensity && intensity->text(3) == "LightAPI" && intensity->text(1) == "float" && intensity->text(2) == "1",
          "a property: type, fallback, declared by");
    CHECK(width && width->text(3) == "RectLight", "its own");
    QTreeWidgetItem* extent = property(panel.propertyTree(), "extent");
    CHECK(extent && extent->text(3) == "Boundable", "an inherited one");
    QTreeWidgetItem* purpose = property(panel.propertyTree(), "purpose");
    CHECK(purpose && purpose->text(1) == "uniform token" && purpose->toolTip(0).contains("Allowed: default, render, proxy, guide"),
          "uniform and allowed tokens");

    // Links: in the details, and a property's declaring schema.
    panel.openLink(QUrl(QStringLiteral("schema:LightAPI")));
    CHECK(panel.currentSchema() && panel.currentSchema()->name == "LightAPI", "a link goes to its schema");
    CHECK(panel.details()->toPlainText().contains("Built into") && panel.details()->toPlainText().contains("RectLight"),
          "an API says what it's built into");
    panel.selectSchema(QStringLiteral("Mesh"));
    if (QTreeWidgetItem* points = property(panel.propertyTree(), "points")) Q_EMIT panel.propertyTree()->itemActivated(points, 0);
    CHECK(panel.currentSchema() && panel.currentSchema()->name == "PointBased", "double-click a property: its declaring schema");
    CHECK(panel.selectSchema(QStringLiteral("CollectionAPI:lightLink")) && panel.currentSchema()->name == "CollectionAPI",
          "an instance names its schema");
    CHECK(panel.details()->toPlainText().contains("collection:name:"), "a multiple-apply API's property namespace");
    CHECK(!panel.selectSchema(QStringLiteral("NotASchema")), "an unknown one isn't chosen");

    // The filter and Show.
    const int all = visibleSchemas(tree);
    panel.setFilterText(QStringLiteral("rigidbody"));
    CHECK(visibleSchemas(tree) == 1 && !itemFor(tree, "PhysicsRigidBodyAPI")->isHidden() && itemFor(tree, "Mesh")->isHidden(),
          "the filter, case-insensitive: [%d]", visibleSchemas(tree));
    panel.setFilterText(QStringLiteral("Mesh"));
    CHECK(!itemFor(tree, "Mesh")->isHidden() && !itemFor(tree, "Gprim")->isHidden(), "a match keeps its ancestors");
    CHECK(itemFor(tree, "Gprim")->foreground(0) != itemFor(tree, "Mesh")->foreground(0), "dimmed, as they don't match");
    panel.setFilterText(QString());
    panel.setShow(SchemaExplorerViewPanel::Show::APISchemas);
    CHECK(itemFor(tree, "Mesh")->isHidden() && !itemFor(tree, "LightAPI")->isHidden(), "Show: API schemas");
    panel.setShow(SchemaExplorerViewPanel::Show::PrimTypes);
    CHECK(!itemFor(tree, "Mesh")->isHidden() && itemFor(tree, "LightAPI")->isHidden(), "Show: prim types");
    CHECK(panel.selectSchema(QStringLiteral("LightAPI")) && visibleSchemas(tree) == all, "going to a hidden one clears the filter");

    // Session state.
    panel.setFilterText(QStringLiteral("Light"));
    const QJsonObject state = panel.saveSessionState();
    CHECK(state.value("schema").toString() == "LightAPI" && state.value("filter").toString() == "Light", "saved");
    SchemaExplorerViewPanel copy(&stages, nullptr, nullptr);
    copy.restoreSessionState(state);
    CHECK(copy.currentSchema() && copy.currentSchema()->name == "LightAPI", "and restored");
    panel.setFilterText(QString());

    // Show Source.
    usdcc::usd::UsdaPreviewDialog* dialog = panel.showSource(QStringLiteral("Mesh"));
    CHECK(dialog && dialog->path().endsWith("usdGeom/schema.usda"), "Show Source opens schema.usda: [%s]",
          dialog ? qPrintable(dialog->path()) : "none");
    if (dialog) {
        // Its sublayers (@usd/schema.usda@, ...) resolve in the plugins' resources,
        // as usdGenSchema finds them.
        QElapsedTimer timer;
        timer.start();
        while (!dialog->isStageLoaded() && timer.elapsed() < 20000) QApplication::processEvents();
        CHECK(dialog->isStageLoaded() && dialog->view()->stage() &&
                  dialog->view()->stage()->usdStage()->GetRootLayer()->GetSubLayerPaths().size() == 1 &&
                  dialog->view()->stage()->usdStage()->GetLayerStack().size() >= 3,
              "its stage has the sublayers it names");
        dialog->close();
    }
    usdcc::usd::UsdaPreviewDialog* generated = panel.showSource(QStringLiteral("Mesh"), true);
    CHECK(generated && generated->path().endsWith("usdGeom/resources/generatedSchema.usda"), "and the generated one");
    if (generated) generated->close();
    CHECK(!panel.showSource(QStringLiteral("NotASchema")), "nothing for an unknown one");
}

// This stage (docs/SCHEMAS.md §4): its counts follow edits, its unregistered
// names, Select, Create where the Create menu would, and Apply to Selection with
// the Attribute Editor's questions, each one undo step.
USDCC_TEST(schema_explorer_stage) {
    using namespace PXR_NS;
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    s->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    s->DefinePrim(SdfPath("/World/Cube"), TfToken("Cube"));
    s->DefinePrim(SdfPath("/World/Mesh"), TfToken("Mesh"));
    s->DefinePrim(SdfPath("/Thing"), TfToken("StudioThing"));
    s->DefinePrim(SdfPath("/Tagged"), TfToken("Xform")).AddAppliedSchema(TfToken("NotARealAPI"));
    s->DefinePrim(SdfPath("/Proto"), TfToken("Scope"));
    s->DefinePrim(SdfPath("/Proto/Ball"), TfToken("Sphere"));
    const UsdPrim instance = s->DefinePrim(SdfPath("/Instance"));
    instance.GetReferences().AddInternalReference(SdfPath("/Proto"));
    instance.SetInstanceable(true);
    QUndoStack* stack = undo->stack(stage);

    SchemaExplorerViewPanel panel(&stages, stage, undo);
    panel.resize(1000, 700);
    panel.show();
    QTreeWidget* tree = panel.tree();

    QTreeWidgetItem* group = topLevel(tree, "This stage");
    CHECK(group && tree->indexOfTopLevelItem(group) == tree->topLevelItemCount() - 1, "This stage, last");
    CHECK(group && group->isExpanded(), "open, as something isn't registered");
    CHECK(stageRow(tree, "Cube") && stageRow(tree, "Cube")->text(1) == "1 prim", "a schema it uses, with its count");
    CHECK(stageRow(tree, "Sphere") && stageRow(tree, "Sphere")->text(1) == "2 prims", "inside instances too");
    CHECK(stageRow(tree, "StudioThing") && stageRow(tree, "NotARealAPI") &&
              stageRow(tree, "StudioThing")->parent()->text(0) == "Not registered",
          "what USD doesn't know, under Not registered");

    // A schema's use and what can be done with it.
    panel.selectSchema(QStringLiteral("Cube"));
    QString details = panel.details()->toPlainText();
    CHECK(details.contains("On this stage") && details.contains("1 prim") && details.contains("Create at the root"),
          "its count, and where Create puts one: [%s]", qPrintable(details.left(300)));
    CHECK(panel.selectPrimsUsing(QStringLiteral("Cube")) &&
              stages.selectedPaths(stage) == std::vector<SdfPath>{SdfPath("/World/Cube")},
          "Select");
    CHECK(panel.details()->toPlainText().contains("under /World/Cube"), "Create follows the selection");

    // Create: under the selected prim, one undo step, selected.
    const SdfPath created = panel.createPrim(QStringLiteral("Cube"));
    CHECK(created == SdfPath("/World/Cube/Cube") && s->GetPrimAtPath(created).GetTypeName() == "Cube", "Create: [%s]",
          created.GetText());
    CHECK(stack->undoText() == "Create Cube", "one undo step, as the Create menu's: [%s]", qPrintable(stack->undoText()));
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>{created}, "and selected");
    settle();
    CHECK(panel.stageUse().count("Cube") == 2 && stageRow(tree, "Cube")->text(1) == "2 prims", "the count follows");
    CHECK(panel.currentSchema() && panel.currentSchema()->name == "Cube", "still on Cube");
    stack->undo();
    settle();
    CHECK(panel.stageUse().count("Cube") == 1, "and its undo");
    QString why;
    CHECK(panel.createPrim(QStringLiteral("Gprim"), &why).IsEmpty() && why.contains("isn't a prim type"),
          "an abstract type can't be created");

    // Apply to Selection: one step for all, the attributes question asked once.
    stages.setSelectedPaths(stage, {SdfPath("/World/Cube"), SdfPath("/World/Mesh")});
    panel.selectSchema(QStringLiteral("MotionAPI"));
    CHECK(panel.details()->toPlainText().contains("Apply to Selection (2 prims selected)"), "Apply says how many");
    QString asked;
    answerNextQuestion(QStringLiteral("Apply Schema Only"), &asked);
    CHECK(panel.applyToSelection(QStringLiteral("MotionAPI")), "Apply to Selection");
    CHECK(asked.contains("Apply MotionAPI to 2 prims?") && asked.contains("motion:velocityScale"),
          "asks about its attributes: [%s]", qPrintable(asked));
    CHECK(stack->undoText() == "Apply MotionAPI to 2 prims", "one undo step: [%s]", qPrintable(stack->undoText()));
    CHECK(s->GetPrimAtPath(SdfPath("/World/Cube")).HasAPI(TfToken("MotionAPI")) &&
              s->GetPrimAtPath(SdfPath("/World/Mesh")).HasAPI(TfToken("MotionAPI")),
          "both have it");
    settle();
    CHECK(stageRow(tree, "MotionAPI") && stageRow(tree, "MotionAPI")->text(1) == "2 prims", "This stage has it now");

    // Again: neither can take it, so nothing is asked, and it says why.
    answerNextQuestion(QStringLiteral("OK"), &asked);
    CHECK(!panel.applyToSelection(QStringLiteral("MotionAPI")), "nothing to apply to");
    CHECK(asked.contains("any of the 2 prims") && asked.contains("has it already"), "says why: [%s]", qPrintable(asked));
    stack->undo();
    settle();

    // One it can't go on is left out, and named.
    stages.setSelectedPaths(stage, {SdfPath("/World/Mesh"), SdfPath("/Instance/Ball")});
    answerNextQuestion(QStringLiteral("Apply and Add Attributes"), &asked);
    CHECK(panel.applyToSelection(QStringLiteral("MotionAPI")), "applied to what it could");
    CHECK(asked.contains("Apply MotionAPI to /World/Mesh?") && asked.contains("1 prim is left out") &&
              asked.contains("/Instance/Ball"),
          "names the one left out: [%s]", qPrintable(asked));
    CHECK(stack->undoText() == "Apply MotionAPI to Mesh" &&
              s->GetPrimAtPath(SdfPath("/World/Mesh")).GetAttribute(TfToken("motion:velocityScale")).HasAuthoredValue(),
          "one prim, with its attributes");
    stack->undo();
    settle();

    // Show: On This Stage.
    panel.setShow(SchemaExplorerViewPanel::Show::OnStage);
    CHECK(!itemFor(tree, "Cube")->isHidden() && itemFor(tree, "PhysicsRigidBodyAPI")->isHidden(), "Show: On This Stage");
    panel.setShow(SchemaExplorerViewPanel::Show::All);

    // Not registered: what it is, and its prims.
    CHECK(panel.selectUnregistered(QStringLiteral("StudioThing")) && panel.currentUnregistered() == "StudioThing" &&
              !panel.currentSchema(),
          "an unregistered one is chosen");
    details = panel.details()->toPlainText();
    CHECK(details.contains("Not registered") && details.contains("no schema") && details.contains("/Thing"),
          "says what that means, with its prims: [%s]", qPrintable(details.left(300)));
    CHECK(panel.propertyTree()->topLevelItemCount() == 0, "and has no properties");
    panel.openLink(QUrl(QStringLiteral("prim:/Thing")));
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>{SdfPath("/Thing")}, "a prim's link selects it");
    CHECK(panel.selectPrimsUsing(QStringLiteral("NotARealAPI")) &&
              stages.selectedPaths(stage) == std::vector<SdfPath>{SdfPath("/Tagged")},
          "Select works for an unregistered API");
    s->RemovePrim(SdfPath("/Thing"));
    settle();
    CHECK(!stageRow(tree, "StudioThing") && panel.currentUnregistered().isEmpty(), "gone once its prims are");
    CHECK(panel.details()->toPlainText().contains("Choose a schema"), "and its details with it");
}

// Prims that author a codeless API's properties without having it (Promote to
// Schema's second step, docs/SCHEMAS.md §6): the details count them, and Apply
// to Them gives them it in one undo step, their values kept. Needs
// test_data/schemas/usdccTestSchemas registered at startup, which the
// usdcc_ui_schema_adopt test does.
USDCC_TEST(schema_explorer_adopt) {
    using namespace PXR_NS;
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    SchemaExplorerViewPanel panel(&stages, stage, undo);
    if (!panel.selectSchema(QStringLiteral("UsdccTestShotAPI"))) {
        std::printf("  skipped: the usdccTestSchemas plugin isn't registered (run the usdcc_ui_schema_adopt test)\n");
        return;
    }
    const UsdStageRefPtr& s = stage->usdStage();
    for (const char* path : {"/A", "/B"}) {
        s->DefinePrim(SdfPath(path), TfToken("Xform"))
            .CreateAttribute(TfToken("usdccTest:shot:frameStart"), SdfValueTypeNames->Int, /*custom=*/true)
            .Set(990);
    }
    s->DefinePrim(SdfPath("/C"), TfToken("Xform")).AddAppliedSchema(TfToken("UsdccTestShotAPI"));
    s->DefinePrim(SdfPath("/D"), TfToken("Xform"));
    panel.show();
    settle();  // the panel follows the stage's edits
    panel.selectSchema(QStringLiteral("UsdccTestShotAPI"));
    CHECK(panel.primsWithAttributesOf(QStringLiteral("UsdccTestShotAPI")) == std::vector<SdfPath>({SdfPath("/A"), SdfPath("/B")}),
          "the prims with its attributes, not those with it already");
    CHECK(panel.details()->toPlainText().contains("Has its attributes") && panel.details()->toPlainText().contains("Apply to Them"),
          "the details offer it");
    CHECK(panel.applyToPrimsWithItsAttributes(QStringLiteral("UsdccTestShotAPI")), "Apply to Them");
    QUndoStack* stack = undo->stack(stage);
    CHECK(stack->undoText() == "Apply UsdccTestShotAPI to 2 prims", "one undo step: [%s]", qPrintable(stack->undoText()));
    const UsdPrim a = s->GetPrimAtPath(SdfPath("/A"));
    int frame = 0;
    CHECK(a.HasAPI(TfToken("UsdccTestShotAPI")) && a.GetAttribute(TfToken("usdccTest:shot:frameStart")).Get(&frame) && frame == 990,
          "they have it, their values kept");
    std::printf("  a custom attribute a schema now defines reads as custom: %s\n",
                a.GetAttribute(TfToken("usdccTest:shot:frameStart")).IsCustom() ? "yes" : "no");
    CHECK(panel.primsWithAttributesOf(QStringLiteral("UsdccTestShotAPI")).empty(), "none left");
    stack->undo();
    CHECK(!s->GetPrimAtPath(SdfPath("/A")).HasAPI(TfToken("UsdccTestShotAPI")), "undo takes it off again");
}
