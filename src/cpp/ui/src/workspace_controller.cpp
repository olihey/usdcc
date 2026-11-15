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
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QSettings>
#include <QToolButton>
#include <QUrl>
#include <QWidgetAction>

namespace usdcc::ui {

namespace {

constexpr int kIconSize = 16;
constexpr int kMaxNameWidth = 160;

QString arrangedKey(const QString& id) { return QStringLiteral("%1/%2").arg(QLatin1String(WorkspaceController::kArrangedGroup), id); }

std::string shortcutKey(const QString& id) { return "Shortcuts/workspace." + id.toStdString(); }

// A 2×2 grid of panels in the accent colour, drawn rather than shipped so it
// follows the setting.
QIcon gridIcon(qreal pixelRatio) {
    const int size = scaled::pixels(kIconSize);
    QPixmap pixmap(QSize(size, size) * pixelRatio);
    pixmap.setDevicePixelRatio(pixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::accent());
    const qreal gap = size / 8.0;
    const qreal cell = (size - 3 * gap) / 2.0;
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            painter.drawRoundedRect(QRectF(gap + column * (cell + gap), gap + row * (cell + gap), cell, cell), 1.5, 1.5);
        }
    }
    return QIcon(pixmap);
}

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
    const QSettings settings = appSettings();
    m_chosen = settings.contains(QLatin1String(kCurrentKey));
    m_currentId = settings.value(QLatin1String(kCurrentKey), QLatin1String(kDefaultId)).toString();
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
    if (id != m_currentId) rememberArrangement();
    workspace::Definition definition = *found;
    // How it was left, if it was (§11 #2): the layout only; the file's side panels and fit still apply.
    const QByteArray arranged = appSettings().value(arrangedKey(id)).toByteArray();
    if (!arranged.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(arranged);
        if (std::optional<workspace::Definition> saved = workspace::fromJson(document.object(), id)) {
            definition.layout = saved->layout;
        }
    }
    return applyDefinition(definition, error);
}

bool WorkspaceController::reset(QString* error) {
    const workspace::Definition* definition = current();
    if (!definition) {
        if (error) *error = tr("There's no workspace \"%1\".").arg(m_currentId);
        return false;
    }
    {
        QSettings settings = appSettings();
        settings.remove(arrangedKey(m_currentId));
    }
    return applyDefinition(*definition, error);
}

bool WorkspaceController::resetToShadowed(const QString& id, QString* error) {
    if (!m_library->removeUserCopy(id, error)) return false;
    {
        QSettings settings = appSettings();
        settings.remove(arrangedKey(id));
    }
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
    {
        QSettings settings = appSettings();
        settings.remove(arrangedKey(id));
    }
    setCurrentId(id);
    return id;
}

void WorkspaceController::setCurrentId(const QString& id) {
    if (id.isEmpty()) return;
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

void WorkspaceController::rememberArrangement() {
    if (!m_chosen || !m_context.window || !m_library->find(m_currentId)) return;
    const workspace::Definition arranged = workspace::capture(m_context.window, m_currentId, currentName(), false);
    QSettings settings = appSettings();
    settings.setValue(arrangedKey(m_currentId), QJsonDocument(arranged.toJson()).toJson(QJsonDocument::Compact));
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
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("titleBarWorkspaceButton"));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setFocusPolicy(Qt::NoFocus);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    scaled::iconSize(button, kIconSize, kIconSize);
    auto* menu = new QMenu(button);
    menu->setObjectName(QStringLiteral("workspaceMenu"));
    menu->setToolTipsVisible(true);
    connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
        menu->clear();
        populateMenu(menu);
    });
    button->setMenu(menu);
    m_button = button;
    updateButton();
    // The icon is drawn in the accent colour, at the scale's size.
    onSettingChanged(button, theme::accentColorKey, [this]() { updateButton(); });
    UiScaleController::instance().bind(button, "workspaceIcon", [this]() { updateButton(); });
    return button;
}

void WorkspaceController::updateButton() {
    if (!m_button) return;
    const QString name = currentName();
    m_button->setIcon(gridIcon(m_button->devicePixelRatioF()));
    const QString shown = m_button->fontMetrics().elidedText(name, Qt::ElideRight, scaled::pixels(kMaxNameWidth));
    m_button->setText(shown);
    const workspace::Definition* definition = current();
    m_button->setToolTip(definition && !definition->description.isEmpty()
                             ? tr("Workspace: %1\n%2").arg(name, definition->description)
                             : tr("Workspace: %1").arg(name));
}

void WorkspaceController::populateMenu(QMenu* menu) {
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
            action->setChecked(definition->id == m_currentId);
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
            connect(action, &QAction::triggered, this, [this, id]() {
                QString error;
                if (!switchTo(id, &error) && m_context.window) {
                    QMessageBox::warning(m_context.window, tr("Workspace"), error);
                }
            });
        }
    }

    menu->addSeparator();
    if (m_fitAllViewsAction) menu->addAction(m_fitAllViewsAction);
    menu->addSeparator();
    if (const workspace::Definition* definition = current()) {
        QAction* resetAction = menu->addAction(tr("Reset \"%1\"").arg(definition->name));
        resetAction->setObjectName(QStringLiteral("resetWorkspace"));
        resetAction->setToolTip(tr("Lay it out as its file says, forgetting how it was left"));
        connect(resetAction, &QAction::triggered, this, [this]() {
            QString error;
            if (!reset(&error) && m_context.window) QMessageBox::warning(m_context.window, tr("Workspace"), error);
        });
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
    QAction* open = menu->addAction(tr("Open Workspaces Folder"));
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
