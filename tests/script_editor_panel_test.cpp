// The Script Editor panel (docs/SCRIPTING.md phase 3): PythonHighlighter,
// CodeEditor's indentation and comments, and ScriptEditorSidePanel's runs,
// output, kept variables, files and persistence. Runs offscreen in
// usdcc_python_tests.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/code_editor.h"
#include "usdcc/scripting/python_highlighter.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_editor_side_panel.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QTabBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <cstdio>

using usdcc::scripting::CodeEditor;
using usdcc::scripting::PythonHighlighter;
using usdcc::scripting::ScriptEditorSidePanel;
using usdcc::test::pythonTestApp;

namespace {
QTextCharFormat formatAt(QTextDocument& document, int line, int column) {
    const QTextBlock block = document.findBlockByNumber(line);
    for (const QTextLayout::FormatRange& range : block.layout()->formats()) {
        if (column >= range.start && column < range.start + range.length) return range.format;
    }
    return {};
}

bool sameColor(const QTextCharFormat& a, const QTextCharFormat& b) {
    return a.foreground().color() == b.foreground().color();
}

void press(QWidget* widget, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(widget, &event);
}

// Replaces the text as typing would: unlike setPlainText(), this marks the
// document modified.
void type(CodeEditor& editor, const QString& text) {
    editor.selectAll();
    editor.insertPlainText(text);
}

void putCursor(CodeEditor& editor, int line, int column) {
    QTextCursor cursor(editor.document()->findBlockByNumber(line));
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);
    editor.setTextCursor(cursor);
}

void selectLines(CodeEditor& editor, int first, int last) {
    QTextCursor cursor(editor.document()->findBlockByNumber(first));
    cursor.setPosition(editor.document()->findBlockByNumber(last).position() +
                           editor.document()->findBlockByNumber(last).length() - 1,
                       QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
}

struct ScratchFiles {
    QString dir = QDir::temp().filePath(QStringLiteral("usdcc_scripting_panel_%1").arg(QApplication::applicationPid()));
    ScratchFiles() { QDir().mkpath(dir); }
    ~ScratchFiles() { QDir(dir).removeRecursively(); }
    QString path(const QString& name) const { return QDir(dir).filePath(name); }
};
}  // namespace

USDCC_TEST(python_highlighter) {
    QTextDocument document;
    PythonHighlighter highlighter(&document);
    document.setPlainText(
        "def compute(x):  # note\n"
        "    s = 'a # not a comment'\n"
        "    \"\"\"doc\n"
        "    still doc\"\"\"\n"
        "    return 42\n"
        "@decorated\n");
    // A highlighter attached to a document does its first pass from the event
    // loop; do it now.
    highlighter.rehighlight();
    using Kind = PythonHighlighter::Kind;
    CHECK(sameColor(formatAt(document, 0, 0), highlighter.format(Kind::Keyword)), "def is a keyword");
    CHECK(formatAt(document, 0, 4).fontWeight() == QFont::Bold, "the def's name");
    CHECK(sameColor(formatAt(document, 0, 18), highlighter.format(Kind::Comment)), "a comment");
    CHECK(sameColor(formatAt(document, 1, 12), highlighter.format(Kind::String)), "'#' inside a string stays string");
    CHECK(sameColor(formatAt(document, 3, 6), highlighter.format(Kind::String)),
          "a triple-quoted string carries over to the next line");
    CHECK(sameColor(formatAt(document, 4, 4), highlighter.format(Kind::Keyword)) &&
              sameColor(formatAt(document, 4, 11), highlighter.format(Kind::Number)),
          "keywords and numbers after the string ended");
    CHECK(sameColor(formatAt(document, 5, 1), highlighter.format(Kind::Decorator)), "a decorator");
}

