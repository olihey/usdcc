#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <QMenu>

#include <vector>

namespace ads {
class CDockManager;
}

namespace usdcc::ui {
class ViewPanel;
class ViewPanelRegistry;
class WorkspaceController;
}

namespace usdcc::usd {

class StageManager;

// Stages > a stage > Make Current: every view panel on screen (open, its tab in
// front) in `dockManager` switches to `stage`, which becomes the current stage.
// Open Stage with Options' Make current does the same (docs/WORKSPACES.md §8).
USDCC_USD_UI_API void makeCurrentEverywhere(StageManager* stageManager, ads::CDockManager* dockManager,
                                            const StageRefPtr& stage);

// The menu bar's Stages menu: one submenu per open stage (titled with its
// short name, see shortStageNames(); the full identifier is the tooltip),
// each with "Make Current" first (makeCurrentEverywhere()), "Save Stage" and
// "Save Stage As..." (saveStageInteractively(), saveStageAsInteractively()), then
// "Workspace" (with setWorkspaces(): switching to one with that stage current), "Views", a
// submenu per registered ViewPanel type listing the open panels of that type (picking one
// brings it to the front, switched to that stage if it showed another) and, below them,
// "Create New" (a new panel showing that stage), then
// "Close Stage..." (closeStageInteractively()). Rebuilt each time it opens;
// shows a disabled "No stages open" entry while there are none.
//
// `stageManager`, `panelTypes` and `dockManager` must outlive the menu;
// `window` parents the new panels and the close dialog.
class USDCC_USD_UI_API StagesMenu : public QMenu {
    Q_OBJECT

public:
    StagesMenu(StageManager* stageManager, const usdcc::ui::ViewPanelRegistry* panelTypes,
               ads::CDockManager* dockManager, QWidget* window);

    // Stages > a stage > Make Current: every view panel on screen (open, its
    // tab in front) switches to `stage`, which becomes the current stage.
    void makeCurrentEverywhere(const StageRefPtr& stage);

    // Gives each stage a Workspace submenu, above Views: the workspaces, each laid out
    // with that stage current. `workspaces` must outlive the menu.
    void setWorkspaces(usdcc::ui::WorkspaceController* workspaces) { m_workspaces = workspaces; }

private:
    void rebuild();
    // The panels of type `id` (ViewPanelRegistry::kTypeProperty) that aren't closed.
    std::vector<usdcc::ui::ViewPanel*> openPanelsOfType(const QString& id) const;
    // Views > a type > an open panel: shows `stage` in it, then brings it to the front.
    void bringToFront(usdcc::ui::ViewPanel* panel, const StageRefPtr& stage);

    StageManager* m_stageManager;
    const usdcc::ui::ViewPanelRegistry* m_panelTypes;
    ads::CDockManager* m_dockManager;
    QWidget* m_window;
    usdcc::ui::WorkspaceController* m_workspaces = nullptr;
};

}  // namespace usdcc::usd
