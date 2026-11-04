// Relationships in the Attribute Editor (docs/RELATIONSHIPS.md): targets as pills
// that select them and an × that removes them, the target dialog with its filter
// and greyed rows, and adding MaterialBindingAPI with its material. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_tree_widget.h"
#include "usdcc/usd/properties/category_section.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/properties/relationship_targets_editor.h"
#include "usdcc/usd/properties/schema_pills.h"
#include "usdcc/usd/relationship_target_dialog.h"
#include "usdcc/usd/relationship_targets.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>

#include <functional>
#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::PrimTreeWidget;
using usdcc::usd::RelationshipTargetDialog;
using usdcc::usd::RelationshipTargetRule;
using usdcc::usd::RelationshipTargetRules;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::properties::RelationshipTargetsEditor;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

QModelIndex findRow(QAbstractItemModel* model, const QString& name, const QModelIndex& parent = QModelIndex()) {
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (index.data().toString() == name) return index;
        const QModelIndex below = findRow(model, name, index);
        if (below.isValid()) return below;
    }
    return {};
}

// Runs `answer` on the target dialog that opens next.
void answerNextDialog(std::function<void(RelationshipTargetDialog*)> answer) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, answer]() {
        auto* dialog = qobject_cast<RelationshipTargetDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        poll->stop();
        poll->deleteLater();
        answer(dialog);
    });
    poll->start();
}

// The pill of a row's editor showing this path.
QFrame* pillFor(AttributesViewPanel& panel, const QString& path) {
    for (auto* editor : panel.findChildren<RelationshipTargetsEditor*>()) {
        for (QFrame* pill : editor->findChildren<QFrame*>(QStringLiteral("schemaPill"))) {
            auto* link = pill->findChild<QToolButton*>(QStringLiteral("pillLink"));
            if (link && link->text() == path) return pill;
        }
    }
    return nullptr;
}

// The Material section's binding row, by its label: other rows can say None too,
// such as a Mesh's proxyPrim.
RelationshipTargetsEditor* bindingRowOf(AttributesViewPanel& panel) {
    for (auto* row : panel.findChildren<usdcc::usd::properties::PropertyRow*>()) {
        auto* editor = row->editor() ? qobject_cast<RelationshipTargetsEditor*>(row->editor()) : nullptr;
        if (editor && row->labelText() == "Binding" && row->isVisibleTo(&panel)) return editor;
    }
    return nullptr;
}

