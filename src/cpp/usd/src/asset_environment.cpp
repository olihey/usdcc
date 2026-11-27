#include "usdcc/usd/asset_environment.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/usd/asset_servers_editor.h"
#include "usdcc/usd/asset_settings.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/assets/activity.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/sources.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/toast_manager.h"

#include <pxr/usd/sdf/layer.h>

#include <QApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStyle>
#include <QPointer>
#include <QPushButton>
#include <QThread>

#include <algorithm>
#include <mutex>
#include <set>
#include <tuple>
#include <exception>
#include <memory>

namespace usdcc::usd {

namespace {

std::function<QString(const StageRefPtr&)>& reopenGuard() {
    static std::function<QString(const StageRefPtr&)> guard;
    return guard;
}

QString whyNotReopen(const StageRefPtr& stage) { return reopenGuard() ? reopenGuard()(stage) : QString(); }

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString nameOf(const StageRefPtr& stage) { return qs(stage->usdStage()->GetRootLayer()->GetDisplayName()); }

std::string rootOf(const StageRefPtr& stage) { return stage->usdStage()->GetRootLayer()->GetIdentifier(); }

QString environmentText(const std::string& environment) {
    return environment.empty() ? QObject::tr("(none)") : qs(environment);
}

}  // namespace

void setReopenGuard(std::function<QString(const StageRefPtr&)> guard) { reopenGuard() = std::move(guard); }

void installAssetEnvironmentOffer(usdcc::ui::MainWindow* window, StageManager* stageManager) {
    QPointer<usdcc::ui::MainWindow> target(window);
    usdcc::assets::setUnknownNameCallback([target, stageManager](const std::string& scheme, const std::string& name,
                                                                 const std::string& environment) {
        static std::mutex mutex;
        static std::set<std::tuple<std::string, std::string, std::string>> told;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!told.insert({scheme, name, environment}).second) return;
        }
        // From a resolver's thread: the question waits for the UI's.
        QMetaObject::invokeMethod(
            qApp,
            [target, stageManager, scheme, name, environment]() {
                // With nothing configured there's nothing to offer; the Log says what didn't resolve.
                // Servers alone are a configuration (docs/REMOTE_ASSETS.md §4.1).
                const usdcc::assets::Config config = usdcc::assets::defaultConfig();
                if (!target || (config.environments.empty() && config.endpoints.empty())) return;
                std::vector<std::string> offered = usdcc::assets::offeredEnvironments(config, scheme, name);
                offered.erase(std::remove(offered.begin(), offered.end(), environment), offered.end());
                // The stage it's about: the current one, if it's in that environment, else one that is.
                StageRefPtr stage = stageManager->currentStage();
                if (!stage || stageAssetEnvironment(stage->usdStage()) != environment) {
                    stage.reset();
                    for (const StageRefPtr& open : stageManager->stages()) {
                        if (stageAssetEnvironment(open->usdStage()) == environment) stage = open;
                    }
                }
                const QString path = qs(scheme + "://" + name);
                usdcc::core::ToastRequest request = usdcc::core::ToastRequest::sticky();
                request.kind = usdcc::core::ToastKind::Warning;
                QList<usdcc::ui::ToastAction> actions;
                if (offered.empty()) {
                    request.title = QObject::tr("%1 isn't configured").arg(path).toStdString();
                    request.body = (environment.empty()
                                        ? QObject::tr("There is no asset server named %1.").arg(qs(name))
                                        : QObject::tr("The asset environment %1 has no server named %2, and nor has any other.")
                                              .arg(environmentText(environment), qs(name)))
                                       .toStdString();
                    actions.append({QObject::tr("Asset Servers..."), [target]() { showAssetServersEditor(target); }});
                } else {
                    request.title = QObject::tr("%1 is in another environment").arg(path).toStdString();
                    QStringList names;
                    for (const std::string& each : offered) names << qs(each);
                    request.body = (environment.empty()
                                        ? QObject::tr("There is no server named %1. %2 has one.")
                                              .arg(qs(name), names.join(QStringLiteral(", ")))
                                        : QObject::tr("%1 has no server named %2. %3 has.")
                                              .arg(environmentText(environment), qs(name), names.join(QStringLiteral(", "))))
                                       .toStdString();
                    // Offered, never switched silently: switching contacts that source's servers.
                    for (size_t i = 0; i < offered.size() && i < 3 && stage; ++i) {
                        const std::string chosen = offered[i];
                        actions.append({QObject::tr("Reopen in %1").arg(qs(chosen)), [target, stageManager, stage, chosen]() {
                                            QString error;
                                            if (reopenInEnvironment(*stageManager, stage, chosen, target, &error)) {
                                                // One question per project (docs/ASSET_SOURCES.md §4).
                                                rememberFolderEnvironment(folderOf(rootOf(stage)), chosen);
                                                assetConfigChanged()();  // the status bar says so
                                            } else if (!error.isEmpty()) {
                                                QMessageBox::warning(target, QObject::tr("Asset Environment"), error);
                                            }
                                        }});
                    }
                }
                target->toasts()->show(request, actions);
            },
            Qt::QueuedConnection);
    });
}

