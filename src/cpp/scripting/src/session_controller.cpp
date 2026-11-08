#include "usdcc/scripting/session_controller.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/core/version.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_editor_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/close_stage_dialog.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/session_stages.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"
#include "usdcc/usd/window_preview.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QThread>

#include <memory>

namespace usdcc::scripting {

namespace {
constexpr const char* kOnStartEmpty = "Empty";
constexpr const char* kOnStartLast = "Last session";

QString fileName(const QString& path) { return QFileInfo(path).fileName(); }

QString listed(const QStringList& items) {
    if (items.size() <= 1) return items.join(QString());
    return QObject::tr("%1 and %2").arg(items.mid(0, items.size() - 1).join(QStringLiteral(", ")), items.last());
}

// Runs `work` on a thread of its own while the event loop goes on, so the window
// keeps drawing (behind a modal progress dialog that takes the input).
template <typename Work>
void runOffThread(Work work) {
    QEventLoop loop;
    QThread* thread = QThread::create(std::move(work));
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    thread->start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    thread->wait();
    delete thread;
}
}  // namespace

void SessionController::defineSettings() {
    usdcc::core::Settings::instance().define({.key = kOnStartKey,
                                              .category = "Files",
                                              .group = "Sessions",
                                              .label = "On start",
                                              .description = "What the app opens with: nothing, or the session it had "
                                                             "when it was last closed (docs/SESSIONS.md).",
                                              .type = usdcc::core::SettingType::Choice,
                                              .defaultValue = std::string(kOnStartEmpty),
                                              .choices = {kOnStartEmpty, kOnStartLast}});
}

bool SessionController::loadsLastSessionAtStart() {
    auto& settings = usdcc::core::Settings::instance();
    return settings.contains(kOnStartKey) && settings.get<std::string>(kOnStartKey) == kOnStartLast;
}

SessionController::SessionController(const Context& context, const QString& folder, QObject* parent)
    : QObject(parent), m_context(context), m_store(folder) {
    QWidget* window = m_context.window;
    m_prompts.saveDespite = [window](const QString& message) {
        QMessageBox box(QMessageBox::Warning, tr("Save Session"), message, QMessageBox::Cancel, window);
        QPushButton* save = box.addButton(tr("Save Session Anyway"), QMessageBox::AcceptRole);
        box.setDefaultButton(save);
        box.exec();
        return box.clickedButton() == save;
    };
    m_prompts.plugins = [window](const QString& message) {
        QMessageBox box(QMessageBox::Question, tr("Load Session"), message, QMessageBox::Cancel, window);
        QPushButton* restart = box.addButton(tr("Restart and Load"), QMessageBox::AcceptRole);
        QPushButton* without = box.addButton(tr("Load Without"), QMessageBox::ActionRole);
        box.setDefaultButton(restart);
        box.exec();
        if (box.clickedButton() == restart) return Prompts::PluginAnswer::RestartAndLoad;
        if (box.clickedButton() == without) return Prompts::PluginAnswer::LoadWithout;
        return Prompts::PluginAnswer::Cancel;
    };
    m_prompts.replaceScripts = [window](const QString& message) {
        return QMessageBox::question(window, tr("Load Session"), message, QMessageBox::Ok | QMessageBox::Cancel,
                                     QMessageBox::Ok) == QMessageBox::Ok;
    };
    m_prompts.locate = [window](const QString& missing) {
        QMessageBox box(QMessageBox::Question, tr("Load Session"),
                        tr("The session's file %1 isn't where it was:\n%2\n\nLocate it, and the files beside it are "
                           "found too.")
                            .arg(fileName(missing), QDir::toNativeSeparators(missing)),
                        QMessageBox::NoButton, window);
        QPushButton* locate = box.addButton(tr("Locate..."), QMessageBox::AcceptRole);
        box.addButton(tr("Skip"), QMessageBox::RejectRole);
        box.setDefaultButton(locate);
        box.exec();
        if (box.clickedButton() != locate) return QString();
        const QString suffix = QFileInfo(missing).suffix();
        return QFileDialog::getOpenFileName(window, tr("Where is %1?").arg(fileName(missing)), QString(),
                                            suffix.isEmpty() ? QString() : tr("%1 (*.%2)").arg(fileName(missing), suffix));
    };
}

// --- Capturing -------------------------------------------------------------------

QJsonArray SessionController::captureStages(std::vector<usdcc::usd::StageRefPtr>* order) const {
    QJsonArray stages;
    const usdcc::usd::StageRefPtr current = m_context.stageManager->currentStage();
    for (const usdcc::usd::StageRefPtr& stage : m_context.stageManager->stages()) {
        QJsonObject state = usdcc::usd::session_stages::capture(*m_context.stageManager, stage);
        if (stage == current) state.insert(QStringLiteral("current"), true);
        stages << state;
        order->push_back(stage);
    }
    return stages;
}

QJsonObject SessionController::capture(const QString& name, const QImage& preview) const {
    QJsonObject json = usdcc::ui::session::create(name);
    json.insert(QStringLiteral("app"), QString::fromLatin1(usdcc::core::version()));

    std::vector<usdcc::usd::StageRefPtr> order;
    json.insert(QStringLiteral("stages"), captureStages(&order));

    if (m_context.plugins) {
        QJsonArray plugins;
        for (const PluginInfo& plugin : m_context.plugins->plugins()) {
            if (plugin.state != PluginState::Active) continue;
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), QString::fromStdString(plugin.manifest.id));
            entry.insert(QStringLiteral("name"), QString::fromStdString(plugin.manifest.name));
            entry.insert(QStringLiteral("version"), QString::fromStdString(plugin.manifest.version));
            plugins << entry;
        }
        json.insert(QStringLiteral("plugins"), plugins);
    }

    if (m_context.window) {
        json.insert(QStringLiteral("layout"), m_context.window->layoutState());
        QJsonArray panels;
        for (ads::CDockWidget* dock : m_context.window->dockManager()->dockWidgetsMap()) {
            auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(dock);
            const QString type = panel ? panel->property(usdcc::ui::ViewPanelRegistry::kTypeProperty).toString() : QString();
            if (type.isEmpty()) continue;
            // A closed one would come back hidden every time; the app's own are kept.
            if (panel->isClosed() && !m_context.fixedPanels.contains(panel->objectName())) continue;
            QJsonObject entry;
            entry.insert(QStringLiteral("type"), type);
            entry.insert(QStringLiteral("name"), panel->objectName());
            const auto shown = std::find(order.begin(), order.end(), panel->currentStage());
            entry.insert(QStringLiteral("stage"), shown == order.end() ? -1 : int(shown - order.begin()));
            entry.insert(QStringLiteral("optionsPinned"), panel->isOptionsPinned());
            const QJsonObject state = panel->saveSessionState();
            if (!state.isEmpty()) entry.insert(QStringLiteral("state"), state);
            panels << entry;
        }
        json.insert(QStringLiteral("panels"), panels);
    }

    if (m_context.scriptEditor) json.insert(QStringLiteral("scriptEditor"), m_context.scriptEditor->sessionState());

    if (m_context.python && m_context.python->isAvailable()) {
        const auto data = m_context.python->evaluate("__import__('usdcc.app', fromlist=['app'])._session_save_data()");
        const QJsonDocument document = data ? QJsonDocument::fromJson(QByteArray::fromStdString(*data)) : QJsonDocument();
        if (document.isObject() && !document.object().isEmpty()) json.insert(QStringLiteral("pluginData"), document.object());
    }

    usdcc::ui::session::setPreview(json, usdcc::ui::session::scaledPreview(preview));
    return json;
}

