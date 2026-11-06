// The Material Editor panel, with real mouse events (docs/MATERIAL_EDITOR.md §5), and the
// Debug > Shader Nodes browser.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/material_editor_view_panel.h"
#include "usdcc/usd/shader_node_graphics.h"
#include "usdcc/usd/shader_node_palette.h"
#include "usdcc/usd/shader_nodes_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_shade_graph_model.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <string>

using usdcc::usd::MaterialEditorViewPanel;
using usdcc::usd::ShaderNodeGeometry;
using usdcc::usd::ShaderNodesDialog;
using usdcc::usd::ShadeGraphRow;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::UsdShadeGraphModel;

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 80) QApplication::processEvents();
}

const SdfPath kWood("/World/Looks/Wood");
const SdfPath kMaps("/World/Looks/Wood/Maps");
const SdfPath kSteel("/World/Looks/Steel");
const SdfPath kSurface("/World/Looks/Wood/Surface");
const SdfPath kNormal("/World/Looks/Wood/Normal");

// A panel in a window, over test_data/materials.usda.
struct Fixture {
    TestSettings settings;
    QTemporaryDir temp;
    StageManager stages;
    StageRefPtr stage;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    QMainWindow window;
    ads::CDockManager* docks;
    MaterialEditorViewPanel* panel;
    ads::CDockAreaWidget* area;

    Fixture() {
        // A copy: these edit it, and USD keeps a layer for whoever opens it next.
        const QString copy = temp.filePath("materials.usda");
        QFile::copy(QStringLiteral(USDCC_TEST_DATA_DIR "/materials.usda"), copy);
        stage = stages.openStage(copy.toStdString());
        docks = new ads::CDockManager(&window);
        panel = new MaterialEditorViewPanel(&stages, stage, undo, "Material Editor");
        panel->setObjectName("materialEditor");
        area = docks->addDockWidget(ads::CenterDockWidgetArea, panel);
        window.resize(1100, 800);
        window.show();
        settle();
    }

    QtNodes::NodeGraphicsObject* item(const SdfPath& path) const {
        const auto ids = panel->model()->nodesFor(path);
        return ids.empty() ? nullptr : panel->scene()->nodeGraphicsObject(ids.front());
    }
};

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

// Its middle: not a dot, which are on its sides.
QPointF grabPoint(QtNodes::NodeGraphicsObject* node) { return node->sceneBoundingRect().center(); }

// Most of a node is its body's colour: the rest is its caption and port names.
bool drawnInTheme(QtNodes::GraphicsView* view, QtNodes::NodeGraphicsObject* node) {
    view->centerOn(node);
    const QImage shot = view->grab().toImage();
    const QRect inside = view->mapFromScene(node->sceneBoundingRect().adjusted(12, 4, -12, -4)).boundingRect();
    int bodyPixels = 0, allPixels = 0;
    for (int y = inside.top(); y <= inside.bottom(); ++y) {
        for (int x = inside.left(); x <= inside.right(); ++x) {
            if (x < 0 || y < 0 || x >= shot.width() || y >= shot.height()) continue;
            const int red = shot.pixelColor(x, y).red();
            ++allPixels;
            if (red > 30 && red < 80) ++bodyPixels;
        }
    }
    return allPixels > 0 && bodyPixels * 2 > allPixels;
}
}  // namespace

USDCC_TEST(material_editor_shows_a_material) {
    Fixture f;
    CHECK(f.panel->shownLevel() == kWood, "it opens on the stage's first Material: %s", f.panel->shownLevel().GetText());
    CHECK(f.panel->breadcrumb() == QStringList{"Wood"}, "and says so: %s", qPrintable(f.panel->breadcrumb().join(",")));
    CHECK(!f.panel->isEmpty(), "it isn't empty");
    CHECK(f.panel->model()->allNodeIds().size() == 5, "Surface, Maps, Normal, and the Material's two: %d", int(f.panel->model()->allNodeIds().size()));
    auto* surface = f.item(kSurface);
    CHECK(surface != nullptr, "drawn");
    if (!surface) return;
    CHECK(f.panel->model()->allConnectionIds(surface->nodeId()).size() == 4, "with its four lines: %d",
          int(f.panel->model()->allConnectionIds(surface->nodeId()).size()));
    CHECK(f.panel->view()->actions().isEmpty(), "none of QtNodes' Undo, Delete or Paste are left: %d", int(f.panel->view()->actions().size()));

    // In the theme, at a size to read.
    const QtNodes::NodeStyle style(QJsonDocument::fromVariant(f.panel->model()->nodeData(surface->nodeId(), QtNodes::NodeRole::Style)).object());
    CHECK(style.Opacity == 1.0f, "a node reads the style: %g", style.Opacity);
    CHECK(drawnInTheme(f.panel->view(), surface), "its body is drawn in the theme");
    CHECK(f.panel->view()->transform().m11() >= 0.6, "at a size to read: %g", f.panel->view()->transform().m11());
}