std::string wantedEnvironment(const StageRefPtr& stage) {
    return stage->pendingAssetEnvironment().empty() ? stageAssetEnvironment(stage->usdStage())
                                                    : stage->pendingAssetEnvironment();
}

StageRefPtr reopenInEnvironment(StageManager& stageManager, const StageRefPtr& stage, const std::string& environment,
                                QWidget* parent, QString* error) {
    const auto fail = [error](const QString& why) -> StageRefPtr {
        if (error) *error = why;
        return {};
    };
    if (!stage) return fail(QObject::tr("There is no stage."));
    if (!hasAssetEnvironment(environment)) {
        return fail(QObject::tr("The configuration has no environment '%1'.").arg(qs(environment)));
    }
    if (const QString why = whyNotReopen(stage); !why.isEmpty()) return fail(why);

    QJsonObject state = session_stages::capture(stageManager, stage);
    state.insert(QStringLiteral("assetEnvironment"), qs(environment));
    state.remove(QStringLiteral("pendingAssetEnvironment"));

    // On a worker, as servers may be slow; input waits behind the dialog meanwhile. `stage`
    // stays until the new one is open, so layers both use keep their unsaved edits.
    struct Result {
        PXR_NS::UsdStageRefPtr stage;
        std::vector<std::string> errors;
    };
    const auto result = std::make_shared<Result>();
    {
        QProgressDialog progress(QObject::tr("Opening %1 in %2...").arg(nameOf(stage), qs(environment)), QString(), 0, 0,
                                 parent);
        progress.setWindowTitle(QObject::tr("Asset Environment"));
        progress.setWindowModality(Qt::ApplicationModal);
        progress.setCancelButton(nullptr);
        progress.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
        progress.show();
        QEventLoop loop;
        std::unique_ptr<QThread> thread(QThread::create([result, state]() {
            try {
                result->stage = session_stages::open(state, &result->errors);
            } catch (const std::exception& e) {
                result->errors.push_back(e.what());
            }
        }));
        QObject::connect(thread.get(), &QThread::finished, &loop, &QEventLoop::quit);
        thread->start();
        loop.exec();
        thread->wait();
    }
    const auto& open = stageManager.stages();
    if (std::find(open.begin(), open.end(), stage) == open.end()) return fail(QObject::tr("The stage was closed."));
    if (!result->stage) {
        return fail(result->errors.empty() ? QObject::tr("%1 couldn't be opened.").arg(nameOf(stage))
                                           : qs(result->errors.front()));
    }
    // A server's layer is another layer in the other environment: its edits stay behind.
    if (!confirmUnsavedLayers(&stageManager, {stage}, parent, result->stage)) return fail(QString());
    return replaceStage(stageManager, stage, result->stage, state);
}