QImage SessionController::takePreview() const {
    return m_context.window ? usdcc::usd::captureWindowPreview(m_context.window) : QImage();
}

QString SessionController::saveWarning() const {
    // A stage in memory is kept whole, as its text; a file's edits aren't.
    QStringList unsaved;
    const std::vector<std::string> names = usdcc::usd::shortStageNames(m_context.stageManager->stages());
    for (std::size_t i = 0; i < m_context.stageManager->stages().size(); ++i) {
        const usdcc::usd::StageRefPtr& stage = m_context.stageManager->stages()[i];
        if (usdcc::usd::session_stages::hasFile(stage) && !usdcc::usd::layer_actions::unsavedLayers(stage).empty()) {
            unsaved << QString::fromStdString(names[i]);
        }
    }
    if (unsaved.isEmpty()) return {};
    return tr("%1 %2 unsaved edits. The session only remembers the files, so these edits won't be in it.")
        .arg(listed(unsaved), unsaved.size() == 1 ? tr("has") : tr("have"));
}

bool SessionController::save(const QString& name, const QImage& preview, bool ask, QString* error) {
    m_warnings.clear();
    if (name.trimmed().isEmpty()) {
        if (error) *error = tr("A session needs a name.");
        return false;
    }
    if (ask) {
        const QString warning = saveWarning();
        if (!warning.isEmpty() && m_prompts.saveDespite && !m_prompts.saveDespite(warning)) return false;
    }
    if (!m_store.write(capture(name.trimmed(), preview), error)) return false;
    m_current = name.trimmed();
    USDCC_LOG_INFO("usdcc.session", "Saved the session \"{}\"", m_current.toStdString());
    Q_EMIT sessionsChanged();
    return true;
}

