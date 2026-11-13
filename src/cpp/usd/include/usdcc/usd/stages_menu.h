#pragma once

#include "usdcc/core/export.h"
#include "usdcc/usd/stage.h"

#include <QMenu>

namespace ads {
class CDockManager;
}

namespace usdcc::ui {
class ViewPanelRegistry;
}

namespace usdcc::usd {

class StageManager;

// The menu bar's Stages menu: one submenu per open stage (titled with its
// short name, see shortStageNames(); the full identifier is the tooltip),
// each with "Make Current" first (makeCurrentEverywhere()), "Save Stage" and
// "Save Stage As..." (saveStageInteractively(), saveStageAsInteractively()), then "Views", a
// new panel of every registered ViewPanel type showing that stage, then
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

private:
    void rebuild();

    StageManager* m_stageManager;
    const usdcc::ui::ViewPanelRegistry* m_panelTypes;
    ads::CDockManager* m_dockManager;
    QWidget* m_window;
};

}  // namespace usdcc::usd
