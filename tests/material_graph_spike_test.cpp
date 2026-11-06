// Phase 0's exit criteria (docs/MATERIAL_EDITOR.md §5), with real mouse events: dragging a
// node writes its position through an edit session and undoes in one step. And the
// Debug > Shader Nodes browser. Goes with the spike when phase 2 replaces it.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/usd/material_graph_spike_model.h"
#include "usdcc/usd/material_graph_spike_panel.h"
#include "usdcc/usd/shader_nodes_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>

#include <fstream>
#include <functional>
#include <memory>

using usdcc::usd::MaterialGraphSpikePanel;
using usdcc::usd::ShaderNodesDialog;
using usdcc::usd::StageManager;
using usdcc::usd::UndoManager;

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 80) QApplication::processEvents();
}

constexpr auto kMaterial = R"(#usda 1.0
def Material "Mat"
{
    token outputs:surface.connect = </Mat/Surface.outputs:surface>

    def Shader "Surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </Mat/Tex.outputs:rgb>
        token outputs:surface
    }

    def Shader "Tex"
    {
        uniform token info:id = "UsdUVTexture"
        color3f outputs:rgb
    }
}
)";

// A press, moves and a release through the view, as a mouse does it.
void drag(QtNodes::GraphicsView* view, QPointF from, QPointF to) {
    QWidget* viewport = view->viewport();
    const QPoint start = view->mapFromScene(from);
    const QPoint end = view->mapFromScene(to);
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 5; ++step) {
        const QPoint at = start + (end - start) * step / 5;
        QMouseEvent move(QEvent::MouseMove, QPointF(at), QPointF(viewport->mapToGlobal(at)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QApplication::processEvents();
    }
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
    QApplication::processEvents();
}

// Its middle: not a port, which are at the sides, nor the margin around it for them.
QPointF grabPoint(QtNodes::NodeGraphicsObject* node) { return node->sceneBoundingRect().center(); }

// Most of a node, inside the margin for its ports, is its body's colour: the rest is its
// caption and port names.
bool drawnInTheme(QtNodes::GraphicsView* view, QtNodes::NodeGraphicsObject* node) {
    const QImage shot = view->grab().toImage();
    const QRect inside = view->mapFromScene(node->sceneBoundingRect().adjusted(24, 24, -24, -24)).boundingRect();
    int bodyPixels = 0, allPixels = 0;
    for (int y = inside.top(); y <= inside.bottom(); ++y) {
        for (int x = inside.left(); x <= inside.right(); ++x) {
            const int red = shot.pixelColor(x, y).red();
            ++allPixels;
            if (red > 30 && red < 80) ++bodyPixels;
        }
    }
    return allPixels > 0 && bodyPixels * 2 > allPixels;
}
}  // namespace

USDCC_TEST(material_spike_shows_a_material) {
    TestSettings settings;
    QTemporaryDir temp;
    const QString file = temp.filePath("mat.usda");
    std::ofstream(file.toStdString()) << kMaterial;
    StageManager stages;
    auto stage = stages.openStage(file.toStdString());
    auto undo = std::make_shared<UndoManager>();
    MaterialGraphSpikePanel panel(&stages, stage, undo, "Spike");

    CHECK(panel.shownMaterial() == SdfPath("/Mat"), "it finds the stage's Material");
    auto* model = panel.model();
    CHECK(model && model->allNodeIds().size() == 3, "the Material and its two shaders: %d", model ? int(model->allNodeIds().size()) : -1);
    if (!model) return;
    const auto surface = model->nodeFor(SdfPath("/Mat/Surface"));
    const auto material = model->nodeFor(SdfPath("/Mat"));
    CHECK(model->allConnectionIds(surface).size() == 2, "Surface is connected both ways: %d", int(model->allConnectionIds(surface).size()));
    CHECK(model->allConnectionIds(material).size() == 1, "the terminal to Surface");

    // QtNodes' own keys would edit its copy, not the stage.
    CHECK(panel.view()->actions().isEmpty(), "none of QtNodes' Undo, Delete or Paste are left: %d", int(panel.view()->actions().size()));
}