// --- Loading ---------------------------------------------------------------------

QString SessionController::pluginName(const QString& id) const {
    if (m_context.plugins) {
        if (const PluginInfo* plugin = m_context.plugins->find(id.toStdString())) {
            return QString::fromStdString(plugin->manifest.name);
        }
    }
    return id;
}

SessionController::PluginDifference SessionController::pluginDifference(const QJsonObject& session) const {
    PluginDifference difference;
    if (!m_context.plugins || !session.contains(QStringLiteral("plugins"))) return difference;
    QSet<QString> wanted;
    QHash<QString, QString> names;
    for (const QJsonValue& value : session.value(QStringLiteral("plugins")).toArray()) {
        const QJsonObject plugin = value.toObject();
        const QString id = plugin.value(QStringLiteral("id")).toString();
        wanted.insert(id);
        names.insert(id, plugin.value(QStringLiteral("name")).toString(id));
    }
    for (const PluginInfo& plugin : m_context.plugins->plugins()) {
        const QString id = QString::fromStdString(plugin.manifest.id);
        const bool active = plugin.state == PluginState::Active;
        if (wanted.contains(id) && !active) {
            if (!m_context.plugins->isEnabled(plugin.manifest.id)) {
                difference.enable << id;
            } else {
                // On, but not running here (refused, failed): a restart won't change that.
                difference.missing << QString::fromStdString(plugin.manifest.name);
            }
        } else if (!wanted.contains(id) && active) {
            difference.disable << id;
        }
    }
    for (const QString& id : std::as_const(wanted)) {
        if (!m_context.plugins->find(id.toStdString())) difference.missing << names.value(id);
    }
    return difference;
}

bool SessionController::locateMissing(QJsonObject& session, const LoadOptions& options) {
    QStringList missing = usdcc::ui::session::missingFiles(session);
    if (missing.isEmpty() || !options.interactive || options.atStart || !m_prompts.locate) return false;
    bool changed = false;
    QSet<QString> skipped;
    while (true) {
        missing = usdcc::ui::session::missingFiles(session);
        QString next;
        for (const QString& file : std::as_const(missing)) {
            if (!skipped.contains(file)) {
                next = file;
                break;
            }
        }
        if (next.isEmpty()) break;
        const QString found = m_prompts.locate(next);
        if (found.isEmpty()) break;  // Skip: the rest are most likely in the same place
        const auto [from, to] = usdcc::ui::session::folderMapping(next, found);
        const int count = from.isEmpty() ? usdcc::ui::session::remapPaths(session, next, found)
                                         : usdcc::ui::session::remapPaths(session, from, to);
        if (count == 0) {
            skipped.insert(next);
            continue;
        }
        changed = true;
        USDCC_LOG_INFO("usdcc.session", "Found the session's files under {} ({} paths)",
                       QDir::toNativeSeparators(from.isEmpty() ? found : to).toStdString(), count);
    }
    // Asked only once: the stored session keeps the new paths.
    if (changed && !options.source.isEmpty() && !options.source.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) &&
        m_store.contains(options.source)) {
        QString error;
        if (!m_store.write(session, &error)) USDCC_LOG_WARNING("usdcc.session", "{}", error.toStdString());
    }
    return changed;
}