USDCC_TEST(material_editor_stays_readable_when_short) {
    Fixture f;
    f.window.resize(900, 160);
    settle();
    f.panel->view()->hide();  // being shown is when QtNodes' view fits it itself
    f.panel->view()->show();
    settle();
    CHECK(f.panel->view()->transform().m11() >= 0.6, "in a short panel: %g", f.panel->view()->transform().m11());
}

USDCC_TEST(material_editor_drag_is_one_undo_step) {
    Fixture f;
    auto* node = f.item(kNormal);
    CHECK(node != nullptr, "it's drawn");
    if (!node) return;
    f.panel->view()->centerOn(node);
    const QPointF before = node->pos();
    QUndoStack* history = f.undo->stack(f.stage);
    const int stepsBefore = history->count();
    const UsdPrim prim = f.stage->usdStage()->GetPrimAtPath(kNormal);
    CHECK(!prim.HasAPI<UsdUINodeGraphNodeAPI>(), "nothing authored by drawing it");
    CHECK(!f.stage->usdStage()->GetRootLayer()->IsDirty(), "the layer isn't dirty from drawing it");

    const QPointF from = grabPoint(node);
    drag(f.panel->view(), from, from + QPointF(60, 40));
    node = f.item(kNormal);
    const QPointF moved = node->pos();
    CHECK(moved != before, "dragging moves it");

    GfVec2f pos;
    CHECK(prim.HasAPI<UsdUINodeGraphNodeAPI>(), "the API is applied");
    CHECK(UsdUINodeGraphNodeAPI(prim).GetPosAttr().Get(&pos), "with a position");
    // USD counts in nodes, not pixels.
    CHECK(qAbs(pos[0] * usdcc::usd::kShadeNodePixels - moved.x()) < 0.01 && qAbs(pos[1] * usdcc::usd::kShadeNodePixels - moved.y()) < 0.01, "where it's been dragged to: %g %g", pos[0], pos[1]);
    CHECK(history->count() == stepsBefore + 1, "one undo step, for all its moves: %d", history->count() - stepsBefore);

    history->undo();
    settle();
    node = f.item(kNormal);
    CHECK(node->pos() == before, "undoing moves it back");
    CHECK(!UsdPrim(prim).HasAPI<UsdUINodeGraphNodeAPI>(), "and takes the API away");
    history->redo();
    settle();
    node = f.item(kNormal);
    CHECK(qAbs(node->pos().x() - moved.x()) < 0.01 && qAbs(node->pos().y() - moved.y()) < 0.01, "redoing moves it again");

    // Another edit of the same attribute, from anywhere, moves it too.
    UsdUINodeGraphNodeAPI(prim).GetPosAttr().Set(GfVec2f(2, 1));
    settle();
    CHECK(f.item(kNormal)->pos() == QPointF(2 * usdcc::usd::kShadeNodePixels, usdcc::usd::kShadeNodePixels), "the stage is the only data");
    CHECK(history->count() == stepsBefore + 1, "a change from elsewhere isn't a step of ours: %d", history->count() - stepsBefore);

    // A click isn't an edit.
    node = f.item(kNormal);
    f.panel->view()->centerOn(node);
    const int steps = history->count();
    QTest::mouseClick(f.panel->view()->viewport(), Qt::LeftButton, Qt::NoModifier, f.panel->view()->mapFromScene(grabPoint(node)));
    QApplication::processEvents();
    CHECK(history->count() == steps, "a click adds nothing");
}