QAction* findEntry(QMenu* menu, const QString& category, const QString& text) {
    for (QAction* top : menu->actions()) {
        if (top->text() != category || !top->menu()) continue;
        for (QAction* action : top->menu()->actions()) {
            if (action->text() == text) return action;
        }
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(relationship_target_dialog) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/Looks"), TfToken("Scope"));
    stage->DefinePrim(SdfPath("/Looks/Wood"), TfToken("Material"));
    stage->DefinePrim(SdfPath("/Looks/Steel"), TfToken("Material"));
    stage->DefinePrim(SdfPath("/Box"), TfToken("Mesh"));

    RelationshipTargetDialog dialog(stage, "material:binding", SdfPath("/Looks/Steel"));
    dialog.show();
    PrimTreeWidget* tree = dialog.tree();
    QAbstractItemModel* model = tree->treeView()->model();
    CHECK(tree->selectedPath() == SdfPath("/Looks/Steel") && dialog.useButton()->isEnabled(), "the current target selected");

    // A row the rule blocks can't be selected, but is still there, and opens.
    const QModelIndex wood = findRow(model, "Wood");
    const QModelIndex box = findRow(model, "Box");
    const QModelIndex looks = findRow(model, "Looks");
    CHECK(wood.flags() & Qt::ItemIsSelectable, "a Material can be picked");
    CHECK(box.isValid() && !(box.flags() & Qt::ItemIsSelectable) && (box.flags() & Qt::ItemIsEnabled), "a Mesh is greyed out");
    CHECK(box.data(Qt::ToolTipRole).toString().contains("targets a Material"), "with the rule's reason");
    CHECK(box.data(Qt::ForegroundRole).value<QColor>() != wood.data(Qt::ForegroundRole).value<QColor>(), "and looks it");
    CHECK(!(looks.flags() & Qt::ItemIsSelectable) && tree->treeView()->isExpanded(looks), "a Scope too, opened to reach under it");
    CHECK(!tree->selectPath(SdfPath("/Nowhere")), "a prim that isn't there");
    tree->treeView()->selectionModel()->clearSelection();
    CHECK(!dialog.useButton()->isEnabled(), "nothing picked: no Use Prim");
    tree->treeView()->setCurrentIndex(box);
    tree->treeView()->selectionModel()->select(box, QItemSelectionModel::Select);
    CHECK(!tree->treeView()->selectionModel()->selectedRows().isEmpty() ? tree->treeView()->selectionModel()->selectedRows().front() != box : true,
          "clicking the greyed row selects nothing");

    // The filter, keeping parents.
    tree->setFilter(QStringLiteral("wo"));
    model = tree->treeView()->model();
    CHECK(findRow(model, "Wood").isValid() && findRow(model, "Looks").isValid() && !findRow(model, "Box").isValid(),
          "the filter finds it");
    CHECK(tree->matchCount() == 1, "one match");
    dialog.reject();
    CHECK(dialog.isVisible() && findRow(model, "Box").isValid(), "Esc clears the filter first");

    // Rules changed at runtime count in the next dialog.
    RelationshipTargetRule rule;
    rule.relationship = "myStudio:look";
    rule.types = {"Material"};
    rule.severity = RelationshipTargetRule::Severity::Warn;
    rule.reason = "Usually a Material";
    RelationshipTargetRules::instance().set(rule);
    {
        RelationshipTargetDialog warned(stage, "myStudio:look", SdfPath());
        QAbstractItemModel* warnedModel = warned.tree()->treeView()->model();
        const QModelIndex warnedBox = findRow(warnedModel, "Box");
        CHECK((warnedBox.flags() & Qt::ItemIsSelectable) && warnedBox.data(Qt::ToolTipRole).toString().contains("Usually"),
              "a warning can be picked, with its reason");
    }
    {
        RelationshipTargetDialog free(stage, "myStudio:anything", SdfPath());
        const QModelIndex freeBox = findRow(free.tree()->treeView()->model(), "Box");
        CHECK(freeBox.flags() & Qt::ItemIsSelectable, "no rule: anything can be picked");
    }
    RelationshipTargetRules::instance().reset();
    QStandardPaths::setTestModeEnabled(false);
}

USDCC_TEST(relationship_rows) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdPrim wood = s->DefinePrim(SdfPath("/Wood"), TfToken("Material"));
    const UsdPrim steel = s->DefinePrim(SdfPath("/Steel"), TfToken("Material"));
    const UsdPrim plain = s->DefinePrim(SdfPath("/Plain"), TfToken("Xform"));
    const UsdPrim box = s->DefinePrim(SdfPath("/Box"), TfToken("Mesh"));
    box.CreateRelationship(TfToken("myStudio:many"), true).AddTarget(wood.GetPath());
    box.GetRelationship(TfToken("myStudio:many")).AddTarget(SdfPath("/Gone"));
    QUndoStack* stack = undo->stack(stage);

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(600, 900);
    panel.show();
    stages.setSelectedPaths(stage, {box.GetPath()});
    settle();

    // A pill per target; one that's gone is marked.
    CHECK(pillFor(panel, "/Wood") && pillFor(panel, "/Gone"), "each target has a pill");
    CHECK(pillFor(panel, "/Wood") && pillFor(panel, "/Wood")->property("state").toString() == "authored", "a fine one");
    CHECK(pillFor(panel, "/Gone") && pillFor(panel, "/Gone")->property("state").toString() == "unregistered" &&
              pillFor(panel, "/Gone")->findChild<QToolButton*>("pillLink")->toolTip().contains("doesn't exist"),
          "one that's gone says so");

    // A click on a missing one does nothing; on a real one selects it.
    pillFor(panel, "/Gone")->findChild<QToolButton*>("pillLink")->click();
    settle();
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>({box.GetPath()}), "a missing target can't be selected");

    // × removes one target, one undo step.
    pillFor(panel, "/Gone")->findChild<QToolButton*>("schemaPillRemove")->click();
    settle();
    CHECK(!pillFor(panel, "/Gone") && pillFor(panel, "/Wood"), "× removes that target");
    CHECK(stack->undoText().startsWith("Remove a target"), "one undo step: [%s]", qPrintable(stack->undoText()));
    stack->undo();
    settle();
    CHECK(pillFor(panel, "/Gone"), "undo brings it back");

    // Selecting a target.
    pillFor(panel, "/Wood")->findChild<QToolButton*>("pillLink")->click();
    settle();
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>({wood.GetPath()}), "a click selects the target");
    stages.setSelectedPaths(stage, {box.GetPath()});
    settle();

    // Adding MaterialBindingAPI asks for its material. Cancel applies nothing.
    answerNextDialog([](RelationshipTargetDialog* dialog) {
        CHECK(dialog->windowTitle().contains("Bind a Material"), "the title says what for");
        dialog->reject();
    });
    {
        QMenu menu;
        panel.populateSchemaMenu(&menu);
        findEntry(&menu, "Shading", "MaterialBindingAPI")->trigger();
        settle();
    }
    const TfToken bindingApi("MaterialBindingAPI");
    auto applied = [&](const UsdPrim& prim) {
        const TfTokenVector schemas = prim.GetAppliedSchemas();
        return std::find(schemas.begin(), schemas.end(), bindingApi) != schemas.end();
    };
    CHECK(!applied(box), "Cancel applies nothing");

    // Picking one: the schema and the binding, one undo step.
    answerNextDialog([](RelationshipTargetDialog* dialog) {
        dialog->tree()->selectPath(SdfPath("/Wood"));
        dialog->accept();
    });
    {
        QMenu menu;
        panel.populateSchemaMenu(&menu);
        findEntry(&menu, "Shading", "MaterialBindingAPI")->trigger();
        settle();
    }
    CHECK(applied(box) && UsdShadeMaterialBindingAPI(box).ComputeBoundMaterial().GetPrim() == wood, "bound to the picked Material");
    CHECK(pillFor(panel, "/Wood"), "its binding's row shows it");
    CHECK(stack->undoText() == "Apply MaterialBindingAPI to Box", "one undo step: [%s]", qPrintable(stack->undoText()));
    stack->undo();
    settle();
    CHECK(!applied(box) && !box.GetRelationship(TfToken("material:binding")), "undo takes both away");

    // Apply Without Material: the schema, and a row to bind one later.
    answerNextDialog([](RelationshipTargetDialog* dialog) {
        for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (button->text() == "Apply Without Material") button->click();
        }
    });
    {
        QMenu menu;
        panel.populateSchemaMenu(&menu);
        findEntry(&menu, "Shading", "MaterialBindingAPI")->trigger();
        settle();
    }
    CHECK(applied(box) && !box.GetRelationship(TfToken("material:binding")), "applied, with no binding");
    RelationshipTargetsEditor* binding = bindingRowOf(panel);
    CHECK(binding && binding->addButton()->isVisible() && binding->findChild<QLabel*>() &&
              binding->findChild<QLabel*>()->text() == "None",
          "the row is there with nothing set, and can add");

    // Set it from the row: the dialog, then the target, replacing for a single one.
    answerNextDialog([](RelationshipTargetDialog* dialog) {
        dialog->tree()->selectPath(SdfPath("/Steel"));
        dialog->accept();
    });
    if (binding) binding->addButton()->click();
    settle();
    CHECK(pillFor(panel, "/Steel"), "set from the row");
    answerNextDialog([](RelationshipTargetDialog* dialog) {
        CHECK(dialog->tree()->selectedPath() == SdfPath("/Steel"), "changing starts at the current one");
        dialog->tree()->selectPath(SdfPath("/Wood"));
        dialog->accept();
    });
    {
        auto* row = pillFor(panel, "/Steel") ? qobject_cast<RelationshipTargetsEditor*>(pillFor(panel, "/Steel")->parentWidget()) : nullptr;
        if (row) row->addButton()->click();
    }
    settle();
    SdfPathVector targets;
    box.GetRelationship(TfToken("material:binding")).GetTargets(&targets);
    CHECK(targets == SdfPathVector({wood.GetPath()}), "a binding holds one: it's replaced");

    // Removing its last target removes the relationship, rather than a delete list.
    QFrame* boundPill = nullptr;
    for (auto* editor : panel.findChildren<RelationshipTargetsEditor*>()) {
        for (QFrame* pill : editor->findChildren<QFrame*>(QStringLiteral("schemaPill"))) {
            if (pill->findChild<QToolButton*>("pillLink")->text() == "/Wood" &&
                editor->addButton()->toolTip().contains("Change")) {
                boundPill = pill;
            }
        }
    }
    CHECK(boundPill, "the binding's pill");
    if (boundPill) boundPill->findChild<QToolButton*>("schemaPillRemove")->click();
    settle();
    CHECK(!box.GetRelationship(TfToken("material:binding")).IsAuthored(), "no empty relationship left behind");

    // A target the rule objects to is marked.
    box.CreateRelationship(TfToken("material:binding"), false).AddTarget(plain.GetPath());
    settle();
    QFrame* wrong = pillFor(panel, "/Plain");
    CHECK(wrong && wrong->property("state").toString() == "unregistered" &&
              wrong->findChild<QToolButton*>("pillLink")->toolTip().contains("targets a Material"),
          "a binding to an Xform is marked, with the reason");

    (void)steel;
    QStandardPaths::setTestModeEnabled(false);
}