USDCC_TEST(code_editor) {
    CodeEditor editor;
    editor.setPlainText("if ready:");
    putCursor(editor, 0, 9);
    press(&editor, Qt::Key_Return);
    editor.insertPlainText("go()");
    press(&editor, Qt::Key_Return);
    CHECK(editor.toPlainText() == "if ready:\n    go()\n    ", "Enter indents after ':' and keeps it: [%s]",
          qPrintable(editor.toPlainText()));

    editor.setPlainText("a = 1\nb = 2");
    selectLines(editor, 0, 1);
    press(&editor, Qt::Key_Tab);
    CHECK(editor.toPlainText() == "    a = 1\n    b = 2", "Tab indents the selected lines: [%s]",
          qPrintable(editor.toPlainText()));
    CHECK(editor.selectedCode() == "a = 1\nb = 2\n", "selectedCode() drops the common indentation: [%s]",
          qPrintable(editor.selectedCode()));
    press(&editor, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(editor.toPlainText() == "a = 1\nb = 2", "Shift+Tab dedents them");

    selectLines(editor, 0, 1);
    press(&editor, Qt::Key_Slash, Qt::ControlModifier);
    CHECK(editor.toPlainText() == "# a = 1\n# b = 2", "Ctrl+/ comments: [%s]", qPrintable(editor.toPlainText()));
    press(&editor, Qt::Key_Slash, Qt::ControlModifier);
    CHECK(editor.toPlainText() == "a = 1\nb = 2", "and uncomments");

    putCursor(editor, 1, 0);
    CHECK(editor.selectedCode() == "b = 2\n", "without a selection: the cursor's line");
}

USDCC_TEST(scripting_panel_runs) {
    ScratchFiles files;
    auto& app = pythonTestApp();
    ScriptEditorSidePanel panel(app.python, app.runner, nullptr, files.path("settings.ini"));
    CHECK(panel.editor() != nullptr, "an editor while Python runs");
    if (!panel.editor()) return;

    panel.editor()->setPlainText("print('hello from the panel')\n");
    CHECK(panel.run(), "a script runs");
    QString out = panel.output()->toPlainText();
    CHECK(out.contains(">>> Run Untitled") && out.contains("hello from the panel") && out.contains("done in"),
          "header, output and footer: [%s]", qPrintable(out));

    panel.clearOutput();
    panel.editor()->setPlainText("def boom():\n    raise ValueError('boom')\nboom()\n");
    CHECK(!panel.run(), "a raising script fails");
    out = panel.output()->toPlainText();
    CHECK(out.contains("Traceback") && out.contains("ValueError: boom") && out.contains("failed after"),
          "the traceback is in the output: [%s]", qPrintable(out));

    panel.clearOutput();
    panel.editor()->setPlainText("print('first')\nprint('second')\n");
    putCursor(*panel.editor(), 1, 0);
    CHECK(panel.runSelection(), "Run Selection runs");
    out = panel.output()->toPlainText();
    CHECK(out.contains("second") && !out.contains("first\n") && out.contains("(selection)"),
          "only the cursor's line: [%s]", qPrintable(out));

    panel.setKeepVariables(true);
    panel.editor()->setPlainText("kept = 41\n");
    panel.run();
    panel.clearOutput();
    panel.editor()->setPlainText("print(kept + 1)\n");
    CHECK(panel.run() && panel.output()->toPlainText().contains("42"), "variables kept between runs");
    panel.setKeepVariables(false);
    CHECK(!panel.run(), "and forgotten once that's off");
}

USDCC_TEST(scripting_panel_files) {
    ScratchFiles files;
    auto& app = pythonTestApp();
    const QString settings = files.path("settings.ini");
    const QString script = files.path("shot_setup.py");
    const QString path = QFileInfo(script).absoluteFilePath();
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        QTabBar* tabs = panel.tabBar();
        CHECK(tabs && tabs->count() == 1 && tabs->tabText(0) == "Untitled", "starts with one untitled tab");
        type(*panel.editor(), "x = 1\n");
        CHECK(panel.isModified() && tabs->tabText(0) == QString::fromUtf8("Untitled •"), "an unsaved new script: [%s]",
              qPrintable(tabs->tabText(0)));

        CHECK(panel.saveAs(script), "Save As");
        CHECK(!panel.isModified() && panel.currentFile() == path && tabs->tabText(0) == "shot_setup.py",
              "names the tab after the file, nothing unsaved: [%s]", qPrintable(tabs->tabText(0)));
        CHECK(!panel.recentFiles().isEmpty() && panel.recentFiles().front() == path, "a recent file");

        panel.newScript();
        type(*panel.editor(), "draft = True\n");
        CHECK(panel.tabCount() == 2 && panel.currentTab() == 1, "New opens a second tab");
        panel.setCurrentTab(0);
        type(*panel.editor(), "x = 2  # not saved yet\n");

        // Leaving: the file's changes are asked about; the untitled draft isn't.
        QStringList asked;
        panel.setSavePrompt([&](const QString& name) {
            asked << name;
            return QMessageBox::Cancel;
        });
        CHECK(!panel.canClose() && asked == QStringList{"shot_setup.py"}, "Cancel keeps the app open: [%s]",
              qPrintable(asked.join(',')));
        panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
        CHECK(panel.canClose() && panel.editor()->toPlainText() == "x = 1\n" && !panel.isModified(),
              "Discard goes back to the file");
        type(*panel.editor(), "x = 3  # unsaved at a crash\n");
    }
    {
        // Both tabs come back: the untitled draft's text, and the file with the
        // unsaved changes it had when the panel went away without asking.
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        CHECK(panel.tabCount() == 2, "two tabs restored: %d", panel.tabCount());
        CHECK(panel.currentTab() == 0 && panel.currentFile() == path &&
                  panel.editor()->toPlainText() == "x = 3  # unsaved at a crash\n" && panel.isModified(),
              "the file, unsaved text and all: [%s]", qPrintable(panel.editor()->toPlainText()));
        panel.setCurrentTab(1);
        CHECK(panel.currentFile().isEmpty() && panel.editor()->toPlainText() == "draft = True\n" && panel.isModified(),
              "the untitled draft");

        panel.setSavePrompt([](const QString&) { return QMessageBox::Save; });
        CHECK(panel.canClose() && QFileInfo(script).size() > 0, "Save writes the file");
    }
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        CHECK(!panel.isModified() && panel.editor()->toPlainText() == "x = 3  # unsaved at a crash\n",
              "a clean file reopens from disk");
        CHECK(panel.openFile(script) && panel.tabCount() == 2 && panel.currentTab() == 0,
              "opening an open file switches to its tab");
    }
}

