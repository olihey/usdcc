#include "usdcc/usd/viewport_view_panel.h"

#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace usdcc::usd {

ViewportViewPanel::ViewportViewPanel(StageManager* stageManager, StageRefPtr stage,
                                      std::shared_ptr<usdcc::tools::ToolSet> toolSet, const QString& title,
                                      QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_toolSet(std::move(toolSet)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(10, 4, 10, 4);

    m_rendererCombo = new QComboBox(toolbar);
    toolbarLayout->addWidget(new QLabel(tr("Stage:"), toolbar));
    toolbarLayout->addWidget(stageCombo(), 1);
    toolbarLayout->addWidget(new QLabel(tr("Renderer:"), toolbar));
    toolbarLayout->addWidget(m_rendererCombo, 1);

    // Embedded via createWindowContainer() (QWindow, not QOpenGLWidget) —
    // see hydra_viewport_window.h for why.
    m_viewport = new HydraViewportWindow();
    m_viewport->setToolSet(m_toolSet);
    QWidget* viewportContainer = QWidget::createWindowContainer(m_viewport, container);

    layout->addWidget(toolbar);
    layout->addWidget(viewportContainer, 1);

    setContentWidget(container);

    connect(m_rendererCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ViewportViewPanel::onRendererComboChanged);
    connect(m_viewport, &HydraViewportWindow::rendererPluginsChanged, this, &ViewportViewPanel::refreshRendererCombo);
    connect(m_viewport, &HydraViewportWindow::selectionRequested, this, &ViewportViewPanel::onSelectionRequested);

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
    m_viewport->setStage(stage->usdStage());
    m_viewport->setSelectedPaths(stageManager()->selectedPaths(stage));
}

void ViewportViewPanel::refreshRendererCombo() {
    m_rendererCombo->blockSignals(true);
    m_rendererCombo->clear();

    for (const auto& pluginId : m_viewport->rendererPlugins()) {
        m_rendererCombo->addItem(m_viewport->rendererDisplayName(pluginId), QString::fromUtf8(pluginId.GetText()));
    }

    const QString current = QString::fromUtf8(m_viewport->currentRendererPlugin().GetText());
    const int currentIndex = m_rendererCombo->findData(current);
    if (currentIndex >= 0) {
        m_rendererCombo->setCurrentIndex(currentIndex);
    }

    m_rendererCombo->blockSignals(false);
}

void ViewportViewPanel::onRendererComboChanged(int index) {
    if (index < 0) {
        return;
    }
    const QString pluginId = m_rendererCombo->itemData(index).toString();
    m_viewport->setRendererPlugin(PXR_NS::TfToken(pluginId.toStdString()));
}

void ViewportViewPanel::onSelectionRequested(std::vector<PXR_NS::SdfPath> paths) {
    stageManager()->setSelectedPaths(currentStage(), std::move(paths));
}

void ViewportViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage == currentStage()) {
        m_viewport->setSelectedPaths(std::move(paths));
    }
}

usdcc::ui::ViewPanel* ViewportViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new ViewportViewPanel(stageManager(), currentStage(), m_toolSet, windowTitle(), parent);

    // Renderer selection and camera state: the duplicate's HydraViewportWindow
    // has no GL context yet (it's created lazily on first expose), so its
    // renderer combo isn't populated until rendererPluginsChanged() fires.
    // Apply both once that happens rather than immediately.
    const PXR_NS::TfToken rendererPluginId = m_viewport->currentRendererPlugin();
    const HydraViewportWindow::CameraState cameraState = m_viewport->cameraState();
    connect(copy->m_viewport, &HydraViewportWindow::rendererPluginsChanged, copy,
            [copy, rendererPluginId, cameraState]() {
                if (!rendererPluginId.IsEmpty()) {
                    const int index = copy->m_rendererCombo->findData(QString::fromUtf8(rendererPluginId.GetText()));
                    if (index >= 0) {
                        copy->m_rendererCombo->setCurrentIndex(index);
                    }
                }
                copy->m_viewport->setCameraState(cameraState);
            });

    return copy;
}

void ViewportViewPanel::populateContextMenu(QMenu* menu) {
    usdcc::ui::ViewPanel::populateContextMenu(menu);
    menu->addAction(tr("Reset Camera"), this, [this]() { m_viewport->setCameraState({}); });
}

}  // namespace usdcc::usd