USDCC_TEST(material_editor_selection_both_ways) {
    Fixture f;
    // From the graph: clicking a node selects its prim.
    auto* node = f.item(kNormal);
    f.panel->view()->centerOn(node);
    QTest::mouseClick(f.panel->view()->viewport(), Qt::LeftButton, Qt::NoModifier, f.panel->view()->mapFromScene(grabPoint(node)));
    QApplication::processEvents();
    auto selected = f.stages.selectedPaths(f.stage);
    CHECK(selected.size() == 1 && selected[0] == kNormal, "clicking a node selects its prim");
    CHECK(f.panel->shownLevel() == kWood, "and stays where it is");

    // From the stage: a prim in another Material shows that Material, with its node selected.
    f.stages.setSelectedPaths(f.stage, {SdfPath("/World/Looks/Steel/Multiply")});
    settle();
    CHECK(f.panel->shownLevel() == kSteel, "a shader elsewhere goes to its Material: %s", f.panel->shownLevel().GetText());
    auto* multiply = f.item(SdfPath("/World/Looks/Steel/Multiply"));
    CHECK(multiply && multiply->isSelected(), "with its node selected");
    CHECK(f.stages.selectedPaths(f.stage).size() == 1, "and the selection unchanged by that");

    // A prim with one bound.
    f.stages.setSelectedPaths(f.stage, {SdfPath("/World/WoodBall")});
    settle();
    CHECK(f.panel->shownLevel() == kWood, "a bound prim shows its Material");
    CHECK(f.stages.selectedPaths(f.stage).size() == 1 && f.stages.selectedPaths(f.stage)[0] == SdfPath("/World/WoodBall"), "and keeps the selection");

    // Inside a NodeGraph.
    f.stages.setSelectedPaths(f.stage, {SdfPath("/World/Looks/Wood/Maps/Tex")});
    settle();
    CHECK(f.panel->shownLevel() == kMaps, "a shader in a NodeGraph enters it");
    CHECK(f.panel->breadcrumb() == (QStringList{"Wood", "Maps"}), "the breadcrumb says so");

    // What isn't a material leaves it be.
    f.stages.setSelectedPaths(f.stage, {SdfPath("/World")});
    settle();
    CHECK(f.panel->shownLevel() == kMaps, "something else doesn't move it");

    // A pin.
    f.panel->setPinned(true);
    f.stages.setSelectedPaths(f.stage, {kSteel});
    settle();
    CHECK(f.panel->shownLevel() == kMaps, "pinned, it stays");
    f.panel->setPinned(false);
    f.stages.setSelectedPaths(f.stage, {kSteel});
    settle();
    CHECK(f.panel->shownLevel() == kSteel, "unpinned, it follows");
}

USDCC_TEST(material_editor_enters_and_leaves_a_nodegraph) {
    Fixture f;
    auto* maps = f.item(kMaps);
    CHECK(maps != nullptr, "the NodeGraph is a node");
    if (!maps) return;
    f.panel->view()->centerOn(maps);
    QTest::mouseDClick(f.panel->view()->viewport(), Qt::LeftButton, Qt::NoModifier, f.panel->view()->mapFromScene(grabPoint(maps)));
    settle();
    CHECK(f.panel->shownLevel() == kMaps, "double-clicking enters it: %s", f.panel->shownLevel().GetText());
    CHECK(f.panel->breadcrumb() == (QStringList{"Wood", "Maps"}), "the breadcrumb goes on");
    CHECK(f.item(SdfPath("/World/Looks/Wood/Maps/Tex")) != nullptr, "its shaders show");
    CHECK(f.panel->model()->nodesFor(kMaps).size() == 2, "and its interface, twice");

    QToolButton* up = nullptr;
    for (QToolButton* button : f.panel->findChildren<QToolButton*>("materialEditorCrumb")) {
        if (button->isVisible() && button->text() == "Wood") up = button;
    }
    CHECK(up != nullptr, "there's a way back");
    if (!up) return;
    up->click();
    settle();
    CHECK(f.panel->shownLevel() == kWood, "the breadcrumb goes back up");
    CHECK(f.item(kSurface) != nullptr, "to its nodes");

    // A Shader isn't entered.
    auto* surface = f.item(kSurface);
    f.panel->view()->centerOn(surface);
    QTest::mouseDClick(f.panel->view()->viewport(), Qt::LeftButton, Qt::NoModifier, f.panel->view()->mapFromScene(grabPoint(surface)));
    settle();
    CHECK(f.panel->shownLevel() == kWood, "double-clicking a shader doesn't");
}

USDCC_TEST(material_editor_opens_a_page) {
    Fixture f;
    f.panel->showLevel(kSteel);
    settle();
    const SdfPath path("/World/Looks/Steel/Surface");
    auto* node = f.item(path);
    CHECK(node != nullptr, "standard_surface is drawn");
    if (!node) return;
    const auto* shade = f.panel->model()->shadeNode(node->nodeId());
    int pageRow = -1;
    bool wasOpen = true;
    for (std::size_t r = 0; r < shade->rows.size() && pageRow < 0; ++r) {
        if (shade->rows[r].kind == ShadeGraphRow::Kind::Page && !shade->rows[r].open) {
            pageRow = static_cast<int>(r);
            wasOpen = shade->rows[r].open;
        }
    }
    CHECK(pageRow >= 0 && !wasOpen, "with a closed page");
    if (pageRow < 0) return;
    const std::string page = shade->rows[static_cast<std::size_t>(pageRow)].page;
    const std::size_t inputsBefore = shade->in.size();
    const QPointF before = node->pos();

    const auto& geometry = static_cast<const ShaderNodeGeometry&>(f.panel->scene()->nodeGeometry());
    const QPointF rowInScene = node->mapToScene(geometry.rowRect(node->nodeId(), pageRow).center());
    f.panel->view()->centerOn(rowInScene);
    const int steps = f.undo->stack(f.stage)->count();
    QTest::mouseClick(f.panel->view()->viewport(), Qt::LeftButton, Qt::NoModifier, f.panel->view()->mapFromScene(rowInScene));
    settle();

    node = f.item(path);
    shade = f.panel->model()->shadeNode(node->nodeId());
    CHECK(shade->in.size() > inputsBefore, "clicking a page lists its inputs: %d, was %d", int(shade->in.size()), int(inputsBefore));
    CHECK(node->pos() == before, "without moving the node");
    CHECK(f.undo->stack(f.stage)->count() == steps, "or being an edit");
    CHECK(f.panel->model()->pageStates().count(usdcc::usd::pageKey(path, page)) == 1, "and it's remembered");
}

