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
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

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
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        auto* label = panel.findChild<QLabel*>(QStringLiteral("scriptFileLabel"));
        type(*panel.editor(), "x = 1\n");
        CHECK(panel.isModified() && label && label->text() == QString::fromUtf8("Untitled •"),
              "an unsaved new script: [%s]", label ? qPrintable(label->text()) : "?");

        CHECK(panel.saveAs(script), "Save As");
        CHECK(!panel.isModified() && panel.currentFile() == QFileInfo(script).absoluteFilePath() &&
                  label->text() == "shot_setup.py",
              "names the file, nothing unsaved: [%s]", qPrintable(label->text()));
        CHECK(!panel.recentFiles().isEmpty() && panel.recentFiles().front() == panel.currentFile(), "a recent file");

        type(*panel.editor(), "x = 2  # not saved yet\n");
    }
    {
        // Unsaved changes come back after a restart, with the file still open.
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        CHECK(panel.editor()->toPlainText() == "x = 2  # not saved yet\n" && panel.isModified(),
              "unsaved text restored: [%s]", qPrintable(panel.editor()->toPlainText()));
        CHECK(panel.currentFile() == QFileInfo(script).absoluteFilePath(), "with its file");
        panel.editor()->document()->setModified(false);  // discard, without the dialog
        CHECK(panel.openFile(script) && panel.editor()->toPlainText() == "x = 1\n", "Open reads the saved file");
    }
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, settings);
        CHECK(!panel.isModified() && panel.editor()->toPlainText() == "x = 1\n", "a clean file reopens from disk");
    }
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
