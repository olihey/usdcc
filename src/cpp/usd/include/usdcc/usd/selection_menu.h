#pragma once

#include "usdcc/core/export.h"

#include <QMenu>

namespace usdcc::usd {

class StageManager;

// Edit > Selection (docs/SELECTION_MENU.md): selects prims of the current stage. None,
// All, Invert; By Type, every prim type grouped as the Create menu groups them; By Schema,
// every applied API schema grouped by where it comes from; By Name..., a dialog. Each
// entry of By Type and By Schema shows how many prims have it, and is greyed with none.
// Rebuilt each time it opens. With no stage open, everything is greyed.
class USDCC_USD_UI_API SelectionMenu : public QMenu {
    Q_OBJECT

public:
    SelectionMenu(StageManager* stageManager, QWidget* parent);

    // Fills the menu now, as opening it does.
    void rebuild();

private:
    StageManager* m_stageManager;
};

}  // namespace usdcc::usd
