#include "usdcc/usd/material_editor_view_panel.h"

#include "usdcc/ui/theme.h"
#include "usdcc/usd/shade_graph.h"
#include "usdcc/usd/shader_node_graphics.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_shade_graph_model.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsView>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
constexpr double kSmallestToOpenAt = 0.6;  // smaller than this can't be read
constexpr double kSmallestToFrame = 0.2;

// The app's dark theme, for what QtNodes draws itself. Its styles are process-wide.
void applyTheme() {
    const QString accent = usdcc::ui::theme::accent().name();
    QtNodes::GraphicsViewStyle::setStyle(QStringLiteral(R"({"GraphicsViewStyle": {
        "BackgroundColor": [28, 28, 30], "FineGridColor": [36, 36, 38], "CoarseGridColor": [46, 46, 49]}})"));
    // ShaderNodePainter draws the nodes: these are what QtNodes still reads, such as the opacity.
    QtNodes::NodeStyle::setNodeStyle(QStringLiteral(R"({"NodeStyle": {
        "NormalBoundaryColor": [70, 70, 74], "SelectedBoundaryColor": "%1",
        "GradientColor0": [50, 50, 54], "GradientColor1": [46, 46, 50], "GradientColor2": [44, 44, 48], "GradientColor3": [42, 42, 46],
        "ShadowColor": [10, 10, 10], "ShadowEnabled": false, "FontColor": [225, 225, 228], "FontColorFaded": [140, 140, 145],
        "ConnectionPointColor": [160, 160, 164], "FilledConnectionPointColor": "%1", "ErrorColor": [255, 95, 86], "WarningColor": [255, 189, 46],
        "ToolTipIconColor": "white", "PenWidth": 1.0, "HoveredPenWidth": 1.5, "ConnectionPointDiameter": 9.0, "Opacity": 1.0}})")
                                      .arg(accent));
    QtNodes::ConnectionStyle::setConnectionStyle(QStringLiteral(R"({"ConnectionStyle": {
        "ConstructionColor": [160, 160, 164], "NormalColor": [130, 130, 136], "SelectedColor": "%1", "SelectedHaloColor": "%1", "HoveredColor": "%1",
        "LineWidth": 2.0, "ConstructionLineWidth": 2.0, "PointDiameter": 8.0, "UseDataDefinedColors": false}})")
                                                     .arg(accent));
}

QtNodes::NodeGraphicsObject* nodeAt(QtNodes::GraphicsView* view, QPoint viewportPoint) {
    for (QGraphicsItem* item = view->itemAt(viewportPoint); item; item = item->parentItem()) {
        if (auto* node = dynamic_cast<QtNodes::NodeGraphicsObject*>(item)) return node;
    }
    return nullptr;
}
}  // namespace

MaterialEditorViewPanel::MaterialEditorViewPanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo, const QString& title,
                                                 QWidget* parent)
    : ViewPanel(stageManager, std::move(stage), title, parent), m_undo(std::move(undo)) {
    applyTheme();

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    addPanelControls(toolbarLayout);
    m_pin = new QToolButton(toolbar);
    m_pin->setObjectName(QStringLiteral("materialEditorPin"));
    m_pin->setText(tr("Pin"));
    m_pin->setCheckable(true);
    m_pin->setToolTip(tr("Stay on this material when the selection changes"));
    toolbarLayout->addWidget(m_pin);
    m_breadcrumb = new QHBoxLayout;
    m_breadcrumb->setContentsMargins(6, 0, 0, 0);
    m_breadcrumb->setSpacing(2);
    toolbarLayout->addLayout(m_breadcrumb);
    toolbarLayout->addStretch(1);

    m_stack = new QStackedWidget(container);
    m_view = new QtNodes::GraphicsView(m_stack);
    m_view->setScaleRange(kSmallestToFrame, 2.0);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);
    m_empty = new QLabel(tr("Select a Material, or a prim with one bound."), m_stack);
    m_empty->setAlignment(Qt::AlignCenter);
    m_stack->addWidget(m_view);
    m_stack->addWidget(m_empty);
    layout->addWidget(toolbar);
    layout->addWidget(m_stack, 1);
    setContentWidget(container);

    auto* frame = new QShortcut(QKeySequence(Qt::Key_F), m_view);
    frame->setContext(Qt::WidgetWithChildrenShortcut);
    connect(frame, &QShortcut::activated, this, [this]() { fitGraph(kSmallestToFrame); });

    m_selectionChanged = this->stageManager()->selectionChanged.connect([this](StageRefPtr changed, std::vector<SdfPath> paths) {
        if (changed == currentStage()) selectionFromStage(paths);
    });

    refreshStageCombo();
    attachStage();
}

MaterialEditorViewPanel::~MaterialEditorViewPanel() {
    m_view->setScene(nullptr);
    m_scene.reset();
    m_model.reset();
}

usdcc::ui::ViewPanel* MaterialEditorViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new MaterialEditorViewPanel(stageManager(), currentStage(), m_undo, windowTitle(), parent);
    copy->showLevel(m_level);
    copy->setPinned(isPinned());
    return copy;
}

void MaterialEditorViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(tr("Frame All"), this, [this]() { fitGraph(kSmallestToFrame); });
}

void MaterialEditorViewPanel::onStageChanged(const StageRefPtr&) { attachStage(); }

void MaterialEditorViewPanel::attachStage() {
    m_view->setScene(nullptr);
    m_scene.reset();
    m_model.reset();
    m_primsMoved = {};
    m_level = SdfPath();
    const StageRefPtr& stage = currentStage();
    if (stage) {
        m_model = std::make_unique<UsdShadeGraphModel>(stage, m_undo);
        m_scene = std::make_unique<QtNodes::BasicGraphicsScene>(*m_model);
        m_scene->setNodeGeometry(std::make_unique<ShaderNodeGeometry>(*m_model));
        m_scene->setNodePainter(std::make_unique<ShaderNodePainter>());
        m_view->setScene(m_scene.get());
        // QtNodes' own Undo, Redo, Delete, Cut, Copy and Paste work on a QUndoStack of its
        // own that saves its model as JSON. The stage is the only data here, and Edit > Undo
        // is its undo, so none of them can be left to catch those keys.
        for (QAction* action : m_view->actions()) m_view->removeAction(action);

        // Press and release are the only signal of a drag: its moves come as position changes.
        connect(m_scene.get(), &QtNodes::BasicGraphicsScene::nodeClicked, m_model.get(), &UsdShadeGraphModel::endDrag);
        connect(m_scene.get(), &QGraphicsScene::selectionChanged, this, [this]() { selectionToStage(); });
        // Not from inside the node's own event: entering replaces it.
        connect(m_scene.get(), &QtNodes::BasicGraphicsScene::nodeDoubleClicked, this, [this](QtNodes::NodeId id) {
            const ShadeGraphNode* node = m_model->shadeNode(id);
            if (!node || node->kind != ShadeNodeKind::NodeGraph) return;
            QTimer::singleShot(0, this, [this, path = node->path]() { showLevel(path); });
        });
        // The scene's own slot for this came first, so the items are new here.
        connect(m_model.get(), &UsdShadeGraphModel::aboutToReset, this, [this]() { m_syncingSelection = true; });
        connect(m_model.get(), &UsdShadeGraphModel::modelReset, this, [this]() {
            m_syncingSelection = false;
            showSelectionInGraph();
        });
        connect(m_model.get(), &UsdShadeGraphModel::levelLost, this, [this]() {
            // After the move's own signal, which knows where it went.
            QTimer::singleShot(0, this, [this]() {
                if (!currentStage() || m_level.IsEmpty() || currentStage()->usdStage()->GetPrimAtPath(m_level)) return;
                pickLevel();
            });
        });
        m_primsMoved = stage->primsMoved.connect([this](const PrimMoves& moves) {
            const SdfPath moved = pathAfterMoves(m_level, moves);
            if (moved != m_level) showLevel(moved);
        });
    }
    pickLevel();
}

void MaterialEditorViewPanel::pickLevel() {
    SdfPath found;
    const StageRefPtr& stage = currentStage();
    if (stage) {
        const UsdStageRefPtr usd = stage->usdStage();
        for (const SdfPath& path : stageManager()->selectedPaths(stage)) {
            if (const UsdPrim level = shadeLevelFor(usd->GetPrimAtPath(path))) {
                found = level.GetPath();
                break;
            }
        }
        if (found.IsEmpty()) {
            for (const UsdPrim& prim : usd->Traverse()) {
                if (prim.IsA<UsdShadeMaterial>()) {
                    found = prim.GetPath();
                    break;
                }
            }
        }
    }
    showLevel(found);
}

void MaterialEditorViewPanel::showLevel(const SdfPath& level) {
    const bool changed = level != m_level;
    m_level = level;
    if (m_model) m_model->setLevel(level);
    m_stack->setCurrentWidget(level.IsEmpty() ? static_cast<QWidget*>(m_empty) : m_view);
    rebuildBreadcrumb();
    if (changed) fitGraph(kSmallestToOpenAt);
    showSelectionInGraph();
}

void MaterialEditorViewPanel::setPinned(bool pinned) { m_pin->setChecked(pinned); }
bool MaterialEditorViewPanel::isPinned() const { return m_pin->isChecked(); }
bool MaterialEditorViewPanel::isEmpty() const { return m_stack->currentWidget() == m_empty; }

QStringList MaterialEditorViewPanel::breadcrumb() const {
    QStringList names;
    if (!currentStage() || m_level.IsEmpty()) return names;
    for (const UsdPrim& prim : shadeLevelChain(currentStage()->usdStage()->GetPrimAtPath(m_level))) {
        names << QString::fromStdString(prim.GetName().GetString());
    }
    return names;
}

