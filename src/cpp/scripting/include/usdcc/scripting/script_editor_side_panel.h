#pragma once

#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/side_panel.h"

#include <QMessageBox>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QAction;
class QMenu;
class QPlainTextEdit;
class QSettings;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;

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
// ☰ menu; Clear, Rename, Save, Save As, Duplicate, Ruff Format,
// Ruff Check and Close in each tab's context menu.
//
// The open tabs, untitled scripts' text, the recent files, the split and the
// ☰ menu's Keep Variables Between Runs persist; the tabs are saved a moment
// after each change, not only on exit. Closing a tab, or the app,
// with unsaved changes to a file asks whether to save them; untitled
// scripts come back as they were without asking. When Python couldn't
// start, the panel shows why instead of the editor.
class ScriptEditorSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    // `settingsFile` is for tests; empty means the app's settings.
    ScriptEditorSidePanel(PythonRuntime& runtime, ScriptRunner& runner, QWidget* parent = nullptr,
                          const QString& settingsFile = {});
    ~ScriptEditorSidePanel() override;

    // The current tab's editor; null when Python is unavailable.
    CodeEditor* editor() const;
    QPlainTextEdit* output() const { return m_output; }
    QTabBar* tabBar() const { return m_tabBar; }

    // Runs the current tab's whole text, or its selection (Run Selection:
    // the cursor's line without one). Return whether the script ran without
    // an exception.
    bool run();
    bool runSelection();

    // The current tab's script.
    const QString& currentFile() const;
    QString currentName() const;
    bool isModified() const;

    int tabCount() const { return static_cast<int>(m_scripts.size()); }
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
    bool saveTab(int index);
    bool saveTabAs(int index, const QString& path = {});
    void clearTab(int index);
    bool renameTab(int index, const QString& name = {});  // asks for the name when empty
    bool closeTab(int index);

    // Ruff (https://docs.astral.sh/ruff/), found on PATH or in the Python's
    // Scripts folder. Format replaces the tab's text with `ruff format`'s
    // (one undo step); Check prints `ruff check`'s findings to the output.
    // Both use the ruff settings of the file's folder. Return false when ruff
    // is missing or fails.
    QString ruffProgram() const;
    bool ruffFormat(int index);
    bool ruffCheck(int index);

    void clearOutput();
    bool keepsVariables() const;
    void setKeepVariables(bool keep);
    const QStringList& recentFiles() const { return m_recentFiles; }

    // Asks about each file with unsaved changes (SidePanel::canClose()).
    bool canClose() override;

    // Asks whether to save `name`'s changes: Save, Discard or Cancel. A
    // message box unless set (tests answer without one).
    using SavePrompt = std::function<QMessageBox::StandardButton(const QString& name)>;
    void setSavePrompt(SavePrompt prompt) { m_savePrompt = std::move(prompt); }

    // Fill the ☰ menu and a tab's context menu (public for tests).
    void populateMenu(QMenu* menu);
    void populateTabMenu(QMenu* menu, int index);

private:
    struct Script;

    Script* script(int index) const;
    int indexOf(const Script* script) const;
    Script* addScript(const QString& file, const QString& name, const QString& text, bool modified);
    QString untitledName() const;
    QString displayName(const Script& script) const;
    bool runCode(const QString& code, const QString& name);
    // Unsaved changes: ask whether to save them first. False to cancel.
    bool maybeSave(int index);
    bool runRuff(const QStringList& arguments, const Script& script, QByteArray& out, QByteArray& err);
    void addRecentFile(const QString& path);
    void updateTab(const Script* script);
    // The current tab's look: accent-filled while focus is in the panel.
    void updateFocusHighlight();
    void appendOutput(const QString& text, const QColor& color);
    void saveSession() const;
    void restoreSession();
    // Saves the session shortly after a change (typing, tabs added, closed,
    // moved, renamed), so the tabs survive the app being killed or crashing.
    void scheduleSave();

    PythonRuntime& m_runtime;
    ScriptRunner& m_runner;
    QSettings* m_settings = nullptr;
    QTabBar* m_tabBar = nullptr;
    QStackedWidget* m_editors = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QSplitter* m_splitter = nullptr;
    QAction* m_keepVariables = nullptr;
    // In tab order.
    std::vector<Script*> m_scripts;
    QStringList m_recentFiles;
    SavePrompt m_savePrompt;
    QTimer* m_saveTimer = nullptr;
    bool m_focused = false;
};

}  // namespace usdcc::scripting