USDCC_TEST(scripting_panel_untitled_persist) {
    // An untitled tab, renamed and typed into, comes back with its text.
    ScratchFiles files;
    auto& app = pythonTestApp();
    const QString settings = files.path("settings.ini");
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        panel.show();
        CHECK(panel.renameTab(0, "Ttest"), "rename");
        panel.editor()->setFocus();
        for (QChar c : QStringLiteral("print('kept')")) {
            QKeyEvent event(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
            QApplication::sendEvent(panel.editor(), &event);
        }
        panel.newScript();
        panel.setCurrentTab(0);

        // Saved a moment after typing, while the panel is still alive: what is
        // on disk if the app is killed or crashes now.
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < 1000) QApplication::processEvents(QEventLoop::AllEvents, 50);
        QSettings saved(settings, QSettings::IniFormat);
        CHECK(saved.value("ScriptEditor/tabs/size").toInt() == 2 &&
                  saved.value("ScriptEditor/tabs/1/name").toString() == "Ttest" &&
                  saved.value("ScriptEditor/tabs/1/text").toString() == "print('kept')",
              "saved before exit: [%s] [%s]", qPrintable(saved.value("ScriptEditor/tabs/1/name").toString()),
              qPrintable(saved.value("ScriptEditor/tabs/1/text").toString()));
        CHECK(panel.canClose(), "nothing to ask about");
    }
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        CHECK(panel.tabCount() == 2 && panel.tabBar()->tabText(0) == QString::fromUtf8("Ttest •") &&
                  panel.editor()->toPlainText() == "print('kept')",
              "restored: %d tabs, [%s] [%s]", panel.tabCount(), qPrintable(panel.tabBar()->tabText(0)),
              qPrintable(panel.editor()->toPlainText()));
    }
}

