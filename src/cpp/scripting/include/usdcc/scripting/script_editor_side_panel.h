#pragma once

#include "usdcc/scripting/python_runtime.h"
#include "usdcc/ui/side_panel.h"

#include <QString>
#include <QStringList>

class QAction;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QSettings;
class QSplitter;

namespace usdcc::scripting {

class CodeEditor;
class ScriptRunner;

// The Script Editor panel (docs/SCRIPTING.md §7): a Python editor (CodeEditor,
// PythonHighlighter), Run (Ctrl+Enter) and Run Selection (Ctrl+Shift+Enter),
// and an output pane showing each run's header, its print output, warnings
// and tracebacks. Everything a run prints also goes to the Log panel; each
// run is one undo step per stage it changes (ScriptRunner).
//
// Files: New, Open, Save (Ctrl+S), Save As and Open Recent in the ☰ menu,
// with the open file's name (and • for unsaved changes) beside it. The open
// file, the recent files, unsaved text, the split and the ☰ menu's Keep
// Variables Between Runs persist. When Python couldn't start, the panel
// shows why instead of the editor.
class ScriptEditorSidePanel : public usdcc::ui::SidePanel {
    Q_OBJECT

public:
    // `settingsFile` is for tests; empty means the app's settings.
    ScriptEditorSidePanel(PythonRuntime& runtime, ScriptRunner& runner, QWidget* parent = nullptr,
                          const QString& settingsFile = {});
    ~ScriptEditorSidePanel() override;

    // Null when Python is unavailable.
    CodeEditor* editor() const { return m_editor; }
    QPlainTextEdit* output() const { return m_output; }

    // Runs the whole text, or the selection (Run Selection: the cursor's line
    // without one). Return whether the script ran without an exception.
    bool run();
    bool runSelection();

    void newScript();
    bool openFile(const QString& path);
    bool save();
    bool saveAs(const QString& path = {});  // asks for a path when empty
    void clearOutput();

    const QString& currentFile() const { return m_file; }
    bool isModified() const;
    bool keepsVariables() const;
    void setKeepVariables(bool keep);
    const QStringList& recentFiles() const { return m_recentFiles; }

    // Fills the ☰ menu (public for tests).
    void populateMenu(QMenu* menu);

private:
    bool runCode(const QString& code, const QString& name);
    // Unsaved changes: ask whether to save them first. False to cancel.
    bool maybeSave();
    void addRecentFile(const QString& path);
    void updateFileLabel();
    void appendOutput(const QString& text, const QColor& color);

    PythonRuntime& m_runtime;
    ScriptRunner& m_runner;
    QSettings* m_settings = nullptr;
    CodeEditor* m_editor = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QSplitter* m_splitter = nullptr;
    QLabel* m_fileLabel = nullptr;
    QAction* m_keepVariables = nullptr;
    QString m_file;
    QStringList m_recentFiles;
};

}  // namespace usdcc::scripting
