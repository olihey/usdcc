#include "usdcc/usd/stages_menu.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>

#include <string>
#include <vector>

namespace usdcc::usd {

StagesMenu::StagesMenu(StageManager* stageManager, const usdcc::ui::ViewPanelRegistry* panelTypes,
                       ads::CDockManager* dockManager, QWidget* window)
    : QMenu(tr("&Stages"), window),
      m_stageManager(stageManager),
      m_panelTypes(panelTypes),
      m_dockManager(dockManager),
      m_window(window) {
    setToolTipsVisible(true);
    connect(this, &QMenu::aboutToShow, this, &StagesMenu::rebuild);
}

void StagesMenu::rebuild() {
    // clear() deletes only the actions; the submenus addMenu() made are this
    // menu's children and would pile up.
    for (QMenu* stageMenu : findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) {
        stageMenu->deleteLater();
    }
    clear();
    const auto& stages = m_stageManager->stages();
    if (stages.empty()) {
        addAction(tr("No stages open"))->setEnabled(false);
        return;
    }

    const std::vector<std::string> names = shortStageNames(stages);
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const StageRefPtr stage = stages[i];
        const QString name = QString::fromStdString(names[i]);
        QMenu* stageMenu = addMenu(name);
        stageMenu->menuAction()->setToolTip(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetIdentifier()));

        QAction* makeCurrent = stageMenu->addAction(tr("Make &Current"));
        makeCurrent->setObjectName(QStringLiteral("makeCurrent"));
        makeCurrent->setStatusTip(tr("Show %1 in every panel on screen, and make it the current stage").arg(name));
        connect(makeCurrent, &QAction::triggered, this, [this, stage]() { makeCurrentEverywhere(stage); });
        stageMenu->addSeparator();

        QMenu* views = stageMenu->addMenu(tr("&Views"));
        views->setObjectName(QStringLiteral("views"));
        for (const usdcc::ui::ViewPanelRegistry::Entry& type : m_panelTypes->entries()) {
            QAction* action = views->addAction(type.displayName);
            action->setStatusTip(tr("Open a new %1 showing %2").arg(type.displayName, name));
            const QString id = type.id;
            connect(action, &QAction::triggered, this, [this, id, stage]() {
                m_panelTypes->open(id, stage, m_dockManager, m_window);
            });
        }

        stageMenu->addSeparator();
        QAction* close = stageMenu->addAction(tr("&Close Stage..."));
        connect(close, &QAction::triggered, this, [this, stage]() {
            // Queued: the dialog must not run from inside the menu's own event
            // handling, and the stage is held by value until it does.
            QMetaObject::invokeMethod(
                this, [this, stage]() { closeStageInteractively(m_stageManager, stage, m_window); },
                Qt::QueuedConnection);
        });
    }
}

void StagesMenu::makeCurrentEverywhere(const StageRefPtr& stage) {
    // The panels on screen: open, and the tab in front (a tab behind another,
    // or a closed panel, isn't visible).
    for (ads::CDockWidget* dockWidget : m_dockManager->dockWidgetsMap()) {
        auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(dockWidget);
        if (panel && !panel->isClosed() && panel->isVisible()) panel->setStage(stage);
    }
    m_stageManager->setCurrentStage(stage);
}

}  // namespace usdcc::usd
