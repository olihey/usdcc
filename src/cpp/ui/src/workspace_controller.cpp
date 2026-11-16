#include "usdcc/ui/workspace_controller.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/settings_dialog.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/ui/workspace_library.h"

#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QToolButton>
#include <QUrl>
#include <QWidgetAction>

namespace usdcc::ui {

namespace {

constexpr int kMaxNameWidth = 160;

std::string shortcutKey(const QString& id) { return "Shortcuts/workspace." + id.toStdString(); }

void addHeading(QMenu* menu, const QString& text) {
    auto* label = new QLabel(text.toUpper(), menu);
    label->setObjectName(QStringLiteral("workspaceMenuHeading"));
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(label);
    action->setEnabled(false);
    menu->addAction(action);
}

}  // namespace

WorkspaceController::WorkspaceController(const workspace::ApplyContext& context, WorkspaceLibrary* library, QObject* parent)
    : QObject(parent), m_context(context), m_library(library) {
    QSettings settings = appSettings();
    m_chosen = settings.contains(QLatin1String(kCurrentKey));
    m_currentId = settings.value(QLatin1String(kCurrentKey), QLatin1String(kDefaultId)).toString();
    // Default's id was "layout": a profile that says so, or has a shortcut for it, means
    // Default. Before the shortcut settings are defined, so the one for it loads the value.
    if (m_currentId == QLatin1String(workspace::kFormerDefaultId)) {
        m_currentId = QLatin1String(kDefaultId);
        settings.setValue(QLatin1String(kCurrentKey), m_currentId);
    }
    const QString formerShortcut = QString::fromStdString(shortcutKey(QLatin1String(workspace::kFormerDefaultId)));
    if (settings.contains(formerShortcut)) {
        const QString shortcut = QString::fromStdString(shortcutKey(QLatin1String(kDefaultId)));
        if (!settings.contains(shortcut)) settings.setValue(shortcut, settings.value(formerShortcut));
        settings.remove(formerShortcut);
    }
    // How each workspace was left, which an earlier version laid out again instead of
    // its file: gone, so it doesn't sit in usdcc.ini for nothing.
    settings.beginGroup(QLatin1String(kObsoleteArrangedGroup));
    const bool obsolete = !settings.childKeys().isEmpty();
    settings.endGroup();
    if (obsolete) settings.remove(QLatin1String(kObsoleteArrangedGroup));
    connect(m_library, &WorkspaceLibrary::changed, this, [this]() {
        updateButton();
        syncShortcuts();
    });
    syncShortcuts();
}

const workspace::Definition* WorkspaceController::current() const { return m_library->find(m_currentId); }

QString WorkspaceController::currentName() const {
    const workspace::Definition* definition = current();
    return definition ? definition->name : m_currentId;
}

bool WorkspaceController::applyDefinition(const workspace::Definition& definition, QString* error) {
    const workspace::ApplyResult result = workspace::apply(definition, m_context);
    if (!result.applied) {
        if (error) {
            *error = result.refusedBy.isEmpty() ? tr("The workspace \"%1\" couldn't be laid out.").arg(definition.name)
                                                : tr("%1 didn't close, so the workspace stayed as it was.").arg(result.refusedBy);
        }
        return false;
    }
    if (!m_chosen) {
        m_chosen = true;
        QSettings settings = appSettings();
        settings.setValue(QLatin1String(kCurrentKey), definition.id);
    }
    if (m_currentId != definition.id) {
        m_currentId = definition.id;
        QSettings settings = appSettings();
        settings.setValue(QLatin1String(kCurrentKey), m_currentId);
        updateButton();
        Q_EMIT currentChanged(m_currentId);
    }
    return true;
}

bool WorkspaceController::switchTo(const QString& id, QString* error) {
    const workspace::Definition* found = m_library->find(id);
    if (!found) {
        if (error) *error = tr("There's no workspace \"%1\".").arg(id);
        return false;
    }
    // Always as its file says, the active one too: switching is how the window's layout
    // is put back. A layout worth keeping is saved as a workspace of its own.
    return applyDefinition(*found, error);
}

bool WorkspaceController::resetToShadowed(const QString& id, QString* error) {
    if (!m_library->removeUserCopy(id, error)) return false;
    if (id != m_currentId) return true;
    const workspace::Definition* definition = m_library->find(id);
    return definition ? applyDefinition(*definition, error) : true;
}

QString WorkspaceController::saveCurrentLayout(const QString& name, bool includeSidePanels, QString* error) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        if (error) *error = tr("A workspace needs a name.");
        return {};
    }
    // The id of a workspace with that name, if there is one, so saving again replaces it.
    QString id;
    for (const workspace::Definition& definition : m_library->workspaces()) {
        if (definition.name.compare(trimmed, Qt::CaseInsensitive) == 0) id = definition.id;
    }
    if (id.isEmpty()) id = workspace::idFromName(trimmed);
    workspace::Definition definition = workspace::capture(m_context.window, id, trimmed, includeSidePanels);
    if (const workspace::Definition* existing = m_library->find(id)) {
        definition.group = existing->group;
        definition.order = existing->order;
        definition.description = existing->description;
        definition.fitOnSwitch = existing->fitOnSwitch;
    } else {
        definition.order = 900;
    }
    if (!m_library->save(definition, error)) return {};
    setCurrentId(id);
    return id;
}

