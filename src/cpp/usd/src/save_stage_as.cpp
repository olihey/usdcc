#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/asset_picker.h"
#include "usdcc/usd/validation_fix.h"
#include "usdcc/usd/asset_servers.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/toast.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/recent_files.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QVBoxLayout>

#include <string>
#include <vector>

namespace usdcc::usd {

namespace {
bool s_switching = false;
}  // namespace

bool isSwitchingStage() { return s_switching; }

bool stageHasUnsavedLayers(const StageRefPtr& stage) { return stage && !layer_actions::unsavedLayers(stage).empty(); }

bool saveStageInteractively(const StageRefPtr& stage, QWidget* parent) {
    if (!stage) return false;
    const PXR_NS::SdfLayerHandle root = stage->usdStage()->GetRootLayer();
    const std::string name = root->IsAnonymous() ? std::string("The in-memory stage") : root->GetDisplayName();
    const std::size_t count = layer_actions::unsavedLayers(stage).size();
    if (count == 0) {
        USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Info, "usdcc.stage", (usdcc::core::ToastRequest{.title = "Nothing to save"}),
                           "{} has no unsaved changes.", name);
        return true;
    }
    // Validation/beforeSave (docs/VALIDATION.md §3.3).
    if (!validation::confirmValidated(stage, validation::Gate::Save, parent)) return false;
    QString error;
    if (!layer_actions::saveAll(stage, &error)) {
        usdcc::core::ToastRequest failed = usdcc::core::ToastRequest::sticky();
        failed.kind = usdcc::core::ToastKind::Error;
        failed.title = "Save failed";
        USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Error, "usdcc.stage", failed, "Could not save {}: {}", name,
                           error.toStdString());
        return false;
    }
    USDCC_LOG_TOAST_AS(usdcc::core::LogLevel::Info, "usdcc.stage",
                       (usdcc::core::ToastRequest{.kind = usdcc::core::ToastKind::Success, .title = "Saved"}),
                       "Saved {} ({} {}).", name, count, count == 1 ? "layer" : "layers");
    return true;
}

StageRefPtr saveStageAsInteractively(StageManager& stageManager, const StageRefPtr& stage, QWidget* parent) {
    if (!stage) return {};
    // On disk or on a server (docs/ASSET_PICKER.md §5): where the stage is, to start with.
    const PXR_NS::SdfLayerHandle root = stage->usdStage()->GetRootLayer();
    const std::string real = root->GetRealPath();
    AssetPickerOptions options;
    options.title = QObject::tr("Save Stage As");
    options.filters = {QObject::tr("USD Text (*.usda)"), QObject::tr("USD Binary (*.usdc)"), QObject::tr("USD (*.usd)")};
    options.start = real.empty()                     ? QString()
                    : isLocalAssetPath(real)         ? QFileInfo(QString::fromStdString(real)).absolutePath()
                                                     : QString::fromStdString(displayAssetPath(real));
    options.context = stage->usdStage()->GetPathResolverContext();
    if (!validation::confirmValidated(stage, validation::Gate::Save, parent)) return {};
    QString path = pickSaveAsset(parent, options);
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

const char* const kSaveStageShortcutKey = "Shortcuts/file.saveStage";

void bindSaveStageShortcut(QAction* action) {
    usdcc::core::Settings::instance().define({.key = kSaveStageShortcutKey,
                                              .category = "Shortcuts",
                                              .group = "File",
                                              .label = "Save stage",
                                              .type = usdcc::core::SettingType::Shortcut,
                                              .defaultValue = std::string("Ctrl+S")});
    action->setShortcutContext(Qt::ApplicationShortcut);
    const auto apply = [action]() {
        const QKeySequence key = usdcc::ui::settingShortcut(kSaveStageShortcutKey);
        const QKeySequence wanted = focusHasOwnShortcut(QApplication::focusWidget(), key, action) ? QKeySequence() : key;
        if (action->shortcut() != wanted) action->setShortcut(wanted);
    };
    usdcc::ui::onSettingChanged(action, kSaveStageShortcutKey, apply);
    QObject::connect(qApp, &QApplication::focusChanged, action, [apply](QWidget*, QWidget*) { apply(); });
    apply();
}

bool focusHasOwnShortcut(const QWidget* focus, const QKeySequence& key, const QAction* ignore) {
    if (key.isEmpty()) return false;
    for (const QWidget* widget = focus; widget; widget = widget->parentWidget()) {
        // A widget-only shortcut counts on the focus widget itself, the others on any widget
        // the focus is in.
        const auto reaches = [widget, focus](Qt::ShortcutContext context) {
            return context != Qt::WidgetShortcut || widget == focus;
        };
        for (const QShortcut* shortcut : widget->findChildren<QShortcut*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (shortcut->isEnabled() && shortcut->keys().contains(key) && reaches(shortcut->context())) return true;
        }
        for (const QAction* action : widget->actions()) {
            if (action != ignore && action->isEnabled() && action->shortcuts().contains(key) &&
                reaches(action->shortcutContext())) {
                return true;
            }
        }
        // A shortcut of another window (a floating panel's main window) doesn't reach here.
        if (widget->isWindow()) break;
    }
    return false;
}

bool saveStageFromShortcut(StageManager& stageManager, QWidget* parent) {
    if (stageManager.stages().empty()) return false;
    if (stageManager.stages().size() == 1) {
        const StageRefPtr only = stageManager.stages().front();
        return saveStageInteractively(only, parent);
    }
    SaveStageDialog dialog(&stageManager, parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    return saveStageInteractively(dialog.stage(), parent);
}

SaveStageDialog::SaveStageDialog(StageManager* stageManager, QWidget* parent)
    : QDialog(parent), m_stageManager(stageManager) {
    setWindowTitle(tr("Save Stage"));
    usdcc::ui::scaled::minimumWidth(this, 360);

    auto* layout = new QVBoxLayout(this);
    auto* message = new QLabel(tr("Several stages are open. Which one should be saved?"), this);
    message->setWordWrap(true);
    layout->addWidget(message);

    m_choice = new QComboBox(this);
    m_choice->setObjectName(QStringLiteral("saveStageChoice"));
    const auto& stages = stageManager->stages();
    const std::vector<std::string> names = shortStageNames(stages);
    const StageRefPtr current = stageManager->currentStage();
    for (std::size_t i = 0; i < stages.size(); ++i) {
        m_choice->addItem(QString::fromStdString(names[i]), static_cast<qlonglong>(stages[i]->cacheId()));
        m_choice->setItemData(m_choice->count() - 1,
                              QString::fromStdString(stages[i]->usdStage()->GetRootLayer()->GetIdentifier()),
                              Qt::ToolTipRole);
        if (stages[i] == current) m_choice->setCurrentIndex(m_choice->count() - 1);
    }
    layout->addWidget(m_choice);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

StageRefPtr SaveStageDialog::stage() const {
    const QVariant cacheId = m_choice->currentData();
    return cacheId.isValid() ? m_stageManager->findByCacheId(static_cast<long>(cacheId.toLongLong())) : StageRefPtr();
}

}  // namespace usdcc::usd