USDCC_TEST(material_editor_refreshes_live) {
    Fixture f;
    const UsdPrim normal = f.stage->usdStage()->GetPrimAtPath(kNormal);
    const UsdPrim surface = f.stage->usdStage()->GetPrimAtPath(kSurface);
    const auto surfaceLines = [&f]() { return f.panel->model()->allConnectionIds(f.item(kSurface)->nodeId()).size(); };
    const std::size_t lines = surfaceLines();

    // A connection, from anywhere.
    surface.GetAttribute(TfToken("inputs:normal")).ClearConnections();
    settle();
    CHECK(surfaceLines() == lines - 1, "a removed connection goes: %d", int(surfaceLines()));
    f.undo->stack(f.stage);

    // A value isn't anything to redraw for, and leaves the items be.
    auto* before = f.item(kSurface);
    surface.GetAttribute(TfToken("inputs:roughness")).Set(0.1f);
    settle();
    CHECK(f.item(kSurface) == before, "a new value doesn't rebuild the scene");

    // A new prim.
    f.stage->usdStage()->DefinePrim(SdfPath("/World/Looks/Wood/Extra"), TfToken("Shader"));
    settle();
    CHECK(f.item(SdfPath("/World/Looks/Wood/Extra")) != nullptr, "a new shader shows");
    f.stage->usdStage()->RemovePrim(SdfPath("/World/Looks/Wood/Extra"));
    settle();
    CHECK(f.item(SdfPath("/World/Looks/Wood/Extra")) == nullptr, "and goes");

    // The Material itself going: it finds another.
    f.stage->usdStage()->RemovePrim(kWood);
    settle();
    CHECK(f.panel->shownLevel() == kSteel, "when its Material goes, it picks another: %s", f.panel->shownLevel().GetText());
    f.stage->usdStage()->RemovePrim(kSteel);
    settle();
    CHECK(f.panel->isEmpty(), "and says when there's none");
}

