// Connecting shader inputs in the Attribute Editor (docs/ATTRIBUTE_EDITOR.md §4.3):
// a receiver's connect / disconnect button and the dialog behind it, the source as
// a link that selects it, and a shader output's pills of the inputs it feeds, with
// an × and the dialog the other way round. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/properties/connected_inputs_editor.h"
#include "usdcc/usd/properties/property_row.h"
#include "usdcc/usd/shade_connections.h"
#include "usdcc/usd/shader_connection_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <functional>
#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::ShaderConnectionDialog;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::properties::ConnectedInputsEditor;
using usdcc::usd::properties::PropertyRow;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

// Runs `answer` on the connection dialog that opens next.
void answerNextDialog(std::function<void(ShaderConnectionDialog*)> answer) {
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, answer]() {
        auto* dialog = qobject_cast<ShaderConnectionDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        poll->stop();
        poll->deleteLater();
        answer(dialog);
    });
    poll->start();
}

PropertyRow* rowFor(AttributesViewPanel& panel, const QString& path) {
    for (auto* row : panel.findChildren<PropertyRow*>()) {
        if (row->propertyPath() == path && row->isVisibleTo(&panel)) return row;
    }
    return nullptr;
}

// A receiver's connect / disconnect button (styled as #propertyIconButton).
QToolButton* connectButtonOf(PropertyRow* row) {
    if (!row) return nullptr;
    for (QToolButton* button : row->findChildren<QToolButton*>()) {
        if (button->property("connectionButton").toBool() && button->parentWidget() == row) return button;
    }
    return nullptr;
}

// The labels of a shader output's pills.
QStringList pillsOf(PropertyRow* row) {
    QStringList labels;
    auto* editor = row ? qobject_cast<ConnectedInputsEditor*>(row->editor()) : nullptr;
    if (!editor) return {QStringLiteral("<no pills editor>")};
    for (QFrame* pill : editor->findChildren<QFrame*>(QStringLiteral("schemaPill"))) {
        labels << pill->findChild<QToolButton*>(QStringLiteral("pillLink"))->text();
    }
    labels.sort();
    return labels;
}

QFrame* pill(PropertyRow* row, const QString& label) {
    auto* editor = row ? qobject_cast<ConnectedInputsEditor*>(row->editor()) : nullptr;
    if (!editor) return nullptr;
    for (QFrame* frame : editor->findChildren<QFrame*>(QStringLiteral("schemaPill"))) {
        if (frame->findChild<QToolButton*>(QStringLiteral("pillLink"))->text() == label) return frame;
    }
    return nullptr;
}

bool pickable(QTreeWidgetItem* item) { return item && (item->flags() & Qt::ItemIsSelectable); }
bool enabled(QTreeWidgetItem* item) { return item && (item->flags() & Qt::ItemIsEnabled); }
}  // namespace

