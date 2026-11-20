// The Schemas panel (docs/SCHEMAS.md §4): categories, typed schemas nested by
// inheritance, the details and properties of the chosen one, links between
// schemas, the filter and Show, Show Source, and its session state. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/schema_explorer_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_view.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>

using usdcc::usd::SchemaExplorerViewPanel;
using usdcc::usd::StageManager;

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
    SchemaExplorerViewPanel panel(&stages, nullptr);
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
    SchemaExplorerViewPanel copy(&stages, nullptr);
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
