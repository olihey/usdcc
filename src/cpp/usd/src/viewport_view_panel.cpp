#include "usdcc/usd/viewport_view_panel.h"

#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QActionGroup>
#include <QHBoxLayout>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace usdcc::usd {

ViewportViewPanel::ViewportViewPanel(StageManager* stageManager, StageRefPtr stage,
                                      std::shared_ptr<usdcc::tools::ToolSet> toolSet,
                                      std::shared_ptr<UndoManager> undoManager, const QString& title,
                                      QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent),
      m_toolSet(std::move(toolSet)),
      m_undoManager(std::move(undoManager)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);

    addPanelControls(toolbarLayout);

    // Embedded via createWindowContainer() (QWindow, not QOpenGLWidget) —
    // see hydra_viewport_window.h for why.
    m_viewport = new HydraViewportWindow();
    m_viewport->setToolSet(m_toolSet);
    QWidget* viewportContainer = QWidget::createWindowContainer(m_viewport, container);

    layout->addWidget(toolbar);
    layout->addWidget(viewportContainer, 1);

    setContentWidget(container);

    connect(m_viewport, &HydraViewportWindow::selectionRequested, this, &ViewportViewPanel::onSelectionRequested);
    // A click in the viewport focuses this panel, which makes its stage the
    // current one (CurrentStageTracker): ADS doesn't see the native window's
    // focus. Only the highlight moves; the viewport keeps the keyboard.
    connect(m_viewport, &HydraViewportWindow::pressed, this, [this]() {
        if (dockManager()) dockManager()->setDockWidgetFocused(this);
    });
    // One undo step per gizmo drag. The stage is remembered at the start so the
    // session is closed on the same stage it was opened on.
    connect(m_viewport, &HydraViewportWindow::toolEditStarted, this, [this](const QString& description) {
        if (m_undoManager && currentStage()) {
            m_toolEditStage = currentStage();
            m_undoManager->beginEdit(m_toolEditStage, description);
        }
    });
    connect(m_viewport, &HydraViewportWindow::toolEditFinished, this, [this]() {
        if (m_undoManager && m_toolEditStage) {
            m_undoManager->endEdit(m_toolEditStage);
            m_toolEditStage.reset();
        }
    });

    // NB: uses the `stageManager` constructor parameter directly, not the
    // stageManager() accessor — the parameter shadows it in this scope.
    m_selectionChangedConnection = stageManager->selectionChanged.connect(
        [this](StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
            onStageSelectionChanged(stage, std::move(paths));
        });

    refreshStageCombo();
    // The combo's initial selection can't trigger this itself: ViewPanel
    // already holds the initial stage, so setStage() sees it as unchanged.
    onStageChanged(currentStage());
}

void ViewportViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_viewport->setStage(stage ? stage->usdStage() : PXR_NS::UsdStageRefPtr());
    m_viewport->setSelectedPaths(stageManager()->selectedPaths(stage));
    if (!stage) {
        m_stageChangedConnection = {};
        m_targetConnection = {};
        m_viewport->setTargetXformOp(PXR_NS::TfToken());
        m_viewport->requestUpdate();
        return;
    }
    // The viewport only draws when asked: redraw after every edit, wherever it
    // came from (undo/redo, another panel, a script) — requestUpdate() folds a
    // burst of notices into one frame.
    m_stageChangedConnection = stage->changed.connect([this](const StageChanges&) { m_viewport->requestUpdate(); });
    m_targetConnection = stage->targetXformOpChanged.connect([this]() { syncTarget(); });
    syncTarget();
}

void ViewportViewPanel::syncTarget() {
    const std::vector<PXR_NS::SdfPath> selection = stageManager()->selectedPaths(currentStage());
    m_viewport->setTargetXformOp(selection.empty() || !currentStage() ? PXR_NS::TfToken()
                                                   : currentStage()->targetXformOpFor(selection.front()));
}

void ViewportViewPanel::onSelectionRequested(std::vector<PXR_NS::SdfPath> paths) {
    stageManager()->setSelectedPaths(currentStage(), std::move(paths));
}

void ViewportViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage == currentStage()) {
        m_viewport->setSelectedPaths(std::move(paths));
        syncTarget();
    }
}

usdcc::ui::ViewPanel* ViewportViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new ViewportViewPanel(stageManager(), currentStage(), m_toolSet, m_undoManager, windowTitle(), parent);

    // Renderer selection and camera state: the duplicate's HydraViewportWindow
    // has no GL context yet (it's created lazily on first expose), so it has
    // no renderers to pick from until rendererPluginsChanged() fires. Apply
    // both once that happens rather than immediately.
    const PXR_NS::TfToken rendererPluginId = m_viewport->currentRendererPlugin();
    const HydraViewportWindow::CameraState cameraState = m_viewport->cameraState();
    connect(copy->m_viewport, &HydraViewportWindow::rendererPluginsChanged, copy,
            [copy, rendererPluginId, cameraState]() {
                const std::vector<PXR_NS::TfToken> plugins = copy->m_viewport->rendererPlugins();
                if (std::find(plugins.begin(), plugins.end(), rendererPluginId) != plugins.end()) {
                    copy->m_viewport->setRendererPlugin(rendererPluginId);
                }
                copy->m_viewport->setCameraState(cameraState);
            });

    return copy;
}

void ViewportViewPanel::populateContextMenu(QMenu* menu) {
    usdcc::ui::ViewPanel::populateContextMenu(menu);
    menu->addAction(tr("Reset Camera"), this, [this]() { m_viewport->setCameraState({}); });
}

void ViewportViewPanel::populateOptionsMenu(QMenu* menu) {
    QMenu* renderers = menu->addMenu(tr("Renderer"));
    // Empty until the viewport has been shown once: its GL context, and with
    // it the Hydra engine that lists the plugins, is created on first expose.
    const std::vector<PXR_NS::TfToken> plugins = m_viewport->rendererPlugins();
    renderers->setEnabled(!plugins.empty());

    auto* group = new QActionGroup(renderers);  // exclusive: one renderer checked
    const PXR_NS::TfToken current = m_viewport->currentRendererPlugin();
    for (const PXR_NS::TfToken& pluginId : plugins) {
        QAction* action = renderers->addAction(m_viewport->rendererDisplayName(pluginId));
        action->setCheckable(true);
        action->setChecked(pluginId == current);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, pluginId]() { m_viewport->setRendererPlugin(pluginId); });
    }
}

}  // namespace usdcc::usd
