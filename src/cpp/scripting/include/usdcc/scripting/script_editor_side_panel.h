#pragma once

#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/side_panel.h"

#include <QJsonObject>
#include <QMessageBox>
#include <QStringList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QAction;
class QIcon;
class QFileSystemWatcher;
class QMenu;
class QPlainTextEdit;
class QSettings;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolButton;
class QTreeView;

namespace usdcc::scripting {

class CodeEditor;
class ScriptRunner;

// The Script Editor panel (docs/SCRIPTING.md §7): Python editors in tabs
// (CodeEditor, PythonHighlighter), Run (Ctrl+Enter) and Run Selection
// (Ctrl+Shift+Enter) on the current tab, and an output pane showing each
// run's header, its print output, warnings and tracebacks. Everything a run
// prints also goes to the Log panel; each run is one undo step per stage it
// changes (ScriptRunner).
//
// Each tab is a file or an untitled script; its name has a • while it has
// unsaved changes. New Tab (Ctrl+T, or a double-click beside the tabs), Open
// (Ctrl+O, in a new tab), Save (Ctrl+S), Save As and Open Recent are in the
// ☰ menu; Clear, Rename, Save, Save As, Duplicate, Ruff Format, Ruff Check,
// Ruff Fix and Close in each tab's context menu.
//
// A tab can also be a plugin (docs/PLUGIN_DEV.md): the plugin's folder as a tree on
// the left, and its open files in tabs of their own on the right. There, Run is
// Reload (Ctrl+Enter, F5), which saves its files and puts the changes into effect.
//
// The open tabs, untitled scripts' text, the recent files, the split and the
// ☰ menu's Keep Variables Between Runs persist; the tabs are saved a moment
// after each change, not only on exit. Closing a tab, or the app,
// with unsaved changes to a file asks whether to save them; untitled
// scripts come back as they were without asking. A file changing on disk updates
// its tab, asking first if the tab has unsaved changes. When Python couldn't
// start, the panel shows why instead of the editor.
class ScriptEditorSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    // `settingsFile` is for tests; empty means the app's settings.
    ScriptEditorSidePanel(PythonRuntime& runtime, ScriptRunner& runner, QWidget* parent = nullptr,
                          const QString& settingsFile = {});
    ~ScriptEditorSidePanel() override;

    // The current tab's editor; null when Python is unavailable, or on a plugin's tab
    // with no file open.
    CodeEditor* editor() const;
    QPlainTextEdit* output() const { return m_output; }
    QTabBar* tabBar() const { return m_tabBar; }

    // Runs the current tab's whole text, or its selection (Run Selection:
    // the cursor's line without one). Return whether the script ran without
    // an exception. On a plugin's tab, run() reloads the plugin.
    bool run();
    bool runSelection();

    // The current tab's script; on a plugin's tab, its current file.
    const QString& currentFile() const;
    QString currentName() const;
    bool isModified() const;

    // The top-level tabs, where a plugin is one tab.
    int tabCount() const;
    int currentTab() const;
    void setCurrentTab(int index);

    // Each opens a tab and makes it current. openFile() switches to the file's
    // tab if it is open already, and reuses an empty untitled current tab.
    void newScript();
    bool openFile(const QString& path);  // asks for a path when empty
    void duplicateTab(int index);

    // On the current tab.
    bool save();
    bool saveAs(const QString& path = {});  // asks for a path when empty

    // On any tab. Clear is one undo step in that tab's editor. Renaming an
    // untitled script names its tab; renaming a file renames it on disk (in
    // its folder, adding .py when there is no suffix). Closing asks about
    // unsaved changes (an untitled script's too), and returns false when
    // the user cancels; closing the last tab leaves an empty untitled one.
    // On a plugin's tab these act on its current file, except that closing closes
    // the whole plugin tab.
    bool saveTab(int index);
    bool saveTabAs(int index, const QString& path = {});
    void clearTab(int index);
    bool renameTab(int index, const QString& name = {});  // asks for the name when empty
    bool closeTab(int index);

    // Ruff (https://docs.astral.sh/ruff/): the pinned one bundled with the app
    // (a deploy's tools/ruff, or the build's, cmake/modules/UsdccRuff.cmake),
    // else one on PATH or in the Python's Scripts folder. Format replaces the
    // tab's text with `ruff format`'s, and Fix with `ruff check --fix`'s (each
    // one undo step; Fix prints what it couldn't fix); Check prints
    // `ruff check`'s findings to the output. All use the ruff settings of the
    // file's folder. Return false when ruff is missing or fails.
    QString ruffProgram() const;
    bool ruffFormat(int index);
    bool ruffFix(int index);
    bool ruffCheck(int index);

    void clearOutput();
    bool keepsVariables() const;
    void setKeepVariables(bool keep);
    const QStringList& recentFiles() const { return m_recentFiles; }

    // Asks about each file with unsaved changes (SidePanel::canClose()).
    bool canClose() override;

    // Every tab, for a session (docs/SESSIONS.md §2.5): untitled scripts with their
    // text, files by path (with their text while it has unsaved changes), plugin tabs
    // with their files, and which tab is current.
    QJsonObject sessionState() const;
    // Replaces every tab with `state`'s, without asking: ask canClose() first. A file
    // that isn't there comes back with its stored text if it has any, and is left
    // out (with a warning in `warnings`) if not; so is a plugin tab whose folder is gone.
    void restoreSessionState(const QJsonObject& state, QStringList* warnings = nullptr);
    // The untitled tabs that have text, by name: what loading a session replaces.
    QStringList untitledWithText() const;

