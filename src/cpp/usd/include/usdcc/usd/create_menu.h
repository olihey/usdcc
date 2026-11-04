#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <QMenu>

#include <memory>

namespace usdcc::usd {

class StageManager;
class UndoManager;

// The menu bar's Create menu (docs/CREATE_MENU.md §2): a prim of any type USD knows, in
// the current stage, grouped by PrimCreationCatalog, with the Outliner's icons. Rebuilt
// each time it opens, for the selection and any plugin loaded since. Its first line
// says where a new prim will go. Disabled with no stage.
class PrimCreateMenu : public QMenu {
    Q_OBJECT

public:
    PrimCreateMenu(StageManager* stageManager, std::shared_ptr<UndoManager> undo, QWidget* parent);

    // Fills the menu now, as opening it does.
    void rebuild();

private:
    void create(const std::string& type);
    void updateEnabled();

    StageManager* m_stageManager;
    std::shared_ptr<UndoManager> m_undo;
    usdcc::core::Signal<StageRefPtr>::Connection m_currentStageChanged;
};

}  // namespace usdcc::usd