bool SessionController::askAboutUnsaved(const LoadOptions& options, QString* error) {
    usdcc::usd::StageManager& stages = *m_context.stageManager;
    std::vector<std::pair<usdcc::usd::StageRefPtr, PXR_NS::SdfLayerHandle>> unsaved;
    for (const usdcc::usd::StageRefPtr& stage : stages.stages()) {
        for (const PXR_NS::SdfLayerHandle& layer : usdcc::usd::layer_actions::unsavedLayers(stage)) {
            unsaved.emplace_back(stage, layer);
        }
    }
    if (options.interactive) {
        if (!usdcc::usd::confirmUnsavedLayers(&stages, stages.stages(), m_context.window)) return false;
        if (m_context.scriptEditor) {
            if (!m_context.scriptEditor->canClose()) return false;
            const QStringList scratch = m_context.scriptEditor->untitledWithText();
            if (!scratch.isEmpty() && m_prompts.replaceScripts &&
                !m_prompts.replaceScripts(tr("The Script Editor's %1 %2 replaced by the session's tabs.")
                                              .arg(listed(scratch), scratch.size() == 1 ? tr("is") : tr("are")))) {
                return false;
            }
        }
    } else if (!unsaved.empty() && !options.discardUnsaved) {
        QStringList names;
        for (const auto& [stage, layer] : unsaved) names << QString::fromStdString(layer->GetDisplayName());
        if (error) *error = tr("%1 %2 unsaved edits.").arg(listed(names), names.size() == 1 ? tr("has") : tr("have"));
        return false;
    }
    // What's still unsaved now was discarded: back to the files, so that opening them
    // again doesn't find the edited layers still in memory.
    for (const auto& [stage, layer] : unsaved) {
        if (layer && layer->IsDirty()) layer->Reload(true);
    }
    return true;
}

void SessionController::applyPluginData(const QJsonObject& data) {
    if (!m_context.python || !m_context.python->isAvailable()) return;
    const QByteArray encoded = QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64();
    m_context.python->evaluate("__import__('usdcc.app', fromlist=['app'])._session_load_data('" + encoded.toStdString() +
                               "')");
}

SessionController::LoadResult SessionController::load(const QString& nameOrPath, const LoadOptions& options,
                                                      QString* error) {
    std::optional<QJsonObject> session = m_store.read(nameOrPath, error);
    if (!session) return LoadResult::Failed;
    LoadOptions withSource = options;
    if (withSource.source.isEmpty()) withSource.source = nameOrPath;
    return load(*session, withSource, error);
}

