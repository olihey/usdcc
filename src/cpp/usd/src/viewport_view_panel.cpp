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

namespace usdcc::usd {

ViewportViewPanel::ViewportViewPanel(StageManager* stageManager, const QString& title, QWidget* parent)
    : usdcc::ui::ViewPanel(title, parent), m_stageManager(stageManager) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(4, 4, 4, 4);

    m_stageCombo = new QComboBox(toolbar);
    m_rendererCombo = new QComboBox(toolbar);
    toolbarLayout->addWidget(new QLabel(tr("Stage:"), toolbar));
    toolbarLayout->addWidget(m_stageCombo, 1);
    toolbarLayout->addWidget(new QLabel(tr("Renderer:"), toolbar));
    toolbarLayout->addWidget(m_rendererCombo, 1);

    // Embedded via createWindowContainer() (QWindow, not QOpenGLWidget) —
    // see hydra_viewport_window.h for why.
    m_viewport = new HydraViewportWindow();
    QWidget* viewportContainer = QWidget::createWindowContainer(m_viewport, container);

    layout->addWidget(toolbar);
    layout->addWidget(viewportContainer, 1);

    setContentWidget(container);

    connect(m_stageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ViewportViewPanel::onStageComboChanged);
    connect(m_rendererCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ViewportViewPanel::onRendererComboChanged);
    connect(m_viewport, &HydraViewportWindow::rendererPluginsChanged, this, &ViewportViewPanel::refreshRendererCombo);

    if (m_stageManager) {
        connect(m_stageManager, &StageManager::stageOpened, this, &ViewportViewPanel::refreshStageCombo);
        connect(m_stageManager, &StageManager::stageClosed, this, &ViewportViewPanel::refreshStageCombo);
    }

    refreshStageCombo();
}

void ViewportViewPanel::refreshStageCombo() {
    m_stageCombo->blockSignals(true);
    m_stageCombo->clear();

    if (m_stageManager) {
        for (const auto& stage : m_stageManager->stages()) {
            const QString identifier = QString::fromStdString(stage->GetRootLayer()->GetIdentifier());
            m_stageCombo->addItem(identifier, static_cast<qlonglong>(m_stageManager->stageCacheId(stage)));
        }
    }

    m_stageCombo->blockSignals(false);
    onStageComboChanged(m_stageCombo->currentIndex());
}

void ViewportViewPanel::onStageComboChanged(int index) {
    if (!m_stageManager || index < 0) {
        m_viewport->setStage(nullptr);
        return;
    }
    const auto cacheId = static_cast<long>(m_stageCombo->itemData(index).toLongLong());
    m_viewport->setStage(m_stageManager->findByCacheId(cacheId));
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

usdcc::ui::ViewPanel* ViewportViewPanel::duplicate(QWidget* parent) const {
    auto* copy = new ViewportViewPanel(m_stageManager, windowTitle(), parent);

    // Stage selection: refreshStageCombo() already ran synchronously inside
    // the constructor above, so the combo is already populated.
    const int stageIndex = m_stageCombo->currentIndex();
    if (stageIndex >= 0) {
        copy->m_stageCombo->setCurrentIndex(stageIndex);
    }

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
