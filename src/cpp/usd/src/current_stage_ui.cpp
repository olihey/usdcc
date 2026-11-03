#include "usdcc/usd/current_stage_ui.h"

#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"
#include "usdcc/usd/undo_manager.h"

#include <QUndoStack>

#include <algorithm>

namespace usdcc::usd {

CurrentStageUndoGroup::CurrentStageUndoGroup(UndoManager& undoManager, StageManager& stageManager, QObject* parent)
    : QUndoGroup(parent), m_undoManager(undoManager) {
    m_currentStageConnection =
        stageManager.currentStageChanged.connect([this](const StageRefPtr& stage) { follow(stage); });
    follow(stageManager.currentStage());
}

void CurrentStageUndoGroup::follow(const StageRefPtr& stage) {
    QUndoStack* stack = stage ? m_undoManager.stack(stage) : nullptr;
    if (stack && !stacks().contains(stack)) addStack(stack);
    setActiveStack(stack);
}

QString windowTitle(const StageManager& stageManager) {
    const StageRefPtr current = stageManager.currentStage();
    const std::vector<StageRefPtr>& stages = stageManager.stages();
    const auto it = std::find(stages.begin(), stages.end(), current);
    if (!current || it == stages.end()) return QStringLiteral("usdcc");
    const std::string name = shortStageNames(stages)[static_cast<std::size_t>(it - stages.begin())];
    return QStringLiteral("usdcc - %1").arg(QString::fromStdString(name));
}

}  // namespace usdcc::usd