USDCC_TEST(shader_connection_rows) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdShadeMaterial mat = UsdShadeMaterial::Define(s, SdfPath("/Mat"));
    mat.CreateInput(TfToken("tint"), SdfValueTypeNames->Color3f);
    mat.CreateInput(TfToken("gloss"), SdfValueTypeNames->Float);
    mat.CreateSurfaceOutput();
    UsdShadeShader surface = UsdShadeShader::Define(s, SdfPath("/Mat/Surface"));
    surface.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f);
    surface.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f);
    surface.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float);
    UsdShadeShader tex = UsdShadeShader::Define(s, SdfPath("/Mat/Tex"));
    tex.CreateInput(TfToken("file"), SdfValueTypeNames->Asset);
    tex.CreateOutput(TfToken("rgb"), SdfValueTypeNames->Float3);
    tex.CreateOutput(TfToken("r"), SdfValueTypeNames->Float);
    UsdShadeNodeGraph::Define(s, SdfPath("/Mat/Nodes"));
    UsdShadeShader::Define(s, SdfPath("/Mat/Nodes/Inner")).CreateInput(TfToken("in"), SdfValueTypeNames->Color3f);
    QUndoStack* stack = undo->stack(stage);

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(700, 900);
    panel.show();
    stages.setSelectedPaths(stage, {SdfPath("/Mat/Surface")});
    settle();

    // An unconnected input: the connect button, rightmost.
    PropertyRow* diffuse = rowFor(panel, "/Mat/Surface.inputs:diffuseColor");
    QToolButton* button = connectButtonOf(diffuse);
    CHECK(button && button->isVisibleTo(&panel), "an input has the connect button");
    CHECK(button && button->objectName() == "propertyIconButton" && !button->icon().isNull() &&
              !button->icon().pixmap(16, 16).isNull(),
          "styled like the row's other icon buttons, with its icon");
    CHECK(button && diffuse->width() - (button->mapTo(diffuse, QPoint(0, 0)).x() + button->width()) <= 2,
          "at the row's right edge");
    CHECK(button && button->toolTip().startsWith("Connect"), "saying what it does: %s", button ? qPrintable(button->toolTip()) : "");
    CHECK(diffuse && !diffuse->connectedSourceLink()->isVisibleTo(&panel), "no link while unconnected");

    // Cancel changes nothing.
    answerNextDialog([](ShaderConnectionDialog* dialog) { dialog->reject(); });
    button->click();
    settle();
    CHECK(stack->count() == 0, "Cancel connects nothing");

    // The dialog: the material's prims, the edited one not pickable, types greyed.
    answerNextDialog([](ShaderConnectionDialog* dialog) {
        CHECK(dialog->windowTitle() == "Connect inputs:diffuseColor", "the title: %s", qPrintable(dialog->windowTitle()));
        CHECK(dialog->nodeItem(SdfPath("/Mat")) && dialog->nodeItem(SdfPath("/Mat/Nodes/Inner")),
              "the tree is the material and everything in it");
        CHECK(!pickable(dialog->nodeItem(SdfPath("/Mat/Surface"))), "the edited shader can't be picked");

        dialog->nodeTree()->setCurrentItem(dialog->nodeItem(SdfPath("/Mat")));
        CHECK(enabled(dialog->attributeItem(TfToken("inputs:tint"))), "the material's color3f input can be picked");
        CHECK(dialog->attributeItem(TfToken("inputs:gloss")) && !enabled(dialog->attributeItem(TfToken("inputs:gloss"))),
              "its float input is listed but greyed");
        CHECK(!dialog->attributeItem(TfToken("outputs:surface")), "its own output isn't listed: nothing inside takes it");
        CHECK(!pickable(dialog->nodeItem(SdfPath("/Mat/Nodes"))), "a node graph with nothing to offer can't be picked");

        dialog->nodeTree()->setCurrentItem(dialog->nodeItem(SdfPath("/Mat/Tex")));
        QTreeWidgetItem* rgb = dialog->attributeItem(TfToken("outputs:rgb"));
        QTreeWidgetItem* r = dialog->attributeItem(TfToken("outputs:r"));
        CHECK(enabled(rgb), "a float3 output fits a color3f input");
        CHECK(r && !enabled(r) && r->toolTip(0).contains("float"), "a float one is greyed, saying why: %s",
              r ? qPrintable(r->toolTip(0)) : "");
        CHECK(!dialog->attributeItem(TfToken("inputs:file")), "another shader's input isn't listed");
        CHECK(dialog->attributeList()->topLevelItemCount() == 2, "only its outputs are: %d",
              dialog->attributeList()->topLevelItemCount());
        CHECK(!dialog->connectButton()->isEnabled(), "Connect waits for an attribute");
        dialog->attributeList()->setCurrentItem(rgb);
        CHECK(dialog->connectButton()->isEnabled() && dialog->selected() == SdfPath("/Mat/Tex.outputs:rgb"),
              "picking one enables Connect");
        dialog->connectButton()->click();
    });
    button->click();
    settle();
    const UsdAttribute diffuseAttr = s->GetAttributeAtPath(SdfPath("/Mat/Surface.inputs:diffuseColor"));
    CHECK(usdcc::usd::connectedSource(diffuseAttr) == SdfPath("/Mat/Tex.outputs:rgb"), "Connect connects it");
    CHECK(stack->count() == 1 && stack->undoText() == "Connect inputs:diffuseColor", "one undo step: %s",
          qPrintable(stack->undoText()));

    // Connected: a link relative to the material, no badge, and the disconnect button.
    diffuse = rowFor(panel, "/Mat/Surface.inputs:diffuseColor");
    button = connectButtonOf(diffuse);
    CHECK(diffuse && diffuse->connectedSourceLink()->isVisibleTo(&panel) &&
              diffuse->connectedSourceLink()->text() == "Tex.outputs:rgb",
          "the source as a link: %s", diffuse ? qPrintable(diffuse->connectedSourceLink()->text()) : "");
    const QLabel* badge = diffuse ? diffuse->findChild<QLabel*>(QStringLiteral("propertyBadge")) : nullptr;
    CHECK(badge && !badge->isVisibleTo(&panel), "no \"connected\" badge");
    CHECK(button && button->toolTip() == "Disconnect from Tex.outputs:rgb", "the button disconnects now: %s",
          button ? qPrintable(button->toolTip()) : "");
    CHECK(button && button->icon().pixmap(16, 16).toImage() == QIcon(":/usdcc/theme/unlink.png").pixmap(16, 16).toImage() &&
              button->icon().pixmap(16, 16).toImage() != QIcon(":/usdcc/theme/link.png").pixmap(16, 16).toImage(),
          "with the disconnect icon");

    // Disconnecting, and undo.
    button->click();
    settle();
    CHECK(usdcc::usd::connectedSource(diffuseAttr).IsEmpty(), "the button disconnects");
    CHECK(stack->undoText() == "Disconnect inputs:diffuseColor", "one undo step");
    stack->undo();
    settle();
    CHECK(!usdcc::usd::connectedSource(diffuseAttr).IsEmpty(), "undo connects it again");

    // The link selects the source.
    rowFor(panel, "/Mat/Surface.inputs:diffuseColor")->connectedSourceLink()->click();
    settle();
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>({SdfPath("/Mat/Tex")}), "the link selects the source prim");

    // The output's pills.
    PropertyRow* rgbRow = rowFor(panel, "/Mat/Tex.outputs:rgb");
    CHECK(pillsOf(rgbRow) == QStringList({"Surface.inputs:diffuseColor"}), "an output lists the inputs it feeds: %s",
          qPrintable(pillsOf(rgbRow).join(", ")));
    CHECK(rgbRow && qobject_cast<ConnectedInputsEditor*>(rgbRow->editor()), "the pills stand in for its value field");
    CHECK(pillsOf(rowFor(panel, "/Mat/Tex.outputs:r")).isEmpty(), "one feeding nothing has none");

    // An edit on another prim of the material updates them.
    usdcc::usd::prim_actions::connectSource(*undo, stage, SdfPath("/Mat/Nodes/Inner.inputs:in"), SdfPath("/Mat/Tex.outputs:rgb"));
    settle();
    rgbRow = rowFor(panel, "/Mat/Tex.outputs:rgb");
    CHECK(pillsOf(rgbRow) == QStringList({"Nodes/Inner.inputs:in", "Surface.inputs:diffuseColor"}),
          "a connection made elsewhere shows up: %s", qPrintable(pillsOf(rgbRow).join(", ")));

    // × disconnects that input.
    pill(rgbRow, "Nodes/Inner.inputs:in")->findChild<QToolButton*>(QStringLiteral("schemaPillRemove"))->click();
    settle();
    rgbRow = rowFor(panel, "/Mat/Tex.outputs:rgb");
    CHECK(pillsOf(rgbRow) == QStringList({"Surface.inputs:diffuseColor"}), "× disconnects it");
    CHECK(stack->undoText() == "Disconnect inputs:in", "one undo step: %s", qPrintable(stack->undoText()));

    // The dialog the other way round: only unconnected inputs of the same type.
    answerNextDialog([](ShaderConnectionDialog* dialog) {
        CHECK(dialog->windowTitle().startsWith("Connect outputs:rgb to"), "the title: %s", qPrintable(dialog->windowTitle()));
        CHECK(!pickable(dialog->nodeItem(SdfPath("/Mat/Tex"))), "its own shader can't be picked");
        CHECK(!pickable(dialog->nodeItem(SdfPath("/Mat"))), "nor the material, whose inputs come from outside it");
        CHECK(pickable(dialog->nodeItem(SdfPath("/Mat/Nodes/Inner"))), "a shader with an unconnected color3f input can");
        dialog->nodeTree()->setCurrentItem(dialog->nodeItem(SdfPath("/Mat/Surface")));
        CHECK(dialog->attributeList()->topLevelItemCount() == 1 && dialog->attributeItem(TfToken("inputs:emissiveColor")),
              "only the unconnected color3f input is listed: %d", dialog->attributeList()->topLevelItemCount());
        dialog->attributeList()->setCurrentItem(dialog->attributeItem(TfToken("inputs:emissiveColor")));
        dialog->connectButton()->click();
    });
    qobject_cast<ConnectedInputsEditor*>(rgbRow->editor())->connectButton()->click();
    settle();
    rgbRow = rowFor(panel, "/Mat/Tex.outputs:rgb");
    CHECK(pillsOf(rgbRow) == QStringList({"Surface.inputs:diffuseColor", "Surface.inputs:emissiveColor"}),
          "connects the picked input to the output: %s", qPrintable(pillsOf(rgbRow).join(", ")));
    CHECK(stack->undoText() == "Connect inputs:emissiveColor", "one undo step: %s", qPrintable(stack->undoText()));

    // A pill selects its prim.
    pill(rgbRow, "Surface.inputs:emissiveColor")->findChild<QToolButton*>(QStringLiteral("pillLink"))->click();
    settle();
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>({SdfPath("/Mat/Surface")}), "a pill selects the input's prim");
    QStandardPaths::setTestModeEnabled(false);
}
