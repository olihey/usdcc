// Adding an input to a Material, NodeGraph or Shader (docs/MATERIAL_EDITOR.md §4): the + on
// the Attribute Editor's Inputs header and the dialog behind it, which the Material Editor's
// "Add Input..." opens too. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/add_shade_input_dialog.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>

#include <functional>
#include <memory>

using usdcc::usd::AddShadeInputDialog;
using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

// Runs `answer` on the dialog that opens next.
void answerNextDialog(std::function<void(AddShadeInputDialog*)> answer) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, answer]() {
        auto* dialog = qobject_cast<AddShadeInputDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        poll->stop();
        poll->deleteLater();
        answer(dialog);
    });
    poll->start();
}

QToolButton* plusOf(AttributesViewPanel& panel) {
    for (QToolButton* button : panel.findChildren<QToolButton*>()) {
        if (button->property("addInputButton").toBool() && button->isVisibleTo(&panel)) return button;
    }
    return nullptr;
}

bool hasRow(AttributesViewPanel& panel, const QString& path) {
    for (auto* row : panel.findChildren<PropertyRow*>()) {
        if (row->propertyPath() == path && row->isVisibleTo(&panel)) return true;
    }
    return false;
}

// The Type menu's submenu titled `title`, or the menu itself for an empty one.
QMenu* typeSubmenu(AddShadeInputDialog* dialog, const QString& title) {
    if (title.isEmpty()) return dialog->typeMenu();
    for (QAction* action : dialog->typeMenu()->actions()) {
        if (action->menu() && action->text() == title) return action->menu();
    }
    return nullptr;
}

QAction* typeAction(QMenu* menu, const QString& type) {
    if (!menu) return nullptr;
    for (QAction* action : menu->actions()) {
        if (!action->menu() && action->data().toString() == type) return action;
    }
    return nullptr;
}

// Picks it from the menu, as a click does: from the top level, or from the group's submenu.
bool pickType(AddShadeInputDialog* dialog, const char* type, const QString& group = {}) {
    QAction* action = typeAction(typeSubmenu(dialog, group), QString::fromLatin1(type));
    if (!action) return false;
    action->trigger();
    return dialog->scalarType() == QString::fromLatin1(type);
}
}  // namespace

