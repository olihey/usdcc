#include "usdcc/usd/save_stage_as.h"

#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/recent_files.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonObject>
#include <QMessageBox>
#include <QScopedValueRollback>

#include <vector>

namespace usdcc::usd {

namespace {
bool s_switching = false;
}  // namespace

bool isSwitchingStage() { return s_switching; }

bool stageHasUnsavedLayers(const StageRefPtr& stage) { return stage && !layer_actions::unsavedLayers(stage).empty(); }

bool saveStageInteractively(const StageRefPtr& stage, QWidget* parent) {
    if (!stage) return false;
    QString error;
    if (layer_actions::saveAll(stage, &error)) return true;
    QMessageBox::warning(parent, QObject::tr("Save Stage"), error);
    return false;
}

StageRefPtr saveStageAsInteractively(StageManager& stageManager, const StageRefPtr& stage, QWidget* parent) {
    if (!stage) return {};
    const std::string real = stage->usdStage()->GetRootLayer()->GetRealPath();
    QString path = QFileDialog::getSaveFileName(parent, QObject::tr("Save Stage As"),
                                                real.empty() ? QString() : QFileInfo(QString::fromStdString(real)).absolutePath(),
                                                QObject::tr("USD Text (*.usda);;USD Binary (*.usdc);;USD (*.usd)"));
    if (path.isEmpty()) return {};
    // USD needs an extension to know the format.
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".usda");
    QString error;
    const StageRefPtr saved = saveStageAsAndSwitch(stageManager, stage, path.toStdString(), &error);
    if (!saved) {
        QMessageBox::warning(parent, QObject::tr("Save Stage As"), error);
        return {};
    }
    addRecentFile(path);
    return saved;
}

StageRefPtr saveStageAsAndSwitch(StageManager& stageManager, const StageRefPtr& stage, const std::string& path,
                                 QString* error) {
    if (!stage) return {};
    const QScopedValueRollback<bool> switching(s_switching, true);
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
