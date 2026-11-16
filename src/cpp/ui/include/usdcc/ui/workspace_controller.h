#pragma once

#include "usdcc/core/export.h"
#include "usdcc/ui/workspace.h"
#include "usdcc/ui/workspace_applier.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QAction;
class QMenu;
class QToolButton;

namespace usdcc::ui {

class WorkspaceLibrary;

// The active workspace (docs/WORKSPACES.md §6, §7): switches between the
// library's workspaces, remembers how each was left, saves the window's layout
// as a new one, and drives the title bar's workspace button and its menu.
class USDCC_UI_API WorkspaceController : public QObject {
    Q_OBJECT

public:
    // UI state in usdcc.ini: the active workspace's id, and how each was left
    // (Workspaces/arranged/<id>, a workspace's JSON).
    static constexpr const char* kCurrentKey = "Workspaces/current";
    static constexpr const char* kArrangedGroup = "Workspaces/arranged";
    // The workspace the first start gets, and the button shows before any is chosen.
    static constexpr const char* kDefaultId = "layout";

    WorkspaceController(const workspace::ApplyContext& context, WorkspaceLibrary* library, QObject* parent = nullptr);

    WorkspaceLibrary* library() const { return m_library; }
    const workspace::ApplyContext& context() const { return m_context; }

    // The active workspace's id; kDefaultId until one is chosen.
    const QString& currentId() const { return m_currentId; }
    // Its definition, or null if no folder has it any more.
    const workspace::Definition* current() const;
    // The name the button shows.
    QString currentName() const;

    // Switches to `id` (§7.1): how it was left, if it was, else its file. Saves how
    // the workspace being left looked first. False with `error` if there's no such
    // workspace or a panel refused to close.
    bool switchTo(const QString& id, QString* error = nullptr);
    // Lays the active workspace out from its file again, forgetting how it was left.
    bool reset(QString* error = nullptr);
    // Removes your copy of `id` (one shadowing another's), and lays out the one it hid.
    bool resetToShadowed(const QString& id, QString* error = nullptr);
    // Saves the window's layout as a workspace called `name` in your folder, and makes
    // it the active one. Returns its id, or empty with `error`.
    QString saveCurrentLayout(const QString& name, bool includeSidePanels = true, QString* error = nullptr);
    // Marks `id` as the active workspace without laying anything out: a session or
    // a saved layout has put the panels where they are already.
    void setCurrentId(const QString& id);
    // Remembers how the active workspace looks now (on quitting, and before leaving it).
    // Not before one was chosen: the layout a first start or an older version left
    // isn't how any workspace was left.
    void rememberArrangement();
    // Whether the active workspace was chosen (here or in an earlier run), rather than
    // being kDefaultId for lack of one.
    bool isChosen() const { return m_chosen; }

    // Fits every viewport on screen (§3.5.7): "selection" (else the stage) or "stage".
    void fitAllViews(const QString& what = QStringLiteral("selection"));
    // Fit All Views in the menu: the window's action (with its shortcut), set by the app.
    void setFitAllViewsAction(QAction* action) { m_fitAllViewsAction = action; }

    // The title bar's button: the active workspace's name and a down arrow, on the
    // accent colour, opening the workspace menu. The caller places it
    // (MainWindow::addTitleBarWidget()).
    QToolButton* createButton(QWidget* parent);
    // The workspace menu, filled each time it opens (populateMenu()): the title bar
    // button's, and View > Workspace.
    QMenu* createMenu(const QString& title, QWidget* parent);
    // Fills `menu` with the workspaces, grouped, and the commands below them (§6.2).
    void populateMenu(QMenu* menu);
    // Adds only the workspaces, grouped, each switching to it. `before`, when given, runs
    // first (Stages > a stage > Workspace makes the stage current). `markCurrent`: a
    // check on the active one.
    void addWorkspaceActions(QMenu* menu, const std::function<void()>& before = {}, bool markCurrent = true);

    // Asks for a name and saves the layout (the menu's Save Current Layout as Workspace...).
    void saveCurrentLayoutInteractively(QWidget* parent);

Q_SIGNALS:
    void currentChanged(const QString& id);

private:
    bool applyDefinition(const workspace::Definition& definition, QString* error);
    void updateButton();
    void syncShortcuts();

    workspace::ApplyContext m_context;
    WorkspaceLibrary* m_library;
    QString m_currentId;
    bool m_chosen = false;
    QPointer<QToolButton> m_button;
    QPointer<QAction> m_fitAllViewsAction;
    QHash<QString, QPointer<QAction>> m_shortcutActions;  // id -> the window's action
};

}  // namespace usdcc::ui