USDCC_TEST(add_shade_input_from_the_attribute_editor) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    UsdShadeMaterial::Define(s, SdfPath("/Mat")).CreateSurfaceOutput();
    UsdShadeShader surface = UsdShadeShader::Define(s, SdfPath("/Mat/Surface"));
    surface.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    UsdShadeNodeGraph::Define(s, SdfPath("/Mat/Nodes"));
    UsdGeomXform::Define(s, SdfPath("/World"));
    QUndoStack* stack = undo->stack(stage);

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(700, 900);
    panel.show();

    // A Material with no inputs yet still has the +.
    stages.setSelectedPaths(stage, {SdfPath("/Mat")});
    settle();
    QToolButton* plus = plusOf(panel);
    CHECK(plus, "a Material's Inputs header has the +");
    if (!plus) return;
    CHECK(plus->objectName() == "propertyIconButton" && !plus->icon().isNull() && !plus->icon().pixmap(12, 12).isNull(),
          "styled like the other header buttons, with its icon");

    answerNextDialog([](AddShadeInputDialog* dialog) { dialog->reject(); });
    plus->click();
    settle();
    CHECK(stack->count() == 0, "Cancel adds nothing");

    answerNextDialog([](AddShadeInputDialog* dialog) {
        CHECK(!dialog->addButton()->isEnabled(), "no name, no adding");
        // A menu, not a long list: the common types, then a submenu per group.
        QMenu* types = dialog->typeMenu();
        QStringList top;
        QStringList groups;
        for (QAction* action : types->actions()) {
            if (action->menu()) {
                groups << action->text();
            } else if (!action->isSeparator()) {
                top << action->text();
            }
        }
        CHECK(top.size() == 10 && top.front() == "float" && top.contains("color3f"), "the common types first: %s", qPrintable(top.join(",")));
        CHECK(groups.contains("Colors") && groups.contains("Matrices") && groups.indexOf("Colors") < groups.indexOf("Matrices"),
              "then a submenu per group, in a shader's order: %s", qPrintable(groups.join(",")));
        CHECK(typeAction(typeSubmenu(dialog, "Matrices"), "matrix4d"), "every type is in its group");
        CHECK(dialog->typeButton()->text() == "float" && typeAction(types, "float")->isChecked(), "float is picked, and shows it");
        dialog->nameField()->setText(QStringLiteral("tint"));
        CHECK(pickType(dialog, "color3f", "Colors"), "picked from its group");
        CHECK(dialog->typeButton()->text() == "color3f", "the button shows it");
        CHECK(typeAction(types, "color3f")->isChecked() && !typeAction(types, "float")->isChecked(),
              "and the menu ticks it, at the top too");
        CHECK(!dialog->setScalarType("float[]") && !dialog->setScalarType("nonsense") && dialog->scalarType() == "color3f",
              "an array or what isn't a type changes nothing");
        CHECK(dialog->valueEditor() && dialog->valueEditor()->typeInfo().typeName == SdfValueTypeNames->Color3f,
              "the default is edited as a color3f");
        CHECK(!dialog->allowedTokensField()->isVisibleTo(dialog), "allowed tokens are only for tokens");
        dialog->valueEditor()->setValue(VtValue(GfVec3f(1, 0, 0)));
        dialog->displayGroupField()->setText(QStringLiteral("Look"));
        CHECK(dialog->addButton()->isEnabled(), "a name and a type are enough");
        dialog->addButton()->click();
    });
    plus->click();
    settle();
    CHECK(stack->count() == 1, "one undo step: %d", stack->count());
    const UsdShadeInput tint = UsdShadeMaterial(s->GetPrimAtPath(SdfPath("/Mat"))).GetInput(TfToken("tint"));
    GfVec3f color;
    CHECK(tint && tint.Get(&color) && color == GfVec3f(1, 0, 0), "the input, with its default");
    CHECK(tint && tint.GetDisplayGroup() == "Look", "and its display group");
    settle();
    CHECK(hasRow(panel, "/Mat.inputs:tint"), "and its row");

    // A Shader: its definition's inputs are taken; arrays and tokens.
    stages.setSelectedPaths(stage, {SdfPath("/Mat/Surface")});
    settle();
    plus = plusOf(panel);
    CHECK(plus, "a Shader's first page has the +");
    if (!plus) return;
    answerNextDialog([](AddShadeInputDialog* dialog) {
        dialog->nameField()->setText(QStringLiteral("roughness"));
        CHECK(!dialog->addButton()->isEnabled() && dialog->problemLabel()->isVisibleTo(dialog),
              "not one its definition has, and it says why");
        dialog->nameField()->setText(QStringLiteral("inputs:weights"));
        dialog->arrayCheck()->setChecked(true);
        CHECK(dialog->valueEditor()->typeInfo().typeName == SdfValueTypeNames->FloatArray, "Array makes it float[]");
        CHECK(dialog->spec().name == "weights", "a typed inputs: is dropped");
        dialog->arrayCheck()->setChecked(false);
        CHECK(pickType(dialog, "token"), "picked from the common ones");
        CHECK(dialog->allowedTokensField()->isVisibleTo(dialog), "a token can have allowed tokens");
        dialog->allowedTokensField()->setText(QStringLiteral("a, b"));
        Q_EMIT dialog->allowedTokensField()->editingFinished();
        CHECK(dialog->valueEditor()->value() == VtValue(TfToken("a")), "and its default is one of them");
        CHECK(dialog->spec().allowedTokens.size() == 2, "both of them");
        dialog->reject();
    });
    plus->click();
    settle();

    stages.setSelectedPaths(stage, {SdfPath("/Mat/Nodes")});
    settle();
    CHECK(plusOf(panel), "a NodeGraph has the + too");
    stages.setSelectedPaths(stage, {SdfPath("/World")});
    settle();
    CHECK(!plusOf(panel), "an Xform hasn't");
}
