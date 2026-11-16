#include "usdcc/usd/stages_menu.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/ui/workspace_controller.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QHash>
#include <QPointer>

#include <algorithm>
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

        // Queued, as Close Stage below: a message box or file dialog doesn't run inside the menu.
        QAction* save = stageMenu->addAction(tr("&Save Stage"));
        save->setObjectName(QStringLiteral("saveStage"));
        save->setStatusTip(tr("Save every layer of %1 that has unsaved changes").arg(name));
        save->setEnabled(stageHasUnsavedLayers(stage));
        connect(save, &QAction::triggered, this, [this, stage]() {
            QMetaObject::invokeMethod(this, [this, stage]() { saveStageInteractively(stage, m_window); }, Qt::QueuedConnection);
        });
        QAction* saveAs = stageMenu->addAction(tr("Save Stage &As..."));
        saveAs->setObjectName(QStringLiteral("saveStageAs"));
        saveAs->setStatusTip(tr("Save %1 to another file, and go on in that file").arg(name));
        connect(saveAs, &QAction::triggered, this, [this, stage]() {
            QMetaObject::invokeMethod(
                this, [this, stage]() { saveStageAsInteractively(*m_stageManager, stage, m_window); }, Qt::QueuedConnection);
        });
        stageMenu->addSeparator();

        if (m_workspaces) {
            QMenu* workspaces = stageMenu->addMenu(tr("&Workspace"));
            workspaces->setObjectName(QStringLiteral("workspaces"));
            workspaces->setToolTipsVisible(true);
            // With this stage current, so its viewport goes in front and the other
            // panels show it. A check on the active workspace only for the current stage.
            const bool isCurrent = m_stageManager->currentStage() == stage;
            m_workspaces->addWorkspaceActions(
                workspaces, [this, stage]() { m_stageManager->setCurrentStage(stage); }, isCurrent);
        }

        QMenu* views = stageMenu->addMenu(tr("&Views"));
        views->setObjectName(QStringLiteral("views"));
        for (const usdcc::ui::ViewPanelRegistry::Entry& type : m_panelTypes->entries()) {
            QMenu* typeMenu = views->addMenu(type.displayName);
            typeMenu->setObjectName(QStringLiteral("views.%1").arg(type.id));

            // The open panels of the type, each brought to the front showing this stage.
            // Each as "<title> (<its stage>)"; a second one alike as "<title> 2 (<its stage>)".
            QHash<QString, int> seen;
            for (usdcc::ui::ViewPanel* panel : openPanelsOfType(type.id)) {
                const StageRefPtr& shown = panel->currentStage();
                const auto it = std::find(stages.begin(), stages.end(), shown);
                const QString shownName = shown && it != stages.end()
                                              ? QString::fromStdString(names[it - stages.begin()])
                                              : tr("no stage");
                QString title = panel->windowTitle();
                if (const int count = ++seen[title + QLatin1Char('\n') + shownName]; count > 1) {
                    title += QStringLiteral(" %1").arg(count);
                }
                const QString label = QStringLiteral("%1 (%2)").arg(title, shownName);
                QAction* action = typeMenu->addAction(label);
                action->setObjectName(QStringLiteral("openPanel"));
                action->setStatusTip(shown == stage ? tr("Bring this %1 to the front").arg(type.displayName)
                                                    : tr("Bring this %1 to the front, showing %2").arg(type.displayName, name));
                connect(action, &QAction::triggered, this,
                        [this, guarded = QPointer<usdcc::ui::ViewPanel>(panel), stage]() { bringToFront(guarded, stage); });
            }
            if (!typeMenu->isEmpty()) typeMenu->addSeparator();

            QAction* create = typeMenu->addAction(tr("Create &New"));
            create->setObjectName(QStringLiteral("createNew"));
            create->setStatusTip(tr("Open a new %1 showing %2").arg(type.displayName, name));
            const QString id = type.id;
            connect(create, &QAction::triggered, this, [this, id, stage]() {
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

std::vector<usdcc::ui::ViewPanel*> StagesMenu::openPanelsOfType(const QString& id) const {
    std::vector<usdcc::ui::ViewPanel*> panels;
    for (ads::CDockWidget* dockWidget : m_dockManager->dockWidgetsMap()) {
        auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(dockWidget);
        if (panel && !panel->isClosed() && panel->property(usdcc::ui::ViewPanelRegistry::kTypeProperty).toString() == id) {
            panels.push_back(panel);
        }
    }
    return panels;
}

void StagesMenu::bringToFront(usdcc::ui::ViewPanel* panel, const StageRefPtr& stage) {
    // Gone or closed since the menu was built: nothing to bring.
    if (!panel || panel->isClosed()) return;
    if (panel->currentStage() != stage) {
        // As picking the stage in its dropdown: it stops following the current stage.
        panel->setFollowsCurrentStage(false);
        panel->setStage(stage);
    }
    panel->raise();  // its tab current, or its floating window raised
    m_dockManager->setDockWidgetFocused(panel);
}

void StagesMenu::makeCurrentEverywhere(const StageRefPtr& stage) {
    usdcc::usd::makeCurrentEverywhere(m_stageManager, m_dockManager, stage);
}

void makeCurrentEverywhere(StageManager* stageManager, ads::CDockManager* dockManager, const StageRefPtr& stage) {
    // The panels on screen: open, and the tab in front (a tab behind another,
    // or a closed panel, isn't visible).
    for (ads::CDockWidget* dockWidget : dockManager->dockWidgetsMap()) {
        auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(dockWidget);
        if (panel && !panel->isClosed() && panel->isVisible()) panel->setStage(stage);
    }
    stageManager->setCurrentStage(stage);
}

}  // namespace usdcc::usd
