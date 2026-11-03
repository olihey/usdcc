#pragma once

#include "usdcc/core/signal.h"
#include "usdcc/usd/stage.h"

#include <QString>
#include <QUndoGroup>

namespace usdcc::usd {

class StageManager;
class UndoManager;

// Edit > Undo / Redo for the current stage (docs/CURRENT_STAGE.md §3): a
// QUndoGroup whose active stack is the current stage's, following
// StageManager::currentStageChanged. Its createUndoAction() and
// createRedoAction() then name the step ("Undo Rename Chair") and are
// disabled when there is none. A closed stage's stack leaves the group when
// UndoManager drops it.
class CurrentStageUndoGroup : public QUndoGroup {
    Q_OBJECT

public:
    CurrentStageUndoGroup(UndoManager& undoManager, StageManager& stageManager, QObject* parent = nullptr);

private:
    void follow(const StageRefPtr& stage);

    UndoManager& m_undoManager;
    usdcc::core::Signal<StageRefPtr>::Connection m_currentStageConnection;
};

// The main window's title: "usdcc - <current stage>", the stage named as in
// the panels' Stage dropdowns (shortStageNames()); "usdcc" with none.
QString windowTitle(const StageManager& stageManager);

}  // namespace usdcc::usd