// Issue #529 of QtNodes: a view in a tab or a floating dock came back black.
USDCC_TEST(material_editor_draws_after_tabbing_and_floating) {
    Fixture f;
    auto* other = new ads::CDockWidget("Other");
    other->setObjectName("other");
    other->setWidget(new QWidget);
    f.docks->addDockWidgetTabToArea(other, f.area);
    settle();
    other->raise();
    settle();
    CHECK(!f.panel->view()->isVisible(), "another tab is in front");
    f.panel->raise();
    settle();
    auto* node = f.item(kSurface);
    CHECK(f.panel->view()->isVisible() && drawnInTheme(f.panel->view(), node), "it draws when its tab comes back");

    f.panel->setFloating();
    settle();
    CHECK(f.panel->view()->window() != &f.window && drawnInTheme(f.panel->view(), node), "and in a floating window");
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

namespace {
QPointF portScene(const Fixture& f, const SdfPath& path, QtNodes::PortType side, const char* attribute, usdcc::usd::ShadeNodeKind kind = usdcc::usd::ShadeNodeKind::Shader) {
    for (QtNodes::NodeId id : f.panel->model()->nodesFor(path)) {
        const auto* shade = f.panel->model()->shadeNode(id);
        if (shade->kind != kind) continue;
        const auto& ports = side == QtNodes::PortType::In ? shade->in : shade->out;
        for (std::size_t i = 0; i < ports.size(); ++i) {
            if (ports[i].attribute != attribute) continue;
            auto* node = f.panel->scene()->nodeGraphicsObject(id);
            return node->mapToScene(f.panel->scene()->nodeGeometry().portPosition(id, side, static_cast<QtNodes::PortIndex>(i)));
        }
    }
    return QPointF(-100000, -100000);
}

// Everything in view, so a drag between two ports lands.
void showAll(Fixture& f) {
    f.panel->view()->setTransform(QTransform::fromScale(0.5, 0.5));
    f.panel->view()->centerOn(f.panel->scene()->itemsBoundingRect().center());
    settle();
}

SdfPath sourceOf(const Fixture& f, const char* attribute) {
    SdfPathVector sources;
    const UsdAttribute attr = f.stage->usdStage()->GetAttributeAtPath(SdfPath(attribute));
    if (attr) attr.GetConnections(&sources);
    return sources.empty() ? SdfPath() : sources.front();
}
}  // namespace

USDCC_TEST(material_editor_connects_by_dragging_a_line) {
    Fixture f;
    showAll(f);
    QUndoStack* history = f.undo->stack(f.stage);
    const int steps = history->index();

    // To an input that isn't authored yet.
    const QPointF from = portScene(f, kNormal, QtNodes::PortType::Out, "outputs:rgb");
    const QPointF to = portScene(f, kSurface, QtNodes::PortType::In, "inputs:emissiveColor");
    CHECK(from.x() > -1000 && to.x() > -1000, "both ports are there");
    drag(f.panel->view(), from, to);
    settle();
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:emissiveColor") == SdfPath("/World/Looks/Wood/Normal.outputs:rgb"), "dragging from an output to an input connects them");
    CHECK(history->index() == steps + 1, "one undo step: %d", history->index() - steps);
    CHECK(f.stage->usdStage()->GetPrimAtPath(kSurface).GetAttribute(TfToken("inputs:emissiveColor")).GetTypeName() == SdfValueTypeNames->Color3f, "the input is created with its type");
    history->undo();
    settle();
    CHECK(!f.stage->usdStage()->GetPrimAtPath(kSurface).GetAttribute(TfToken("inputs:emissiveColor")), "undo takes it away");

    // A type it can't take.
    showAll(f);
    const int stepsBefore = history->index();
    drag(f.panel->view(), portScene(f, kNormal, QtNodes::PortType::Out, "outputs:r"), portScene(f, kSurface, QtNodes::PortType::In, "inputs:emissiveColor"));
    settle();
    CHECK(history->index() == stepsBefore, "a float output doesn't land on a color");
    // What feeds it can't take from it: a loop.
    showAll(f);
    drag(f.panel->view(), portScene(f, kSurface, QtNodes::PortType::Out, "outputs:surface"), portScene(f, kNormal, QtNodes::PortType::In, "inputs:file"));
    settle();
    CHECK(history->index() == stepsBefore, "nor a surface on a texture's file");
}

USDCC_TEST(material_editor_offers_only_a_systems_terminals) {
    Fixture f;
    f.panel->showLevel(kSteel);
    settle();
    showAll(f);
    CHECK(portScene(f, kSteel, QtNodes::PortType::In, "outputs:mtlx:surface", usdcc::usd::ShadeNodeKind::Outputs).x() > -1000, "what's authored");
    CHECK(portScene(f, kSteel, QtNodes::PortType::In, "outputs:displacement", usdcc::usd::ShadeNodeKind::Outputs).x() < -1000, "and not a preview terminal for MaterialX shaders");
}

USDCC_TEST(material_editor_pulling_a_line_off_is_one_step) {
    Fixture f;
    showAll(f);
    QUndoStack* history = f.undo->stack(f.stage);
    const int steps = history->index();
    const QPointF input = portScene(f, kSurface, QtNodes::PortType::In, "inputs:normal");
    drag(f.panel->view(), input, input + QPointF(-60, 400));  // onto nothing
    settle();
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal").IsEmpty(), "pulling a line off a port disconnects it");
    CHECK(history->index() == steps + 1, "one undo step: %d", history->index() - steps);
    history->undo();
    settle();
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal") == SdfPath("/World/Looks/Wood/Normal.outputs:rgb"), "undo puts it back");

    // Onto another input it fits: still one step.
    showAll(f);
    const int again = history->index();
    drag(f.panel->view(), portScene(f, kSurface, QtNodes::PortType::In, "inputs:diffuseColor"), portScene(f, kSurface, QtNodes::PortType::In, "inputs:emissiveColor"));
    settle();
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:diffuseColor").IsEmpty(), "it leaves the first");
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:emissiveColor") == SdfPath("/World/Looks/Wood/Maps.outputs:color"), "and lands on the second");
    CHECK(history->index() == again + 1, "one undo step: %d", history->index() - again);
    history->undo();
    settle();
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:diffuseColor") == SdfPath("/World/Looks/Wood/Maps.outputs:color"), "and undo puts it back");
}

USDCC_TEST(material_editor_keys) {
    Fixture f;
    f.panel->view()->setFocus();
    QUndoStack* history = f.undo->stack(f.stage);

    // Delete.
    f.stages.setSelectedPaths(f.stage, {kNormal});
    settle();
    int steps = history->index();
    QTest::keyClick(f.panel->view(), Qt::Key_Delete);
    settle();
    CHECK(!f.stage->usdStage()->GetPrimAtPath(kNormal), "Delete removes the selected node");
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal").IsEmpty(), "and what's connected to it");
    CHECK(history->index() == steps + 1, "one undo step");
    history->undo();
    settle();
    CHECK(f.item(kNormal) != nullptr, "undo brings it back");

    // Duplicate.
    f.stages.setSelectedPaths(f.stage, {kNormal});
    settle();
    steps = history->index();
    QTest::keyClick(f.panel->view(), Qt::Key_D, Qt::ControlModifier);
    settle();
    CHECK(f.item(SdfPath("/World/Looks/Wood/Normal1")) != nullptr, "Ctrl+D duplicates it");
    CHECK(history->index() == steps + 1, "one undo step");
    CHECK(f.stages.selectedPaths(f.stage) == std::vector<SdfPath>{SdfPath("/World/Looks/Wood/Normal1")}, "and selects the copy");
    history->undo();
    settle();

    // Copy and paste.
    f.stages.setSelectedPaths(f.stage, {kNormal});
    settle();
    QTest::keyClick(f.panel->view(), Qt::Key_C, Qt::ControlModifier);
    steps = history->index();
    QTest::keyClick(f.panel->view(), Qt::Key_V, Qt::ControlModifier);
    settle();
    CHECK(f.item(SdfPath("/World/Looks/Wood/Normal1")) != nullptr, "Ctrl+V pastes the copy");
    CHECK(history->index() == steps + 1, "one undo step");
    history->undo();
    settle();

    // Rename.
    f.stages.setSelectedPaths(f.stage, {kNormal});
    settle();
    QTest::keyClick(f.panel->view(), Qt::Key_F2);
    CHECK(f.panel->inlineEdit() != nullptr, "F2 starts renaming");
    if (!f.panel->inlineEdit()) return;
    steps = history->index();
    f.panel->inlineEdit()->setText("NormalMap");
    QTest::keyClick(f.panel->inlineEdit(), Qt::Key_Return);
    settle();
    const SdfPath renamed("/World/Looks/Wood/NormalMap");
    CHECK(f.stage->usdStage()->GetPrimAtPath(renamed), "Return renames it");
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal") == renamed.AppendProperty(TfToken("outputs:rgb")), "its connections follow: %s", sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal").GetText());
    CHECK(f.item(renamed) != nullptr, "the node's there");
    CHECK(history->index() == steps + 1, "one undo step");

    QTest::keyClick(f.panel->view(), Qt::Key_F2);
    if (f.panel->inlineEdit()) {
        f.panel->inlineEdit()->setText("Other");
        QTest::keyClick(f.panel->inlineEdit(), Qt::Key_Escape);
        settle();
        CHECK(f.stage->usdStage()->GetPrimAtPath(renamed), "Escape doesn't");
    }
}