// MaterialBindingAPI with no binding yet: its row shows even with Authored Only,
// where the binding could never otherwise appear, and near the top.
USDCC_TEST(relationship_unset_binding_row) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdPrim box = stage->usdStage()->DefinePrim(SdfPath("/Box"), TfToken("Mesh"));
    box.AddAppliedSchema(TfToken("MaterialBindingAPI"));

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(600, 900);
    panel.show();
    stages.setSelectedPaths(stage, {box.GetPath()});
    settle();
    QAction* authoredOnly = nullptr;
    for (QAction* action : panel.findChildren<QAction*>()) {
        if (action->text() == "Authored Only") authoredOnly = action;
    }
    CHECK(authoredOnly, "the option");
    if (!authoredOnly) return;

    for (const bool on : {false, true}) {
        authoredOnly->setChecked(on);
        settle();
        RelationshipTargetsEditor* row = bindingRowOf(panel);
        CHECK(row && row->isVisibleTo(&panel), "the row shows with Authored Only %s", on ? "on" : "off");
        if (!row) continue;
        // Above the type's own sections, not after all of them.
        int firstOther = -1;
        for (auto* section : panel.findChildren<usdcc::usd::properties::CategorySection*>()) {
            auto* header = section->findChild<QToolButton*>(QStringLiteral("sectionHeader"));
            if (header && header->text() == "Mesh") firstOther = section->mapTo(&panel, QPoint()).y();
        }
        CHECK(firstOther < 0 || row->mapTo(&panel, QPoint()).y() < firstOther, "above the Mesh section");
    }
    authoredOnly->setChecked(false);

    // Other material:* properties, without the schema: no binding row.
    const UsdPrim other = stage->usdStage()->DefinePrim(SdfPath("/Other"), TfToken("Mesh"));
    other.CreateAttribute(TfToken("material:note"), SdfValueTypeNames->Float).Set(1.0f);
    stages.setSelectedPaths(stage, {other.GetPath()});
    settle();
    CHECK(!bindingRowOf(panel), "without MaterialBindingAPI there's no row to bind with");
    QStandardPaths::setTestModeEnabled(false);
}