SessionController::LoadResult SessionController::load(QJsonObject session, const LoadOptions& options, QString* error) {
    m_warnings.clear();
    if (const QString why = usdcc::ui::session::problem(session); !why.isEmpty()) {
        if (error) *error = why;
        return LoadResult::Failed;
    }
    const QString name = usdcc::ui::session::name(session);
    usdcc::usd::StageManager& stageManager = *m_context.stageManager;

    // 1. Plugins (§4.3).
    const PluginDifference plugins = pluginDifference(session);
    if (!options.atStart && plugins.needsRestart()) {
        QStringList lines;
        for (const QString& id : plugins.enable) lines << tr("%1 will be turned on.").arg(pluginName(id));
        for (const QString& id : plugins.disable) lines << tr("%1 will be turned off.").arg(pluginName(id));
        for (const QString& missing : plugins.missing) lines << tr("%1 isn't installed here.").arg(missing);
        const QString message = tr("\"%1\" was saved with other plugins, which takes a restart:\n\n%2")
                                    .arg(name, lines.join(QLatin1Char('\n')));
        if (!options.interactive) {
            if (error) *error = message;
            return LoadResult::Failed;
        }
        const auto answer = m_prompts.plugins ? m_prompts.plugins(message) : Prompts::PluginAnswer::Cancel;
        if (answer == Prompts::PluginAnswer::Cancel) return LoadResult::Cancelled;
        if (answer == Prompts::PluginAnswer::RestartAndLoad) {
            if (!m_context.restart || !m_context.plugins) {
                if (error) *error = tr("The app can't restart itself here.");
                return LoadResult::Failed;
            }
            QString source = options.source;
            if (source.isEmpty()) {
                if (!m_store.write(session, error)) return LoadResult::Failed;
                source = name;
            }
            for (const QString& id : plugins.enable) m_context.plugins->setEnabled(id.toStdString(), true);
            for (const QString& id : plugins.disable) m_context.plugins->setEnabled(id.toStdString(), false);
            if (m_context.restart({QStringLiteral("--open-session"), source})) return LoadResult::Restarting;
            // The window stayed: the user cancelled a question. Nothing changes.
            for (const QString& id : plugins.enable) m_context.plugins->setEnabled(id.toStdString(), false);
            for (const QString& id : plugins.disable) m_context.plugins->setEnabled(id.toStdString(), true);
            return LoadResult::Cancelled;
        }
    }
    for (const QString& missing : plugins.missing) {
        m_warnings << tr("The plugin %1 isn't here; the session loads without it.").arg(missing);
    }

    // 2. Files that aren't where they were (§5.1), and what's unsaved now.
    locateMissing(session, options);
    if (!options.atStart && !askAboutUnsaved(options, error)) {
        return error && !error->isEmpty() ? LoadResult::Failed : LoadResult::Cancelled;
    }

    if (m_context.setLoading) m_context.setLoading(true);
    ads::CDockManager* docks = m_context.window ? m_context.window->dockManager() : nullptr;
    const QJsonArray panels = session.value(QStringLiteral("panels")).toArray();

    // 3. Close everything: the panels the session doesn't have, then every stage.
    if (docks) {
        QSet<QString> keep(m_context.fixedPanels.begin(), m_context.fixedPanels.end());
        for (const QJsonValue& value : panels) keep.insert(value.toObject().value(QStringLiteral("name")).toString());
        const auto all = docks->dockWidgetsMap().values();
        for (ads::CDockWidget* dock : all) {
            auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(dock);
            if (!panel || panel->property(usdcc::ui::ViewPanelRegistry::kTypeProperty).toString().isEmpty()) continue;
            if (keep.contains(panel->objectName())) {
                panel->setStage(nullptr);
            } else {
                panel->deleteDockWidget();
            }
        }
    }
    const std::vector<usdcc::usd::StageRefPtr> open = stageManager.stages();
    for (const usdcc::usd::StageRefPtr& stage : open) stageManager.closeStage(stage);

    // 4. and 5. The stages, then what each had.
    const QJsonArray stageStates = session.value(QStringLiteral("stages")).toArray();
    std::vector<usdcc::usd::StageRefPtr> stages(stageStates.size());
    usdcc::usd::StageRefPtr current;
    QPointer<QProgressDialog> progress;
    const bool showProgress = m_context.window && m_context.window->isVisible() && !options.atStart;
    if (showProgress && !stageStates.isEmpty()) {
        progress = new QProgressDialog(QString(), QString(), 0, int(stageStates.size()), m_context.window);
        progress->setWindowTitle(tr("Load Session"));
        progress->setWindowModality(Qt::WindowModal);
        progress->setCancelButton(nullptr);
        progress->setMinimumDuration(300);
    }
    for (qsizetype i = 0; i < stageStates.size(); ++i) {
        const QJsonObject state = stageStates.at(i).toObject();
        const QString file = state.value(QStringLiteral("file")).toString();
        if (progress) {
            progress->setLabelText(tr("Opening %1 of %2: %3").arg(i + 1).arg(stageStates.size()).arg(fileName(file)));
            progress->setValue(int(i));
        }
        PXR_NS::UsdStageRefPtr usdStage;
        std::vector<std::string> errors;
        if (showProgress) {
            runOffThread([&usdStage, &errors, state]() {
                usdcc::core::setThreadName("session-loader");
                usdStage = usdcc::usd::session_stages::open(state, &errors);
            });
        } else {
            usdStage = usdcc::usd::session_stages::open(state, &errors);
        }
        if (!usdStage) {
            m_warnings << tr("Couldn't open %1%2").arg(QDir::toNativeSeparators(file),
                                                        errors.empty() ? QStringLiteral(".")
                                                                       : QStringLiteral(": ") + QString::fromStdString(errors.front()));
            continue;
        }
        stages[i] = stageManager.adoptStage(usdStage);
        usdcc::usd::session_stages::apply(stageManager, stages[i], state, &m_warnings);
        if (state.value(QStringLiteral("current")).toBool()) current = stages[i];
    }
    delete progress.data();

    // 6. The panels: those kept get their stage, the others are made.
    std::vector<std::pair<usdcc::ui::ViewPanel*, QJsonObject>> made;
    if (docks) {
        for (const QJsonValue& value : panels) {
            const QJsonObject entry = value.toObject();
            const QString panelName = entry.value(QStringLiteral("name")).toString();
            const QString type = entry.value(QStringLiteral("type")).toString();
            const int index = entry.value(QStringLiteral("stage")).toInt(-1);
            const usdcc::usd::StageRefPtr stage = index >= 0 && index < int(stages.size()) ? stages[index] : nullptr;
            auto* panel = qobject_cast<usdcc::ui::ViewPanel*>(docks->findDockWidget(panelName));
            if (panel) {
                panel->setStage(stage);
            } else {
                const usdcc::ui::ViewPanelRegistry::Entry* kind = m_context.panelTypes ? m_context.panelTypes->find(type) : nullptr;
                if (!kind || !kind->create || panelName.isEmpty()) {
                    m_warnings << tr("There's no %1 panel here (from a plugin that isn't on?); it's left out.").arg(type);
                    continue;
                }
                panel = kind->create(stage, kind->displayName, m_context.window);
                if (!panel) continue;
                panel->setProperty(usdcc::ui::ViewPanelRegistry::kTypeProperty, type);
                panel->setObjectName(panelName);
                docks->addDockWidget(ads::CenterDockWidgetArea, panel);
            }
            made.emplace_back(panel, entry);
        }
    }

    // 7. The layout, now that every panel it places exists.
    if (m_context.window && session.contains(QStringLiteral("layout"))) {
        m_context.window->restoreLayoutState(session.value(QStringLiteral("layout")).toObject(), options.atStart);
    }

    // 8. What each panel had.
    for (auto& [panel, entry] : made) {
        const bool pinned = entry.value(QStringLiteral("optionsPinned")).toBool();
        if (panel->isOptionsPinned() != pinned) panel->setOptionsPinned(pinned);
        if (entry.contains(QStringLiteral("state"))) panel->restoreSessionState(entry.value(QStringLiteral("state")).toObject());
    }
    if (m_context.scriptEditor && session.contains(QStringLiteral("scriptEditor"))) {
        m_context.scriptEditor->restoreSessionState(session.value(QStringLiteral("scriptEditor")).toObject(), &m_warnings);
    }

    // 9. The current stage, with a panel showing it focused, so it stays current.
    if (!current) {
        for (const usdcc::usd::StageRefPtr& stage : stages) {
            if (stage) {
                current = stage;
                break;
            }
        }
    }
    if (current) {
        stageManager.setCurrentStage(current);
        if (docks) {
            usdcc::ui::ViewPanel* focus = nullptr;
            for (auto& [panel, entry] : made) {
                if (panel->currentStage() != current || panel->isClosed()) continue;
                if (!focus || panel->property(usdcc::ui::ViewPanelRegistry::kTypeProperty).toString() == QLatin1String("viewport")) {
                    focus = panel;
                }
            }
            if (focus) docks->setDockWidgetFocused(focus);
        }
    }

    // 10. The plugins' own data.
    applyPluginData(session.value(QStringLiteral("pluginData")).toObject());

    if (m_context.setLoading) m_context.setLoading(false);
    m_current = name;
    for (const QString& warning : std::as_const(m_warnings)) {
        USDCC_LOG_WARNING("usdcc.session", "{}", warning.toStdString());
    }
    USDCC_LOG_INFO("usdcc.session", "Loaded the session \"{}\"", name.toStdString());
    Q_EMIT sessionsChanged();
    return LoadResult::Loaded;
}

