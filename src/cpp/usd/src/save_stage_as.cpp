#include "usdcc/usd/save_stage_as.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QDir>
#include <QJsonObject>

#include <vector>

namespace usdcc::usd {

StageRefPtr saveStageAsAndSwitch(StageManager& stageManager, const StageRefPtr& stage, const std::string& path,
                                 QString* error) {
    if (!stage) return {};
    const bool ownFile = PXR_NS::SdfLayer::Find(path) == stage->usdStage()->GetRootLayer();
    // Before writing: what the new stage takes over.
    QJsonObject state = session_stages::capture(stageManager, stage);
    if (!layer_actions::saveStageAs(stage, path, error)) return {};
    if (ownFile) return stage;

    state.remove(QStringLiteral("inMemory"));
    state.insert(QStringLiteral("file"), QString::fromStdString(path));
    const PXR_NS::UsdStageRefPtr usdStage = session_stages::open(state);
    if (!usdStage) {
        if (error) {
            *error = QObject::tr("%1 was written, but couldn't be opened.")
                         .arg(QDir::toNativeSeparators(QString::fromStdString(path)));
        }
        return {};
    }
    const bool wasCurrent = stageManager.currentStage() == stage;
    const StageRefPtr replacement = stageManager.adoptStage(usdStage);
    session_stages::apply(stageManager, replacement, state);

    // Closed (hidden) panels too: they still hold the stage.
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(widget);
        if (!panel || panel->currentStage() != stage) continue;
        const QJsonObject panelState = panel->saveSessionState();
        panel->setStage(replacement);
        panel->restoreSessionState(panelState);
    }
    if (wasCurrent) stageManager.setCurrentStage(replacement);
    // Its root's edits are in the new file now, and its other layers are the new stage's.
    stageManager.closeStage(stage);
    return replacement;
}

}  // namespace usdcc::usd
