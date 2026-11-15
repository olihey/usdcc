#pragma once

#include "usdcc/core/export.h"
#include "usdcc/ui/workspace.h"

#include <QString>
#include <QStringList>

namespace ads {
class CDockAreaWidget;
class CDockManager;
}  // namespace ads

namespace usdcc::usd {
class StageManager;
}

namespace usdcc::ui {

class MainWindow;
class ViewPanel;
class ViewPanelRegistry;

// Puts a workspace on screen (docs/WORKSPACES.md §7), and reads the window's
// arrangement back as one (for Save Current Layout as Workspace).
namespace workspace {

struct USDCC_UI_API ApplyContext {
    MainWindow* window = nullptr;
    const ViewPanelRegistry* registry = nullptr;
    usdcc::usd::StageManager* stages = nullptr;
    // The view panels the app always has (Viewport, Outliner, Attributes): closed
    // when a workspace has no place for them, never deleted (sessions count on them).
    QStringList fixedPanels;
};

struct USDCC_UI_API ApplyResult {
    bool applied = false;
    // Panel types the workspace names that aren't registered (left out).
    QStringList missingTypes;
    // The panel that refused to close (ViewPanel::canClose()); nothing changed.
    QString refusedBy;
};

// Switches the window to `definition` (§7.1, §7.2): reuses the panels it can,
// makes the rest from the registry, closes the others, and lays them out. The
// viewport area gets one viewport per open stage, the current stage's in front
// and focused.
USDCC_UI_API ApplyResult apply(const Definition& definition, const ApplyContext& context);

// The window's dock layout as a workspace (§9, workspace_capture): splits in shares,
// the area with the most viewports as the viewport area, other viewports as ones
// following the current stage with their view; floating panels left out. With
// `includeSidePanels`, each side panel's state too.
USDCC_UI_API Definition capture(MainWindow* window, const QString& id, const QString& name,
                                bool includeSidePanels = true);

// Whether `panel` is a viewport (its registry type is "viewport").
USDCC_UI_API bool isViewport(const ViewPanel* panel);
// The viewport area (§7.3): the dock area holding the most open viewports that
// don't follow the current stage; null when there is none.
USDCC_UI_API ads::CDockAreaWidget* viewportArea(ads::CDockManager* dockManager);

}  // namespace workspace

}  // namespace usdcc::ui