    // Asks whether to save `name`'s changes: Save, Discard or Cancel. A
    // message box unless set (tests answer without one).
    using SavePrompt = std::function<QMessageBox::StandardButton(const QString& name)>;
    void setSavePrompt(SavePrompt prompt) { m_savePrompt = std::move(prompt); }
    // Asks whether to load `name`, which changed on disk, over its unsaved changes.
    using DiskChangePrompt = std::function<bool(const QString& name)>;
    void setDiskChangePrompt(DiskChangePrompt prompt) { m_diskPrompt = std::move(prompt); }

    // Fill the ☰ menu and a tab's context menu (public for tests).
    void populateMenu(QMenu* menu);
    void populateTabMenu(QMenu* menu, int index);

    // --- Plugins (docs/PLUGIN_DEV.md) ----------------------------------------------

    // How to reload one; without these, Reload is greyed out.
    struct PluginHooks {
        std::function<std::string(const std::string& id)> whyNotReloadable;
        std::function<PluginManager::ReloadResult(const std::string& id)> reload;
    };
    void setPluginHooks(PluginHooks hooks);

    // Opens the plugin's tab, or goes to the one that's open. `mainFile`, if any, is
    // opened in it the first time.
    void openPlugin(const QString& id, const QString& name, const QString& folder, const QString& mainFile = {});
    // Saves its open files, reloads it, and says how it went in the output. False if
    // it isn't running after that.
    bool reloadPlugin(const QString& id);

    // About the current tab, for it being a plugin: its id, or empty.
    QString currentPluginId() const;
    // Its tree, and its own tab bar. Null if it isn't a plugin's tab.
    QTreeView* pluginTree() const;
    QTabBar* pluginTabBar() const;
    // As a click (a preview, which the next one replaces) or a double-click in the
    // tree. False for a file that can't be shown as text.
    bool openInPlugin(const QString& path, bool preview);
    // Whether the current file on the plugin's tab is still only a preview.
    bool isPreview() const;

    // What the tree's context menu does, on the current plugin's tab. They ask nothing:
    // that's the menu's part. New ones are opened. A rename or delete follows through
    // to any open tab; a delete goes to the Recycle Bin.
    bool newPluginFile(const QString& folder, const QString& name);
    bool newPluginFolder(const QString& folder, const QString& name);
    bool renamePluginPath(const QString& path, const QString& name);
    bool deletePluginPath(const QString& path);

Q_SIGNALS:
    // A reload has run, whichever way it went.
    void pluginReloaded(const QString& id);

private:
    struct TopTab;
    struct Script;
    struct Project;

    TopTab* topTab(int index) const;
    Project* project(int index) const;
    // The script of a top-level tab: for a plugin, its current file.
    Script* script(int index) const;
    Script* scriptIn(QTabBar* bar, int index) const;
    int indexOf(const Script* script) const;
    int indexIn(const QTabBar* bar, const void* tab) const;
    QTabBar* barOf(const Script* script) const;
    Project* projectById(const QString& id) const;
    void showScript(const Script* script);

    Script* addScript(const QString& file, const QString& name, const QString& text, bool modified, Project* project = nullptr);
    Project* addProject(const QString& id, const QString& name, const QString& folder);
    bool closeScript(Script* script);
    bool closeProject(Project* project);
    void updateProjectPage(Project* project);
    void showTreeMenu(Project* project, const QPoint& position);
    void updateHighlighter(Script* script);
    bool saveScript(Script* script);
    bool saveScriptAs(Script* script, const QString& path);
    void clearScript(const Script* script);
    bool renameScript(Script* script, const QString& name);
    bool ruffFormatScript(Script* script);
    bool ruffFixScript(Script* script);
    bool ruffCheckScript(Script* script);
    void populateScriptMenu(QMenu* menu, Script* script);
    QString untitledName() const;
    QString displayName(const Script& script) const;
    bool runCode(const QString& code, const QString& name);
    // Records what's logged during `work`, and prints it to the output afterwards.
    void capturing(const std::function<void()>& work);
    // Unsaved changes: ask whether to save them first. False to cancel.
    bool maybeSave(Script* script);
    bool runRuff(const QStringList& arguments, const Script& script, QByteArray& out, QByteArray& err);
    void addRecentFile(const QString& path);
    void updateTab(const Script* script);
    // A page for a Python file or untitled script, the Plugins panel's Py chip for a
    // plugin, and none for other files.
    QIcon tabIcon(const TopTab* tab) const;
    void updateTabIcons();
    void updateRunButton();
    // The current tab's look: accent-filled while focus is in the panel.
    void updateFocusHighlight();
    void appendOutput(const QString& text, const QColor& color);
    void saveSession() const;
    void restoreSession();
    // Saves the session shortly after a change (typing, tabs added, closed,
    // moved, renamed), so the tabs survive the app being killed or crashing.
    void scheduleSave();
    void watchFiles();
    void checkChangedFiles();

    PythonRuntime& m_runtime;
    ScriptRunner& m_runner;
    QSettings* m_settings = nullptr;
    QTabBar* m_tabBar = nullptr;
    QStackedWidget* m_editors = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QSplitter* m_splitter = nullptr;
    QAction* m_keepVariables = nullptr;
    QToolButton* m_runButton = nullptr;
    // Every file and untitled script, in any tab.
    std::vector<Script*> m_scripts;
    std::vector<Project*> m_projects;
    QStringList m_recentFiles;
    SavePrompt m_savePrompt;
    DiskChangePrompt m_diskPrompt;
    PluginHooks m_pluginHooks;
    QTimer* m_saveTimer = nullptr;
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer* m_diskTimer = nullptr;
    QSet<QString> m_changedFiles;
    bool m_focused = false;
};

}  // namespace usdcc::scripting