AssetEnvironmentButton::AssetEnvironmentButton(StageManager* stageManager, QWidget* parent)
    : QToolButton(parent), m_stageManager(stageManager) {
    setObjectName(QStringLiteral("assetEnvironmentButton"));
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    // Not setMenu(): a button with a menu keeps room for an arrow beside its text.
    m_menu = new QMenu(this);
    m_menu->setObjectName(QStringLiteral("assetEnvironmentMenu"));
    connect(this, &QToolButton::clicked, this, [this]() {
        fillMenu();
        // Upwards, from the status bar.
        m_menu->popup(mapToGlobal(QPoint(0, -m_menu->sizeHint().height())));
    });
    ask = [this](const QString& text) {
        QMessageBox box(QMessageBox::Question, tr("Asset Environment"), text, QMessageBox::Cancel, window());
        box.setInformativeText(
            tr("Reloading opens it again: the selection, cameras and the like are kept, its undo history isn't. Later "
               "does it when the stage is next reopened."));
        QPushButton* now = box.addButton(tr("Reload"), QMessageBox::AcceptRole);
        QPushButton* later = box.addButton(tr("Later"), QMessageBox::ActionRole);
        box.setDefaultButton(now);
        box.exec();
        return box.clickedButton() == now     ? Answer::Reload
               : box.clickedButton() == later ? Answer::Later
                                              : Answer::Cancel;
    };
    m_current = stageManager->currentStageChanged.connect([this](const StageRefPtr& stage) {
        m_ofCurrent = stage ? stage->assetEnvironmentChanged.connect([this]() { refresh(); })
                            : usdcc::core::Signal<>::Connection();
        refresh();
    });
    if (const StageRefPtr stage = stageManager->currentStage()) {
        m_ofCurrent = stage->assetEnvironmentChanged.connect([this]() { refresh(); });
    }
    m_config = assetConfigChanged().connect([this]() { refresh(); });
    // After the setting reached the library: whoever defined it listens too.
    usdcc::ui::onSettingChanged(this, kAssetEnvironmentKey, [this]() {
        QMetaObject::invokeMethod(this, [this]() { refresh(); }, Qt::QueuedConnection);
    });
    refresh();
}

void AssetEnvironmentButton::refresh() {
    setVisible(!assetEnvironments().empty());
    const StageRefPtr stage = m_stageManager->currentStage();
    const std::string fallback = defaultAssetEnvironment();
    const std::string actual = stage ? stageAssetEnvironment(stage->usdStage()) : fallback;
    const std::string pending = stage ? stage->pendingAssetEnvironment() : std::string();
    setText(pending.empty() ? environmentText(actual)
                            : QStringLiteral("%1 \u2192 %2").arg(environmentText(actual), qs(pending)));
    // Coloured by the theme (app.qss).
    const bool attention = actual != fallback || !pending.empty();
    if (property("attention").toBool() != attention) {
        setProperty("attention", attention);
        style()->unpolish(this);
        style()->polish(this);
    }
    QString tip = !stage            ? tr("The asset environment stages open in")
               : pending.empty() ? tr("The asset environment %1 is in").arg(nameOf(stage))
                                 : tr("%1 is in the asset environment %2, and in %3 once it's reopened")
                                       .arg(nameOf(stage), environmentText(actual), qs(pending));
    const std::string remembered = stage ? rememberedEnvironment(rootOf(stage)) : std::string();
    if (!remembered.empty()) tip += QLatin1Char('\n') + tr("Stages in its folder open in %1").arg(qs(remembered));
    setToolTip(tip);
}

