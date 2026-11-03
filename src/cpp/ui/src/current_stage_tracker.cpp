#include "usdcc/ui/current_stage_tracker.h"

#include "usdcc/ui/view_panel.h"

#include <DockManager.h>
#include <DockWidget.h>

namespace usdcc::ui {

CurrentStageTracker::CurrentStageTracker(ads::CDockManager* dockManager, usdcc::usd::StageManager* stageManager,
                                         QObject* parent)
    : QObject(parent), m_stageManager(stageManager) {
    connect(dockManager, &ads::CDockManager::focusedDockWidgetChanged, this,
            [this](ads::CDockWidget*, ads::CDockWidget* now) { follow(now); });
    // Runs before StageManager's own fallback to the first open stage, which
    // it then skips, since the closed stage is no longer current.
    m_stageClosedConnection = stageManager->stageClosed.connect([this](const usdcc::usd::StageRefPtr& closed) {
        if (m_stageManager->currentStage() != closed || !m_focused) return;
        const usdcc::usd::StageRefPtr& focusedStage = m_focused->currentStage();
        if (focusedStage && focusedStage != closed) m_stageManager->setCurrentStage(focusedStage);
    });
    follow(dockManager->focusedDockWidget());
}

void CurrentStageTracker::follow(ads::CDockWidget* focused) {
    auto* panel = qobject_cast<ViewPanel*>(focused);
    if (!panel || panel == m_focused) return;
    disconnect(m_focusedStageConnection);
    m_focused = panel;
    m_focusedStageConnection = connect(panel, &ViewPanel::stageChanged, this, &CurrentStageTracker::makeCurrent);
    makeCurrent(panel->currentStage());
}

void CurrentStageTracker::makeCurrent(const usdcc::usd::StageRefPtr& stage) {
    if (stage) m_stageManager->setCurrentStage(stage);  // a panel with no stage keeps the current one
}

}  // namespace usdcc::ui