void MaterialEditorViewPanel::rebuildBreadcrumb() {
    while (QLayoutItem* item = m_breadcrumb->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            // It may be the button being clicked.
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    if (!currentStage() || m_level.IsEmpty()) return;
    const std::vector<UsdPrim> chain = shadeLevelChain(currentStage()->usdStage()->GetPrimAtPath(m_level));
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i > 0) m_breadcrumb->addWidget(new QLabel(QStringLiteral("›"), m_breadcrumb->parentWidget()));
        auto* button = new QToolButton(m_breadcrumb->parentWidget());
        button->setObjectName(QStringLiteral("materialEditorCrumb"));
        button->setText(QString::fromStdString(chain[i].GetName().GetString()));
        button->setAutoRaise(true);
        button->setToolTip(QString::fromStdString(chain[i].GetPath().GetString()));
        button->setEnabled(i + 1 < chain.size());
        const SdfPath path = chain[i].GetPath();
        connect(button, &QToolButton::clicked, this, [this, path]() { showLevel(path); });
        m_breadcrumb->addWidget(button);
    }
}

void MaterialEditorViewPanel::fitGraph(double smallest) {
    if (!m_scene) return;
    const QRectF all = m_scene->itemsBoundingRect();
    if (all.isEmpty()) return;
    m_view->fitInView(all.adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    // A short panel mustn't make it unreadable, and a small graph needs no enlarging.
    const double scale = m_view->transform().m11();
    const double wanted = qBound(smallest, scale, 1.0);
    if (wanted != scale) m_view->setTransform(QTransform::fromScale(wanted, wanted));
    m_view->centerOn(all.center());
}

void MaterialEditorViewPanel::selectionFromStage(const std::vector<SdfPath>& paths) {
    if (m_syncingSelection) return;
    if (!isPinned()) {
        const UsdStageRefPtr usd = currentStage()->usdStage();
        SdfPath target;
        bool alreadyShown = false;
        for (const SdfPath& path : paths) {
            const UsdPrim prim = usd->GetPrimAtPath(path);
            const UsdPrim level = shadeLevelFor(prim);
            if (!level) continue;
            // It's here, or it's this level or one around it, which entering doesn't undo.
            if (level.GetPath() == m_level || (!m_level.IsEmpty() && m_level.HasPrefix(path))) {
                alreadyShown = true;
                break;
            }
            if (target.IsEmpty()) target = level.GetPath();
        }
        if (!alreadyShown && !target.IsEmpty()) {
            showLevel(target);
            return;  // which shows the selection too
        }
    }
    showSelectionInGraph();
}

void MaterialEditorViewPanel::showSelectionInGraph() {
    if (!m_scene || !m_model || !currentStage() || m_syncingSelection) return;
    m_syncingSelection = true;
    m_scene->clearSelection();
    QtNodes::NodeGraphicsObject* last = nullptr;
    for (const SdfPath& path : stageManager()->selectedPaths(currentStage())) {
        for (QtNodes::NodeId id : m_model->nodesFor(path)) {
            if (auto* node = m_scene->nodeGraphicsObject(id)) {
                node->setSelected(true);
                last = node;
            }
        }
    }
    if (last) m_view->ensureVisible(last);
    m_syncingSelection = false;
}

void MaterialEditorViewPanel::selectionToStage() {
    if (m_syncingSelection || !m_scene || !m_model) return;
    std::vector<SdfPath> paths;
    for (QtNodes::NodeGraphicsObject* node : m_scene->selectedNodes()) {
        const ShadeGraphNode* shade = m_model->shadeNode(node->nodeId());
        if (shade && std::find(paths.begin(), paths.end(), shade->path) == paths.end()) paths.push_back(shade->path);
    }
    m_syncingSelection = true;
    stageManager()->setSelectedPaths(currentStage(), paths);
    m_syncingSelection = false;
}

bool MaterialEditorViewPanel::pressOnPage(QPoint viewportPoint) {
    if (!m_scene || !m_model) return false;
    auto* node = nodeAt(m_view, viewportPoint);
    if (!node) return false;
    const auto& geometry = static_cast<const ShaderNodeGeometry&>(m_scene->nodeGeometry());
    const std::optional<std::string> page = geometry.pageAt(node->nodeId(), node->mapFromScene(m_view->mapToScene(viewportPoint)));
    if (!page) return false;
    // Not from inside the node's own event: opening a page replaces it.
    QTimer::singleShot(0, this, [this, id = node->nodeId(), name = *page]() {
        if (m_model) m_model->togglePage(id, name);
    });
    return true;
}

bool MaterialEditorViewPanel::eventFilter(QObject* watched, QEvent* event) {
    // Each time it's shown QtNodes' view fits the graph itself, however small that makes it.
    // After it, and after the dock has given it its size.
    if (watched == m_view && event->type() == QEvent::Show) {
        QTimer::singleShot(0, this, [this]() { fitGraph(kSmallestToOpenAt); });
    }
    if (watched == m_view->viewport() && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && pressOnPage(mouse->position().toPoint())) return true;
    }
    return ViewPanel::eventFilter(watched, event);
}

}  // namespace usdcc::usd