void AssetEnvironmentButton::fillMenu() {
    QMenu* list = m_menu;
    list->clear();
    const StageRefPtr stage = m_stageManager->currentStage();
    const std::string wanted = stage ? wantedEnvironment(stage) : defaultAssetEnvironment();
    // A shared stage stays in its environment: reopening would end the share.
    const QString why = stage ? whyNotReopen(stage) : QString();
    if (!why.isEmpty()) {
        list->addAction(why)->setEnabled(false);
        list->addSeparator();
    }
    // Grouped by source (docs/ASSET_SOURCES.md §9): yours first, then each source's under its alias.
    std::vector<std::string> environments = assetEnvironments();
    std::stable_sort(environments.begin(), environments.end(), [](const std::string& a, const std::string& b) {
        return usdcc::assets::aliasOf(a) < usdcc::assets::aliasOf(b);
    });
    std::string group;
    for (const std::string& environment : environments) {
        const std::string alias = usdcc::assets::aliasOf(environment);
        if (alias != group) {
            // A heading of its own: this style draws a section without its text.
            list->addSeparator();
            list->addAction(qs(alias))->setEnabled(false);
            group = alias;
        }
        const QString label = alias.empty() ? qs(environment) : qs(environment.substr(alias.size() + 1));
        // "&" would be a mnemonic.
        QAction* action = list->addAction(QString(label).replace(QLatin1Char('&'), QLatin1String("&&")));
        action->setData(qs(environment));
        action->setCheckable(true);
        action->setChecked(environment == wanted);
        action->setEnabled(why.isEmpty());
        connect(action, &QAction::triggered, this, [this, environment, stage]() {
            pick(environment, stage ? std::vector<StageRefPtr>{stage} : std::vector<StageRefPtr>{});
        });
    }
    list->addSeparator();
    if (stage) {
        if (!stage->pendingAssetEnvironment().empty()) {
            list->addAction(tr("Reload in %1 Now").arg(qs(stage->pendingAssetEnvironment())), this,
                            [this, stage]() { reload({stage}); })
                ->setEnabled(why.isEmpty());
        }
        QAction* all = list->addAction(tr("Use %1 for All Open Stages").arg(environmentText(wanted)), this,
                                       [this, wanted]() { pick(wanted, m_stageManager->stages()); });
        all->setEnabled(m_stageManager->stages().size() > 1 && !wanted.empty());
        QAction* fresh = list->addAction(tr("Use %1 for New Stages").arg(environmentText(wanted)), this,
                                         [this, wanted]() { useForNewStages(wanted); });
        fresh->setEnabled(!wanted.empty() && wanted != defaultAssetEnvironment());
        // The folder's own choice, which wins over that: shown, set and taken back here.
        const std::string root = rootOf(stage);
        const std::string remembered = rememberedEnvironment(root);
        if (!folderOf(root).empty() && !wanted.empty() && hasAssetEnvironment(wanted) && wanted != remembered) {
            list->addAction(tr("Use %1 for This Folder").arg(environmentText(wanted)), this, [this, root, wanted]() {
                rememberFolderEnvironment(folderOf(root), wanted);
                refresh();
            });
        }
        if (!remembered.empty()) {
            list->addAction(tr("Forget %1 for This Folder").arg(qs(remembered)), this, [this, root]() {
                forgetFolderEnvironment(root);
                refresh();
            });
        }
        list->addSeparator();
    }
    list->addAction(tr("Asset Servers..."), this, [this]() { showAssetServersEditor(window()); });
}

void AssetEnvironmentButton::useForNewStages(const std::string& environment) {
    usdcc::core::Settings::instance().setValue(kAssetEnvironmentKey, usdcc::core::SettingValue(environment));
}

void AssetEnvironmentButton::pick(const std::string& environment, const std::vector<StageRefPtr>& stages) {
    if (stages.empty()) return useForNewStages(environment);
    std::vector<StageRefPtr> changing;
    QStringList staying;
    for (const StageRefPtr& stage : stages) {
        if (stageAssetEnvironment(stage->usdStage()) == environment) {
            stage->setPendingAssetEnvironment({});
            continue;
        }
        if (const QString reason = whyNotReopen(stage); !reason.isEmpty()) {
            staying << tr("%1 stays in its environment. %2").arg(nameOf(stage), reason);
            continue;
        }
        changing.push_back(stage);
    }
    if (changing.empty()) return;
    QString text = changing.size() == 1
                       ? tr("Reload %1 in the asset environment %2?").arg(nameOf(changing.front()), qs(environment))
                       : tr("Reload %1 stages in the asset environment %2?").arg(changing.size()).arg(qs(environment));
    if (!staying.isEmpty()) text += QStringLiteral("\n\n") + staying.join(QLatin1Char('\n'));
    const Answer answer = ask(text);
    if (answer == Answer::Cancel) return;
    for (const StageRefPtr& stage : changing) stage->setPendingAssetEnvironment(environment);
    if (answer == Answer::Reload) reload(changing);
}

bool AssetEnvironmentButton::reload(const std::vector<StageRefPtr>& stages) {
    for (const StageRefPtr& stage : stages) {
        QString error;
        if (reopenInEnvironment(*m_stageManager, stage, wantedEnvironment(stage), window(), &error)) continue;
        if (!error.isEmpty()) {
            QMessageBox::warning(window(), tr("Asset Environment"),
                                 tr("%1 stays in the environment it is in.\n\n%2").arg(nameOf(stage), error));
        }
        // What's left stays pending.
        return false;
    }
    return true;
}

}  // namespace usdcc::usd