USDCC_TEST(material_editor_tab_search_adds_a_node) {
    Fixture f;
    f.panel->view()->setFocus();
    QUndoStack* history = f.undo->stack(f.stage);
    const int steps = history->index();
    QTest::keyClick(f.panel->view(), Qt::Key_Tab);
    settle();
    usdcc::usd::ShaderNodeSearchPopup* popup = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* found = qobject_cast<usdcc::usd::ShaderNodeSearchPopup*>(widget); found && found->isVisible()) popup = found;
    }
    CHECK(popup != nullptr, "Tab opens the search");
    if (!popup) return;
    QTreeWidget* tree = popup->palette()->tree();
    CHECK(tree->topLevelItem(0)->text(0) == "USD Preview", "what the graph uses comes first: %s", qPrintable(tree->topLevelItem(0)->text(0)));
    popup->palette()->search()->setText("UsdTransform2d");
    CHECK(popup->palette()->firstMatch() == "UsdTransform2d", "typing finds it: %s", qPrintable(popup->palette()->firstMatch()));
    QTest::keyClick(popup->palette()->search(), Qt::Key_Return);
    settle();
    const SdfPath created("/World/Looks/Wood/UsdTransform2d");
    CHECK(f.stage->usdStage()->GetPrimAtPath(created), "Return adds it");
    CHECK(history->index() == steps + 1, "one undo step");
    CHECK(f.item(created) != nullptr, "and it's drawn");
    CHECK(f.stages.selectedPaths(f.stage) == std::vector<SdfPath>{created}, "and selected");

    usdcc::usd::ShaderNodePalette palette;
    palette.search()->setText("no node is called this");
    CHECK(palette.firstMatch().isEmpty(), "a search can find nothing");
    palette.search()->setText("standard_surface");
    CHECK(palette.firstMatch().contains("standard_surface"), "MaterialX is there too: %s", qPrintable(palette.firstMatch()));
}