USDCC_TEST(scripting_panel_tabs) {
    ScratchFiles files;
    auto& app = pythonTestApp();
    ScriptEditorSidePanel panel(app.python, app.runner, nullptr, files.path("settings.ini"));
    panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
    QTabBar* tabs = panel.tabBar();

    // Open reuses the empty untitled tab a fresh start shows.
    const QString script = files.path("rig.py");
    {
        QFile file(script);
        CHECK(file.open(QIODevice::WriteOnly) && file.write("print('rig')\n") > 0, "write rig.py");
    }
    CHECK(panel.openFile(script) && panel.tabCount() == 1 && tabs->tabText(0) == "rig.py", "Open into the empty tab");

    panel.duplicateTab(0);
    CHECK(panel.tabCount() == 2 && panel.currentTab() == 1 && tabs->tabText(1) == QString::fromUtf8("rig copy •") &&
              panel.editor()->toPlainText() == "print('rig')\n" && panel.currentFile().isEmpty(),
          "Duplicate: an untitled copy beside it: [%s]", qPrintable(tabs->tabText(1)));

    CHECK(panel.renameTab(1, "rig_v2") && tabs->tabText(1) == QString::fromUtf8("rig_v2 •"),
          "renaming an untitled tab names it");
    CHECK(panel.renameTab(0, "rigging") && QFileInfo::exists(files.path("rigging.py")) && !QFileInfo::exists(script) &&
              tabs->tabText(0) == "rigging.py",
          "renaming a file renames it on disk, adding .py");

    panel.clearTab(1);
    CHECK(panel.editor()->toPlainText().isEmpty(), "Clear empties the tab");
    panel.editor()->undo();
    CHECK(panel.editor()->toPlainText() == "print('rig')\n", "in one undo step");

    QMenu menu;
    panel.populateTabMenu(&menu, 0);
    QStringList actions;
    for (QAction* action : menu.actions()) {
        if (!action->isSeparator()) actions << action->text().section('\t', 0, 0);
    }
    CHECK(actions == QStringList({"Save", "Save As...", "Rename...", "Duplicate", "Clear", "Ruff Format", "Ruff Check",
                                  "Ruff Fix", "Close", "Close Others"}),
          "the tab's context menu: [%s]", qPrintable(actions.join(", ")));

    CHECK(panel.closeTab(1) && panel.tabCount() == 1, "Close");
    CHECK(panel.closeTab(0) && panel.tabCount() == 1 && panel.currentFile().isEmpty() && panel.editor()->toPlainText().isEmpty(),
          "closing the last tab leaves an empty untitled one");

    // Ruff, where it's installed.
    if (panel.ruffProgram().isEmpty()) {
        std::printf("    (ruff not found: Ruff Format and Ruff Check not tested)\n");
        return;
    }
    type(*panel.editor(), "x=[1,2 ,3]\nimport os\n");
    CHECK(panel.ruffFormat(0) && panel.editor()->toPlainText() == "x = [1, 2, 3]\nimport os\n",
          "Ruff Format: [%s]", qPrintable(panel.editor()->toPlainText()));
    panel.editor()->undo();
    CHECK(panel.editor()->toPlainText() == "x=[1,2 ,3]\nimport os\n", "in one undo step");
    panel.clearOutput();
    CHECK(panel.ruffCheck(0) && panel.output()->toPlainText().contains("F401"), "Ruff Check prints findings: [%s]",
          qPrintable(panel.output()->toPlainText()));

    // The unused import goes; the undefined name can't be fixed and is listed.
    type(*panel.editor(), "import os\nprint(undefined_name)\n");
    panel.clearOutput();
    CHECK(panel.ruffFix(0) && panel.editor()->toPlainText() == "print(undefined_name)\n", "Ruff Fix: [%s]",
          qPrintable(panel.editor()->toPlainText()));
    CHECK(panel.output()->toPlainText().contains("F821"), "and prints what's left: [%s]",
          qPrintable(panel.output()->toPlainText()));
    panel.editor()->undo();
    CHECK(panel.editor()->toPlainText() == "import os\nprint(undefined_name)\n", "in one undo step");
}

USDCC_TEST(scripting_panel_unavailable) {
    ScratchFiles files;
    // A second runtime can't start Python, so it reports itself unavailable.
    usdcc::scripting::PythonRuntime unavailable;
    ScriptEditorSidePanel panel(unavailable, pythonTestApp().runner, nullptr, files.path("settings.ini"));
    auto* reason = panel.findChild<QLabel*>(QStringLiteral("scriptingUnavailable"));
    CHECK(!panel.editor() && reason && reason->text().contains("unavailable"), "says why instead of an editor: %s",
          reason ? qPrintable(reason->text()) : "?");
    CHECK(!panel.run(), "and runs nothing");
}