// --- SessionHost -------------------------------------------------------------------

std::vector<std::string> SessionController::sessionNames() const {
    std::vector<std::string> names;
    for (const usdcc::ui::SessionInfo& info : m_store.list()) names.push_back(info.name.toStdString());
    return names;
}

std::string SessionController::saveSession(const std::string& name) {
    QString error;
    if (!save(QString::fromStdString(name), takePreview(), false, &error)) return error.toStdString();
    return {};
}

std::string SessionController::loadSession(const std::string& nameOrPath, bool discardUnsaved) {
    QString error;
    LoadOptions options;
    options.interactive = false;
    options.discardUnsaved = discardUnsaved;
    const LoadResult result = load(QString::fromStdString(nameOrPath), options, &error);
    if (result == LoadResult::Loaded) return {};
    return (error.isEmpty() ? tr("The session wasn't loaded.") : error).toStdString();
}

std::string SessionController::deleteSession(const std::string& name) {
    QString error;
    if (!m_store.remove(QString::fromStdString(name), &error)) return error.toStdString();
    if (m_current == QString::fromStdString(name)) m_current.clear();
    Q_EMIT sessionsChanged();
    return {};
}

std::string SessionController::importSession(const std::string& path, bool replace, std::string* name) {
    QString error;
    const QString imported = m_store.importFile(QString::fromStdString(path), replace, &error);
    if (imported.isEmpty()) return error.toStdString();
    if (name) *name = imported.toStdString();
    Q_EMIT sessionsChanged();
    return {};
}

std::string SessionController::exportSession(const std::string& name, const std::string& path) {
    QString error;
    if (!m_store.exportTo(QString::fromStdString(name), QString::fromStdString(path), &error)) return error.toStdString();
    return {};
}

}  // namespace usdcc::scripting
