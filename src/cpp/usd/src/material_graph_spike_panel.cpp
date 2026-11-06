#include "usdcc/usd/material_graph_spike_panel.h"

#include "usdcc/ui/theme.h"
#include "usdcc/usd/material_graph_spike_model.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsView>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/NodeStyle>

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QTimer>
#include <QVBoxLayout>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
// The app's dark theme, for what QtNodes draws itself. Its styles are process-wide.
void applyTheme() {
    const QString accent = usdcc::ui::theme::accent().name();
    QtNodes::GraphicsViewStyle::setStyle(QStringLiteral(R"({"GraphicsViewStyle": {
        "BackgroundColor": [28, 28, 30], "FineGridColor": [36, 36, 38], "CoarseGridColor": [46, 46, 49]}})"));
    QtNodes::NodeStyle::setNodeStyle(QStringLiteral(R"({"NodeStyle": {
        "NormalBoundaryColor": [70, 70, 74], "SelectedBoundaryColor": "%1",
        "GradientColor0": [50, 50, 54], "GradientColor1": [46, 46, 50], "GradientColor2": [44, 44, 48], "GradientColor3": [42, 42, 46],
        "ShadowColor": [10, 10, 10], "ShadowEnabled": false, "FontColor": [225, 225, 228], "FontColorFaded": [140, 140, 145],
        "ConnectionPointColor": [160, 160, 164], "FilledConnectionPointColor": "%1", "ErrorColor": [255, 95, 86], "WarningColor": [255, 189, 46],
        "ToolTipIconColor": "white", "PenWidth": 1.0, "HoveredPenWidth": 1.5, "ConnectionPointDiameter": 8.0, "Opacity": 1.0}})")
                                      .arg(accent));
    QtNodes::ConnectionStyle::setConnectionStyle(QStringLiteral(R"({"ConnectionStyle": {
        "ConstructionColor": [160, 160, 164], "NormalColor": [130, 130, 136], "SelectedColor": "%1", "SelectedHaloColor": "%1", "HoveredColor": "%1",
        "LineWidth": 2.0, "ConstructionLineWidth": 2.0, "PointDiameter": 8.0, "UseDataDefinedColors": false}})")
                                                     .arg(accent));
}
}  // namespace

MaterialGraphSpikePanel::MaterialGraphSpikePanel(StageManager* stageManager, StageRefPtr stage, std::shared_ptr<UndoManager> undo, const QString& title,
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
    toolbarLayout->addStretch(1);
    m_view = new QtNodes::GraphicsView(container);
    m_view->setScaleRange(0.2, 2.0);
    m_view->installEventFilter(this);
    layout->addWidget(toolbar);
    layout->addWidget(m_view, 1);
    setContentWidget(container);

    refreshStageCombo();
    pickMaterial();
}

MaterialGraphSpikePanel::~MaterialGraphSpikePanel() {
    m_view->setScene(nullptr);
    m_scene.reset();
    m_model.reset();
}

usdcc::ui::ViewPanel* MaterialGraphSpikePanel::duplicate(QWidget* parent) const {
    return new MaterialGraphSpikePanel(stageManager(), currentStage(), m_undo, windowTitle(), parent);
}

void MaterialGraphSpikePanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(tr("Show Selected Material"), this, [this]() { pickMaterial(); });
    menu->addAction(tr("Fit in View"), this, [this]() { fitGraph(); });
}

bool MaterialGraphSpikePanel::eventFilter(QObject* watched, QEvent* event) {
    // Each time it's shown QtNodes' view fits the graph itself, however small that makes it.
    // After it, and after the dock has given it its size.
    if (watched == m_view && event->type() == QEvent::Show) QTimer::singleShot(0, this, [this]() { fitGraph(); });
    return ViewPanel::eventFilter(watched, event);
}

void MaterialGraphSpikePanel::fitGraph() {
    if (!m_scene) return;
    const QRectF all = m_scene->itemsBoundingRect();
    m_view->fitInView(all.adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    // Fitting into a short panel would make it too small to read, and a small graph needs no enlarging.
    const double scale = m_view->transform().m11();
    const double legible = qBound(0.6, scale, 1.0);
    if (legible != scale) m_view->setTransform(QTransform::fromScale(legible, legible));
    m_view->centerOn(all.center());
}

void MaterialGraphSpikePanel::onStageChanged(const StageRefPtr&) { pickMaterial(); }

void MaterialGraphSpikePanel::pickMaterial() {
    SdfPath found;
    const StageRefPtr& stage = currentStage();
    if (stage) {
        for (const SdfPath& path : stageManager()->selectedPaths(stage)) {
            for (UsdPrim prim = stage->usdStage()->GetPrimAtPath(path); prim && !prim.IsPseudoRoot(); prim = prim.GetParent()) {
                if (prim.IsA<UsdShadeMaterial>()) {
                    found = prim.GetPath();
                    break;
                }
            }
            if (!found.IsEmpty()) break;
        }
        if (found.IsEmpty()) {
            for (const UsdPrim& prim : stage->usdStage()->Traverse()) {
                if (prim.IsA<UsdShadeMaterial>()) {
                    found = prim.GetPath();
                    break;
                }
            }
        }
    }
    showMaterial(found);
}

void MaterialGraphSpikePanel::showMaterial(const SdfPath& path) {
    m_material = path;
    m_view->setScene(nullptr);
    m_scene.reset();
    m_model.reset();
    if (!currentStage() || path.IsEmpty()) return;

    m_model = std::make_unique<UsdShadeSpikeModel>(currentStage(), path, m_undo);
    m_scene = std::make_unique<QtNodes::BasicGraphicsScene>(*m_model);
    m_view->setScene(m_scene.get());
    // QtNodes' own Undo, Redo, Delete, Cut, Copy and Paste work on a QUndoStack of its
    // own that saves its model as JSON. The stage is the only data here, and Edit > Undo
    // is its undo, so none of them can be left to catch those keys.
    for (QAction* action : m_view->actions()) m_view->removeAction(action);
    // Press and release are the only signal of a drag: its moves come as position changes.
    connect(m_scene.get(), &QtNodes::BasicGraphicsScene::nodeClicked, m_model.get(), &UsdShadeSpikeModel::endDrag);
    fitGraph();
}

}  // namespace usdcc::usd