USDCC_TEST(material_spike_drag_is_one_undo_step) {
    TestSettings settings;
    QTemporaryDir temp;
    const QString file = temp.filePath("mat.usda");
    std::ofstream(file.toStdString()) << kMaterial;
    StageManager stages;
    auto stage = stages.openStage(file.toStdString());
    auto undo = std::make_shared<UndoManager>();
    // A dock widget only shows inside a dock manager.
    QMainWindow window;
    auto* docks = new ads::CDockManager(&window);
    auto* panelOwned = new MaterialGraphSpikePanel(&stages, stage, undo, "Spike");
    panelOwned->setObjectName("spike");
    MaterialGraphSpikePanel& panel = *panelOwned;
    docks->addDockWidget(ads::CenterDockWidgetArea, panelOwned);
    window.resize(900, 600);
    window.show();
    settle();
    panel.showMaterial(panel.shownMaterial());  // fits the view now it has a size
    settle();
    CHECK(panel.view()->isVisible(), "the view is on screen");

    // A short panel, as it opens under the others, doesn't shrink it past reading.
    window.resize(900, 160);
    settle();
    panel.showMaterial(panel.shownMaterial());
    CHECK(panel.view()->transform().m11() >= 0.6, "at a size to read in a short panel: %g", panel.view()->transform().m11());
    // Being shown, as when it opens or its tab is picked, is when QtNodes' view fits it itself.
    panel.view()->hide();
    panel.view()->show();
    settle();
    CHECK(panel.view()->transform().m11() >= 0.6, "and still when it's shown again: %g", panel.view()->transform().m11());
    window.resize(900, 600);
    settle();
    panel.showMaterial(panel.shownMaterial());
    settle();

    auto* model = panel.model();
    auto* scene = panel.scene();
    const auto id = model->nodeFor(SdfPath("/Mat/Tex"));
    auto* node = scene->nodeGraphicsObject(id);
    CHECK(node != nullptr, "it's drawn");
    if (!node) return;

    // In the theme: the painter takes a node's style through QJsonDocument::fromVariant(),
    // which turns anything but a map into nothing, and a black box.
    const QtNodes::NodeStyle style(QJsonDocument::fromVariant(model->nodeData(id, QtNodes::NodeRole::Style)).object());
    CHECK(style.FontColor == QColor(225, 225, 228), "a node reads the theme's style: %s", qPrintable(style.FontColor.name()));
    CHECK(drawnInTheme(panel.view(), node), "its body is drawn in the theme");
    CHECK(panel.view()->transform().m11() >= 0.6, "at a size to read: %g", panel.view()->transform().m11());
    const QPointF before = node->pos();
    QUndoStack* history = undo->stack(stage);
    const int stepsBefore = history->count();
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(SdfPath("/Mat/Tex"));
    CHECK(!prim.HasAPI<UsdUINodeGraphNodeAPI>(), "nothing authored by drawing it");
    CHECK(!stage->usdStage()->GetRootLayer()->IsDirty(), "the layer isn't dirty from drawing it");

    const QPointF from = grabPoint(node);
    drag(panel.view(), from, from + QPointF(60, 40));
    const QPointF moved = node->pos();
    CHECK(moved != before, "dragging moves it");

    GfVec2f pos;
    CHECK(prim.HasAPI<UsdUINodeGraphNodeAPI>(), "the API is applied");
    CHECK(UsdUINodeGraphNodeAPI(prim).GetPosAttr().Get(&pos), "with a position");
    CHECK(qFuzzyCompare(float(moved.x()), pos[0]) && qFuzzyCompare(float(moved.y()), pos[1]), "where it's been dragged to: %g %g", pos[0], pos[1]);
    CHECK(history->count() == stepsBefore + 1, "one undo step, for all its moves: %d", history->count() - stepsBefore);

    history->undo();
    settle();
    CHECK(node->pos() == before, "undoing moves it back");
    CHECK(!UsdPrim(prim).HasAPI<UsdUINodeGraphNodeAPI>(), "and takes the API away");
    history->redo();
    settle();
    CHECK(qFuzzyCompare(float(node->pos().x()), float(moved.x())) && qFuzzyCompare(float(node->pos().y()), float(moved.y())), "redoing moves it again");

    // Another edit of the same attribute, from anywhere, moves it too.
    UsdUINodeGraphNodeAPI(prim).GetPosAttr().Set(GfVec2f(500, 7));
    settle();
    CHECK(node->pos() == QPointF(500, 7), "the stage is the only data");
    CHECK(history->count() == stepsBefore + 1, "a change from elsewhere isn't a step of ours: %d", history->count() - stepsBefore);

    // Dragging again is a second step, not more of the first.
    const QPointF again = grabPoint(node);
    drag(panel.view(), again, again + QPointF(-30, 30));
    CHECK(history->count() == stepsBefore + 2, "a second drag is a second step: %d", history->count() - stepsBefore);

    // A click isn't an edit.
    const int steps = history->count();
    const QPoint click = panel.view()->mapFromScene(grabPoint(node));
    QTest::mouseClick(panel.view()->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QApplication::processEvents();
    CHECK(history->count() == steps, "a click adds nothing");
}

// Issue #529 of QtNodes: a view in a tab or a floating dock came back black.
USDCC_TEST(material_spike_draws_after_tabbing_and_floating) {
    TestSettings settings;
    QTemporaryDir temp;
    const QString file = temp.filePath("mat.usda");
    std::ofstream(file.toStdString()) << kMaterial;
    StageManager stages;
    auto stage = stages.openStage(file.toStdString());
    auto undo = std::make_shared<UndoManager>();
    QMainWindow window;
    auto* docks = new ads::CDockManager(&window);
    auto* panel = new MaterialGraphSpikePanel(&stages, stage, undo, "Spike");
    panel->setObjectName("spike");
    auto* other = new ads::CDockWidget("Other");
    other->setObjectName("other");
    other->setWidget(new QWidget);
    auto* area = docks->addDockWidget(ads::CenterDockWidgetArea, panel);
    docks->addDockWidgetTabToArea(other, area);
    window.resize(900, 600);
    window.show();
    settle();

    auto* node = panel->scene()->nodeGraphicsObject(panel->model()->nodeFor(SdfPath("/Mat/Tex")));
    other->raise();
    settle();
    CHECK(!panel->view()->isVisible(), "another tab is in front");
    panel->raise();
    settle();
    panel->view()->centerOn(node);
    CHECK(panel->view()->isVisible() && drawnInTheme(panel->view(), node), "it draws when its tab comes back");

    panel->setFloating();
    settle();
    panel->view()->centerOn(node);
    CHECK(panel->view()->window() != &window && drawnInTheme(panel->view(), node), "and in a floating window");
}

USDCC_TEST(shader_nodes_dialog) {
    TestSettings settings;
    ShaderNodesDialog dialog;
    QTreeWidget* tree = dialog.tree();
    QStringList roots;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) roots << tree->topLevelItem(i)->text(0);
    CHECK(roots.contains("MaterialX") && roots.contains("USD Preview"), "by shading system: %s", qPrintable(roots.join(",")));

    const std::function<QTreeWidgetItem*(QTreeWidgetItem*, const QString&)> find = [&find](QTreeWidgetItem* item, const QString& identifier) -> QTreeWidgetItem* {
        if (item->data(0, Qt::UserRole).toString() == identifier) return item;
        for (int i = 0; i < item->childCount(); ++i) {
            if (QTreeWidgetItem* found = find(item->child(i), identifier)) return found;
        }
        return nullptr;
    };
    QTreeWidgetItem* preview = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !preview; ++i) preview = find(tree->topLevelItem(i), "UsdPreviewSurface");
    CHECK(preview != nullptr, "UsdPreviewSurface is listed");
    if (!preview) return;
    tree->setCurrentItem(preview);
    const QString details = dialog.details()->toPlainText();
    CHECK(details.contains("diffuseColor") && details.contains("color3f"), "its inputs and their types show: %s", qPrintable(details.left(200)));

    dialog.search()->setText("standard_surface");
    CHECK(preview->isHidden(), "a search hides what doesn't match");
    QTreeWidgetItem* standard = nullptr;
    for (int i = 0; i < tree->topLevelItemCount() && !standard; ++i) standard = find(tree->topLevelItem(i), "ND_standard_surface_surfaceshader");
    CHECK(standard && !standard->isHidden(), "and keeps what does");
    dialog.search()->clear();
    CHECK(!preview->isHidden(), "clearing it brings them back");
}