void WorkspaceController::setCurrentId(const QString& given) {
    if (given.isEmpty()) return;
    // A session saved while Default's id was "layout" names that.
    const QString id = workspace::renamedId(given);
    m_chosen = true;
    if (id == m_currentId) {
        QSettings settings = appSettings();
        settings.setValue(QLatin1String(kCurrentKey), m_currentId);
        return;
    }
    m_currentId = id;
    QSettings settings = appSettings();
    settings.setValue(QLatin1String(kCurrentKey), m_currentId);
    updateButton();
    Q_EMIT currentChanged(m_currentId);
}

void WorkspaceController::fitAllViews(const QString& what) {
    if (!m_context.window) return;
    for (ads::CDockWidget* dock : m_context.window->dockManager()->dockWidgetsMap()) {
        auto* panel = qobject_cast<ViewPanel*>(dock);
        // On screen: open, its tab in front.
        if (panel && workspace::isViewport(panel) && !panel->isClosed() && panel->isVisible()) panel->fitView(what);
    }
}

QToolButton* WorkspaceController::createButton(QWidget* parent) {
    // A push button in the accent colour, the workspace's name and a down arrow on its
    // right (dark.qss), as tall as a control rather than the title bar.
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("titleBarWorkspaceButton"));
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setFocusPolicy(Qt::NoFocus);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);  // 22 px tall (dark.qss)
    button->setMenu(createMenu(QString(), button));
    m_button = button;
    updateButton();
    // The name is elided at the scale's width.
    UiScaleController::instance().bind(button, "workspaceName", [this]() { updateButton(); });
    return button;
}

QMenu* WorkspaceController::createMenu(const QString& title, QWidget* parent) {
    auto* menu = new QMenu(title, parent);
    menu->setObjectName(QStringLiteral("workspaceMenu"));
    menu->setToolTipsVisible(true);
    connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
        menu->clear();
        populateMenu(menu);
    });
    return menu;
}

void WorkspaceController::updateButton() {
    if (!m_button) return;
    const QString name = currentName();
    const QString shown = m_button->fontMetrics().elidedText(name, Qt::ElideRight, scaled::pixels(kMaxNameWidth));
    m_button->setText(shown);
    const workspace::Definition* definition = current();
    m_button->setToolTip(definition && !definition->description.isEmpty()
                             ? tr("Workspace: %1\n%2").arg(name, definition->description)
                             : tr("Workspace: %1").arg(name));
}

void WorkspaceController::populateMenu(QMenu* menu) {
    addWorkspaceActions(menu);

    menu->addSeparator();
    if (m_fitAllViewsAction) menu->addAction(m_fitAllViewsAction);
    menu->addSeparator();
    if (const workspace::Definition* definition = current()) {
        const std::vector<workspace::Definition> hidden = m_library->shadowed(definition->id);
        if (!hidden.empty() && m_library->isUserWorkspace(definition->id)) {
            QAction* back = menu->addAction(tr("Reset to \"%1\"").arg(hidden.front().source));
            back->setObjectName(QStringLiteral("resetToShadowed"));
            back->setToolTip(tr("Move your copy to the recycle bin, and use %1 again")
                                 .arg(QDir::toNativeSeparators(hidden.front().file)));
            const QString id = definition->id;
            connect(back, &QAction::triggered, this, [this, id]() {
                QString error;
                if (!resetToShadowed(id, &error) && m_context.window) {
                    QMessageBox::warning(m_context.window, tr("Workspace"), error);
                }
            });
        }
    }
    QAction* save = menu->addAction(tr("Save Current Layout as Workspace..."));
    save->setObjectName(QStringLiteral("saveWorkspace"));
    connect(save, &QAction::triggered, this, [this]() { saveCurrentLayoutInteractively(m_context.window); });

    menu->addSeparator();
    QAction* open = menu->addAction(tr("Open Personal Workspace Folder"));
    open->setObjectName(QStringLiteral("openPersonalWorkspaceFolder"));
    open->setToolTip(QDir::toNativeSeparators(m_library->userFolder()));
    connect(open, &QAction::triggered, this, [this]() {
        QDir().mkpath(m_library->userFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_library->userFolder()));
    });
    QAction* reload = menu->addAction(tr("Reload Workspaces"));
    reload->setObjectName(QStringLiteral("reloadWorkspaces"));
    connect(reload, &QAction::triggered, m_library, &WorkspaceLibrary::reload);
    QAction* folders = menu->addAction(tr("Workspace Folders..."));
    connect(folders, &QAction::triggered, this, [this]() {
        if (SettingsDialog* dialog = SettingsDialog::showDialog(m_context.window)) dialog->showCategory(QStringLiteral("Files"));
    });
}