USDCC_TEST(material_editor_group_ungroup_and_layout) {
    Fixture f;
    f.panel->view()->setFocus();
    QUndoStack* history = f.undo->stack(f.stage);

    f.stages.setSelectedPaths(f.stage, {kNormal});
    settle();
    int steps = history->index();
    QTest::keyClick(f.panel->view(), Qt::Key_G, Qt::ControlModifier);
    settle();
    const SdfPath graph("/World/Looks/Wood/NodeGraph");
    CHECK(f.stage->usdStage()->GetPrimAtPath(graph.AppendChild(TfToken("Normal"))), "Ctrl+G groups it");
    CHECK(history->index() == steps + 1, "one undo step");
    CHECK(f.item(graph) != nullptr && !f.item(kNormal), "it's one node now");
    CHECK(f.stages.selectedPaths(f.stage) == std::vector<SdfPath>{graph}, "and selected");

    steps = history->index();
    QTest::keyClick(f.panel->view(), Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(f.item(kNormal) != nullptr && !f.item(graph), "Ctrl+Shift+G ungroups it");
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal") == SdfPath("/World/Looks/Wood/Normal.outputs:rgb"), "connected as it was");
    CHECK(history->index() == steps + 1, "one undo step");

    // Laying out.
    steps = history->index();
    QTest::keyClick(f.panel->view(), Qt::Key_L);
    settle();
    CHECK(history->index() == steps + 1, "L lays them out, one undo step");
    GfVec2f pos;
    CHECK(UsdUINodeGraphNodeAPI(f.stage->usdStage()->GetPrimAtPath(kSurface)).GetPosAttr().Get(&pos), "and stores where");
    CHECK(f.item(kSurface)->pos().x() < portScene(f, kWood, QtNodes::PortType::In, "outputs:surface", usdcc::usd::ShadeNodeKind::Outputs).x(), "feeding what's to its right");
    // Read back by a new panel.
    const QPointF placed = f.item(kSurface)->pos();
    auto* second = new MaterialEditorViewPanel(&f.stages, f.stage, f.undo, "Second");
    second->setObjectName("second");
    f.docks->addDockWidget(ads::RightDockWidgetArea, second);
    settle();
    const auto ids = second->model()->nodesFor(kSurface);
    const QPointF read = ids.empty() ? QPointF() : second->model()->nodeData(ids.front(), QtNodes::NodeRole::Position).toPointF();
    CHECK(qAbs(read.x() - placed.x()) < 0.01 && qAbs(read.y() - placed.y()) < 0.01, "a new panel reads the same layout");
}

USDCC_TEST(material_editor_backdrops_carry_nodes) {
    Fixture f;
    showAll(f);
    QUndoStack* history = f.undo->stack(f.stage);
    int steps = history->index();
    f.stages.setSelectedPaths(f.stage, {kNormal});
    settle();
    const SdfPath backdrop = f.panel->addBackdrop(QPointF(0, 0));
    settle();
    CHECK(!backdrop.IsEmpty() && history->index() == steps + 1, "a new backdrop, one undo step");
    CHECK(f.panel->backdropItems().size() == 1, "drawn");
    if (f.panel->backdropItems().empty()) return;
    auto* item = f.panel->backdropItems().front();
    const QRectF rect = item->sceneRectOf();
    auto* normal = f.item(kNormal);
    CHECK(rect.contains(normal->sceneBoundingRect().center()), "around what was selected");
    CHECK(item->zValue() < normal->zValue(), "and behind it");

    showAll(f);
    steps = history->index();
    const QPointF normalBefore = normal->pos();
    const QPointF surfaceBefore = f.item(kSurface)->pos();
    // Its title bar, not the node on it.
    const QPointF grab = rect.topLeft() + QPointF(12, 8);
    drag(f.panel->view(), grab, grab + QPointF(80, 60));
    settle();
    CHECK(history->index() == steps + 1, "moving it is one undo step: %d", history->index() - steps);
    CHECK(f.item(kNormal)->pos() != normalBefore, "the node on it goes along");
    CHECK(f.item(kSurface)->pos() == surfaceBefore, "and one not on it doesn't");
    const UsdPrim prim = f.stage->usdStage()->GetPrimAtPath(backdrop);
    GfVec2f pos;
    CHECK(UsdUINodeGraphNodeAPI(prim).GetPosAttr().Get(&pos) && qAbs(pos[0] * usdcc::usd::kShadeNodePixels - f.panel->backdropItems().front()->pos().x()) < 0.01, "its place is stored");
    history->undo();
    settle();
    CHECK(qAbs(f.item(kNormal)->pos().x() - normalBefore.x()) < 0.01, "undoing moves them all back");
    CHECK(qAbs(f.panel->backdropItems().front()->sceneRectOf().x() - rect.x()) < 0.01, "and it");
}

USDCC_TEST(material_editor_shaking_a_node_disconnects_it) {
    Fixture f;
    showAll(f);
    QUndoStack* history = f.undo->stack(f.stage);
    const int steps = history->index();
    auto* node = f.item(kNormal);
    const QPoint start = f.panel->view()->mapFromScene(grabPoint(node));
    QWidget* viewport = f.panel->view()->viewport();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
    for (int i = 1; i <= 10; ++i) {
        const QPoint at = start + QPoint(i % 2 ? 40 : -40, 0);
        QMouseEvent move(QEvent::MouseMove, QPointF(at), QPointF(viewport->mapToGlobal(at)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QApplication::processEvents();
    }
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, start);
    settle();
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal").IsEmpty(), "shaking a node takes its lines off");
    CHECK(history->index() == steps + 1, "one undo step with its move: %d", history->index() - steps);
    history->undo();
    settle();
    CHECK(sourceOf(f, "/World/Looks/Wood/Surface.inputs:normal") == SdfPath("/World/Looks/Wood/Normal.outputs:rgb"), "undo puts them back");
}

USDCC_TEST(material_editor_menu_offers_what_is_under_the_mouse) {
    Fixture f;
    showAll(f);
    const auto texts = [&f](QPointF at) {
        QMenu menu;
        f.panel->populateGraphMenu(&menu, at);
        QStringList names;
        for (QAction* action : menu.actions()) names << action->text();
        return names;
    };
    const QPointF corner = f.panel->view()->mapToScene(f.panel->view()->viewport()->rect().bottomRight() - QPoint(4, 4));
    const QStringList background = texts(corner);
    CHECK(background.contains("Add Node...") && background.contains("Auto Layout") && background.contains("Add Backdrop"), "on nothing: %s", qPrintable(background.join(",")));
    const QStringList shader = texts(grabPoint(f.item(kNormal)));
    CHECK(shader.contains("Rename") && shader.contains("Delete") && shader.contains("Group into NodeGraph"), "on a shader: %s", qPrintable(shader.join(",")));
    CHECK(!shader.contains("Enter"), "not what's for a NodeGraph");
    const QStringList group = texts(grabPoint(f.item(kMaps)));
    CHECK(group.contains("Enter") && group.contains("Ungroup"), "on a NodeGraph: %s", qPrintable(group.join(",")));
    const QStringList port = texts(portScene(f, kSurface, QtNodes::PortType::In, "inputs:normal") + QPointF(30, 0));
    CHECK(port.contains("Disconnect normal"), "on a connected port: %s", qPrintable(port.join(",")));
    const QStringList free = texts(portScene(f, kSurface, QtNodes::PortType::In, "inputs:metallic") + QPointF(30, 0));
    CHECK(free.contains("Promote metallic to the Interface"), "on a free one: %s", qPrintable(free.join(",")));
}

USDCC_TEST(attribute_editor_offers_a_definitions_inputs) {
    TestSettings settings;
    QTemporaryDir temp;
    const QString copy = temp.filePath("materials.usda");
    QFile::copy(QStringLiteral(USDCC_TEST_DATA_DIR "/materials.usda"), copy);
    StageManager stages;
    StageRefPtr stage = stages.openStage(copy.toStdString());
    auto undo = std::make_shared<UndoManager>();
    usdcc::usd::AttributesViewPanel panel(&stages, stage, undo, "Attributes");
    stages.setSelectedPaths(stage, {kSurface});
    settle();
    const auto rowFor = [&panel](const QString& attribute) -> QWidget* {
        for (QWidget* row : panel.findChildren<QWidget*>("availableInput")) {
            if (row->property("attribute").toString() == attribute) return row;
        }
        return nullptr;
    };
    QWidget* row = rowFor("inputs:metallic");
    CHECK(row != nullptr, "an unauthored input of the definition is listed");
    CHECK(rowFor("inputs:roughness") == nullptr, "one that's authored isn't");
    if (!row) return;
    QUndoStack* history = undo->stack(stage);
    const int steps = history->index();
    row->findChild<QToolButton*>()->click();
    settle();
    settle();
    CHECK(stage->usdStage()->GetPrimAtPath(kSurface).GetAttribute(TfToken("inputs:metallic")), "Add creates it");
    CHECK(history->index() == steps + 1, "one undo step");
    CHECK(rowFor("inputs:metallic") == nullptr, "and it's not on offer any more");
}
