#pragma once

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
// each offering a new panel of every registered ViewPanel type showing that
// stage, then "Close Stage..." (closeStageInteractively()). Rebuilt each time
// it opens; shows a disabled "No stages open" entry while there are none.
//
// `stageManager`, `panelTypes` and `dockManager` must outlive the menu;
// `window` parents the new panels and the close dialog.
class StagesMenu : public QMenu {
    Q_OBJECT

public:
    StagesMenu(StageManager* stageManager, const usdcc::ui::ViewPanelRegistry* panelTypes,
               ads::CDockManager* dockManager, QWidget* window);

private:
    void rebuild();

    StageManager* m_stageManager;
    const usdcc::ui::ViewPanelRegistry* m_panelTypes;
    ads::CDockManager* m_dockManager;
    QWidget* m_window;
};

}  // namespace usdcc::usd
