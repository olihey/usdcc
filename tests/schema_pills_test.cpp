// The Applied Schemas row (docs/APPLIED_SCHEMAS.md): a pill per schema with an ×
// only where removing works, wrapping, removing with and without the cleanup
// question and its undo, the + menu by category with greyed entries, and an
// instance proxy read-only. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/properties/schema_pills.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>

#include <algorithm>
#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::properties::SchemaPillsEditor;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

QFrame* pillNamed(SchemaPillsEditor* pills, const QString& name) {
    for (QFrame* pill : pills->findChildren<QFrame*>(QStringLiteral("schemaPill"))) {
        if (auto* label = pill->findChild<QLabel*>(); label && label->text() == name) return pill;
    }
    return nullptr;
}

QToolButton* removeButton(QFrame* pill) { return pill ? pill->findChild<QToolButton*>() : nullptr; }

// Clicks the button with this text on the message box that opens next.
void answerNextQuestion(const QString& button, QString* text = nullptr) {
    // Polled: the × is queued, so the box opens a few events from now.
    auto* poll = new QTimer(qApp);
    poll->setInterval(5);
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, button, text]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        poll->stop();
        poll->deleteLater();
        if (text) *text = box->informativeText();
        for (QAbstractButton* candidate : box->buttons()) {
            if (candidate->text() == button) candidate->click();
        }
    });
    poll->start();
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

