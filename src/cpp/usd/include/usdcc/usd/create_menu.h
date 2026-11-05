#pragma once

#include "usdcc/core/export.h"

#include "usdcc/core/signal.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/stage.h"

#include <QMenu>

#include <memory>

namespace usdcc::usd {

class StageManager;
class UndoManager;

// What both Create menus list, added to `menu`: a first line saying where a new prim goes
// (or why it can't), then every type. Choosing one creates it under `target.parent`, as one
// undo step, and selects it if there's a `stageManager`. With a `target.problem` every entry
// is disabled. `stage` may be null, and then there's no first line.
void addCreateEntries(QMenu* menu, StageManager* stageManager, const StageRefPtr& stage, UndoManager* undo,
                      const prim_actions::CreateTarget& target);

// The menu bar's Create menu (docs/CREATE_MENU.md §2): a prim of any type USD knows, in
// the current stage, grouped by PrimCreationCatalog, with the Outliner's icons. Rebuilt
// each time it opens, for the selection and any plugin loaded since. Its first line
// says where a new prim will go. With no stage it still opens, its entries disabled.
class USDCC_USD_UI_API PrimCreateMenu : public QMenu {
    Q_OBJECT

public:
    PrimCreateMenu(StageManager* stageManager, std::shared_ptr<UndoManager> undo, QWidget* parent);

    // Fills the menu now, as opening it does.
    void rebuild();

private:

    StageManager* m_stageManager;
    std::shared_ptr<UndoManager> m_undo;
};

}  // namespace usdcc::usd