void WorkspaceController::addWorkspaceActions(QMenu* menu, const std::function<void()>& before, bool markCurrent) {
    const std::vector<WorkspaceLibrary::Group> groups = m_library->groups();
    // With one group, or none, no headings (§6.2).
    const bool headings = groups.size() > 1 || (groups.size() == 1 && !groups.front().name.isEmpty());
    for (const WorkspaceLibrary::Group& group : groups) {
        if (headings) {
            if (group.name.isEmpty()) {
                menu->addSeparator();
            } else {
                addHeading(menu, group.name);
            }
        }
        for (const workspace::Definition* definition : group.workspaces) {
            QAction* action = menu->addAction(definition->name);
            action->setObjectName(QStringLiteral("workspace:") + definition->id);
            action->setCheckable(true);
            action->setChecked(markCurrent && definition->id == m_currentId);
            QStringList tip;
            if (!definition->description.isEmpty()) tip << definition->description;
            QStringList missing;
            workspace::pruned(definition->layout,
                              [this](const QString& type) { return m_context.registry && m_context.registry->find(type); },
                              &missing);
            if (!missing.isEmpty()) {
                tip << tr("Needs %1, which isn't loaded: left out.").arg(missing.join(QStringLiteral(", ")));
            }
            tip << tr("%1: %2").arg(definition->source, QDir::toNativeSeparators(definition->file));
            action->setToolTip(tip.join(QLatin1Char('\n')));
            if (const QAction* shortcut = m_shortcutActions.value(definition->id)) action->setShortcut(shortcut->shortcut());
            const QString id = definition->id;
            connect(action, &QAction::triggered, this, [this, id, before]() {
                if (before) before();
                QString error;
                if (!switchTo(id, &error) && m_context.window) {
                    QMessageBox::warning(m_context.window, tr("Workspace"), error);
                }
            });
        }
    }
}

void WorkspaceController::saveCurrentLayoutInteractively(QWidget* parent) {
    bool ok = false;
    const QString name = QInputDialog::getText(parent, tr("Save Workspace"), tr("Name:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    for (const workspace::Definition& definition : m_library->workspaces()) {
        if (definition.name.compare(name.trimmed(), Qt::CaseInsensitive) != 0) continue;
        const QString question = m_library->isUserWorkspace(definition.id)
                                     ? tr("Replace your workspace \"%1\"?").arg(definition.name)
                                     : tr("\"%1\" comes from %2. Save your own, which will be used instead?")
                                           .arg(definition.name, definition.source);
        if (QMessageBox::question(parent, tr("Save Workspace"), question) != QMessageBox::Yes) return;
        break;
    }
    QString error;
    if (saveCurrentLayout(name, true, &error).isEmpty()) QMessageBox::warning(parent, tr("Save Workspace"), error);
}

void WorkspaceController::syncShortcuts() {
    if (!m_context.window) return;
    auto& settings = core::Settings::instance();
    QSet<QString> ids;
    for (const workspace::Definition& definition : m_library->workspaces()) {
        ids.insert(definition.id);
        const std::string key = shortcutKey(definition.id);
        settings.define({.key = key,
                         .category = "Shortcuts",
                         .group = "Workspaces",
                         .label = tr("Workspace: %1").arg(definition.name).toStdString(),
                         .description = tr("Switch to the %1 workspace").arg(definition.name).toStdString(),
                         .type = core::SettingType::Shortcut,
                         .defaultValue = std::string()});
        if (m_shortcutActions.value(definition.id)) continue;
        auto* action = new QAction(definition.name, m_context.window);
        action->setObjectName(QStringLiteral("workspaceShortcut:") + definition.id);
        action->setShortcutContext(Qt::ApplicationShortcut);
        const QString id = definition.id;
        connect(action, &QAction::triggered, this, [this, id]() {
            QString error;
            if (!switchTo(id, &error)) USDCC_LOG_WARNING("usdcc.workspaces", "{}", error.toStdString());
        });
        m_context.window->addAction(action);
        bindShortcut(action, key);
        m_shortcutActions.insert(definition.id, action);
    }
    // A workspace that's gone takes its action with it.
    for (auto it = m_shortcutActions.begin(); it != m_shortcutActions.end();) {
        if (!ids.contains(it.key())) {
            if (it.value()) it.value()->deleteLater();
            it = m_shortcutActions.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace usdcc::ui