USDCC_TEST(schema_pills) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const UsdPrim mesh = s->DefinePrim(SdfPath("/M"), TfToken("Mesh"));
    mesh.AddAppliedSchema(TfToken("MeshLightAPI"));
    mesh.AddAppliedSchema(TfToken("PhysicsRigidBodyAPI"));
    mesh.AddAppliedSchema(TfToken("PhysicsMassAPI"));
    mesh.AddAppliedSchema(TfToken("NotARealAPI"));
    mesh.GetAttribute(TfToken("physics:velocity")).Set(GfVec3f(1, 2, 3));

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(500, 800);
    panel.show();
    stages.setSelectedPaths(stage, {SdfPath("/M")});
    settle();
    auto pills = [&]() { return panel.findChild<SchemaPillsEditor*>(); };
    CHECK(pills(), "the row has pills");
    if (!pills()) return;

    // A pill for each, with an × only where removing works.
    CHECK(removeButton(pillNamed(pills(), "MeshLightAPI")), "an authored one has an ×");
    CHECK(pillNamed(pills(), "LightAPI") && !removeButton(pillNamed(pills(), "LightAPI")) &&
              pillNamed(pills(), "LightAPI")->property("state").toString() == "builtIn" &&
              pillNamed(pills(), "LightAPI")->toolTip().contains("Comes with MeshLightAPI"),
          "one it brings doesn't, and says why");
    CHECK(removeButton(pillNamed(pills(), "NotARealAPI")) &&
              pillNamed(pills(), "NotARealAPI")->property("state").toString() == "unregistered",
          "an unregistered one does, marked");
    CHECK(pills()->addButton()->isVisible(), "the + shows");
    const int right = pills()->addButton()->mapTo(&panel, QPoint(pills()->addButton()->width(), 0)).x();
    for (QFrame* pill : pills()->findChildren<QFrame*>(QStringLiteral("schemaPill"))) {
        CHECK(pill->mapTo(&panel, QPoint(pill->width(), 0)).x() <= right, "pills stay left of the +");
    }

    // Wrapping, on the row alone (other rows hold the panel's own minimum width).
    {
        SchemaPillsEditor row;
        std::vector<usdcc::usd::AppliedSchema> schemas(3);
        schemas[0].name = "A";
        schemas[1].name = "B";
        schemas[2].name = "AVeryLongSchemaNameThatWillNotFitInANarrowRowAtAll";
        row.setSchemas(schemas);
        row.resize(2000, 20);
        row.show();
        settle();
        const int oneLine = row.height();
        row.resize(90, row.height());
        settle();
        CHECK(row.height() > oneLine, "narrower wraps to more lines: %d, was %d", row.height(), oneLine);
        CHECK(row.minimumWidth() < 90 && row.layout()->minimumSize().width() == 0, "a long name doesn't hold the row wide");
        for (QFrame* pill : row.findChildren<QFrame*>(QStringLiteral("schemaPill"))) {
            CHECK(pill->width() <= row.width(), "a pill fits the row: %d", pill->width());
            CHECK(removeButton(pill) && removeButton(pill)->geometry().right() <= pill->width(), "its × still shows");
        }
    }

    // With nothing authored to clean up it goes at once, one undo step.
    QUndoStack* stack = undo->stack(stage);
    removeButton(pillNamed(pills(), "PhysicsMassAPI"))->click();
    settle();
    TfTokenVector applied = mesh.GetAppliedSchemas();
    CHECK(std::find(applied.begin(), applied.end(), TfToken("PhysicsMassAPI")) == applied.end() &&
              !pillNamed(pills(), "PhysicsMassAPI"),
          "removed without a question");
    CHECK(stack->undoText() == "Remove PhysicsMassAPI from M", "one undo step: [%s]", qPrintable(stack->undoText()));
    stack->undo();
    settle();
    CHECK(pillNamed(pills(), "PhysicsMassAPI"), "undo brings its pill back");

    // Authored properties: the question. Cancel changes nothing.
    QString question;
    answerNextQuestion(QStringLiteral("Cancel"), &question);
    removeButton(pillNamed(pills(), "PhysicsRigidBodyAPI"))->click();
    settle();
    CHECK(question.contains("physics:velocity"), "the question lists what's left behind: [%s]", qPrintable(question));
    CHECK(pillNamed(pills(), "PhysicsRigidBodyAPI"), "Cancel keeps it");

    // Schema only: the value stays.
    answerNextQuestion(QStringLiteral("Remove Schema Only"));
    removeButton(pillNamed(pills(), "PhysicsRigidBodyAPI"))->click();
    settle();
    CHECK(!pillNamed(pills(), "PhysicsRigidBodyAPI") && mesh.GetAttribute(TfToken("physics:velocity")).HasAuthoredValue(),
          "Remove Schema Only leaves the property");
    stack->undo();
    settle();

    // Clean up: the value goes too, in one undo step.
    answerNextQuestion(QStringLiteral("Remove and Clean Up"));
    removeButton(pillNamed(pills(), "PhysicsRigidBodyAPI"))->click();
    settle();
    CHECK(!pillNamed(pills(), "PhysicsRigidBodyAPI") && !mesh.GetAttribute(TfToken("physics:velocity")).HasAuthoredValue(),
          "Remove and Clean Up removes it");
    stack->undo();
    settle();
    CHECK(pillNamed(pills(), "PhysicsRigidBodyAPI") && mesh.GetAttribute(TfToken("physics:velocity")).HasAuthoredValue(),
          "undo brings back both");

    // An unregistered one can be removed; a built-in one can't.
    removeButton(pillNamed(pills(), "NotARealAPI"))->click();
    settle();
    CHECK(!pillNamed(pills(), "NotARealAPI"), "an unregistered one goes");
    CHECK(!usdcc::usd::prim_actions::removeSchema(*undo, stage, SdfPath("/M"), "LightAPI", false),
          "a built-in one is refused, rather than USD reporting success and doing nothing");

    // The menu: by category, greyed with a reason, applying adds its pill.
    QMenu menu;
    panel.populateSchemaMenu(&menu);
    CHECK(menu.actions().size() >= 13, "by category: [%d]", int(menu.actions().size()));
    QAction* used = findEntry(&menu, "Physics", "PhysicsRigidBodyAPI");
    CHECK(used && !used->isEnabled() && used->toolTip().contains("Already"), "one already applied is greyed, with why");
    QAction* cantApply = findEntry(&menu, "Shading", "MaterialXConfigAPI");
    CHECK(cantApply && !cantApply->isEnabled() && cantApply->toolTip().contains("Material"), "one that can't apply too");
    QAction* multiple = findEntry(&menu, "Collections & Color", "CollectionAPI...");
    CHECK(multiple && multiple->isEnabled(), "a multiple-apply one isn't, and asks for a name");
    QAction* motion = findEntry(&menu, "Geometry", "MotionAPI");
    CHECK(motion && motion->isEnabled() && !motion->toolTip().isEmpty(), "an available one has its documentation");
    // Applying asks about its attributes. Cancel changes nothing.
    QString asked;
    answerNextQuestion(QStringLiteral("Cancel"), &asked);
    if (motion) motion->trigger();
    settle();
    CHECK(asked.contains("motion:velocityScale"), "it lists the attributes: [%s]", qPrintable(asked));
    CHECK(!pillNamed(pills(), "MotionAPI"), "Cancel doesn't apply it");

    answerNextQuestion(QStringLiteral("Apply Schema Only"));
    {
        QMenu again;
        panel.populateSchemaMenu(&again);
        findEntry(&again, "Geometry", "MotionAPI")->trigger();
        settle();
    }
    CHECK(pillNamed(pills(), "MotionAPI") && removeButton(pillNamed(pills(), "MotionAPI")), "applying adds its pill");
    CHECK(stack->undoText() == "Apply MotionAPI to M", "one undo step: [%s]", qPrintable(stack->undoText()));
    CHECK(!mesh.GetAttribute(TfToken("motion:velocityScale")).IsAuthored(), "Schema Only authors nothing");
    stack->undo();
    settle();

    answerNextQuestion(QStringLiteral("Apply and Add Attributes"));
    {
        QMenu again;
        panel.populateSchemaMenu(&again);
        findEntry(&again, "Geometry", "MotionAPI")->trigger();
        settle();
    }
    const UsdAttribute added = mesh.GetAttribute(TfToken("motion:velocityScale"));
    float scaleValue = 0;
    CHECK(added.HasAuthoredValue() && added.Get(&scaleValue) && scaleValue == 1.0f && !added.IsCustom(),
          "Add Attributes authors it, with its default value: %g", scaleValue);
    CHECK(stack->undoText() == "Apply MotionAPI to M", "in one undo step");
    stack->undo();
    settle();
    CHECK(!pillNamed(pills(), "MotionAPI") && !mesh.GetAttribute(TfToken("motion:velocityScale")).IsValid(),
          "undo takes both away");


    // Nothing applied: says so.
    s->DefinePrim(SdfPath("/Plain"), TfToken("Xform"));
    stages.setSelectedPaths(stage, {SdfPath("/Plain")});
    settle();
    CHECK(pills()->findChildren<QFrame*>(QStringLiteral("schemaPill")).isEmpty() && pills()->findChild<QLabel*>() &&
              pills()->findChild<QLabel*>()->text() == "None",
          "no pills: None");

    // An instance proxy is read-only: no × and no +.
    const UsdPrim source = s->DefinePrim(SdfPath("/Source"), TfToken("Xform"));
    const UsdPrim part = s->DefinePrim(SdfPath("/Source/Part"), TfToken("Xform"));
    part.AddAppliedSchema(TfToken("MotionAPI"));
    const UsdPrim instance = s->DefinePrim(SdfPath("/Inst"), TfToken("Xform"));
    instance.GetReferences().AddInternalReference(source.GetPath());
    instance.SetInstanceable(true);
    settle();
    stages.setSelectedPaths(stage, {SdfPath("/Inst/Part")});
    settle();
    CHECK(s->GetPrimAtPath(SdfPath("/Inst/Part")).IsInstanceProxy() && pillNamed(pills(), "MotionAPI"), "its schema shows");
    CHECK(!removeButton(pillNamed(pills(), "MotionAPI")) && !pills()->addButton()->isVisible(), "no × and no +");
    QStandardPaths::setTestModeEnabled(false);
}
