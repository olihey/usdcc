#include "usdcc/scripting/code_editor.h"

#include <QKeyEvent>
#include <QTextBlock>

#include <algorithm>
#include <limits>

namespace usdcc::scripting {

namespace {
int leadingSpaces(const QString& line) {
    int count = 0;
    while (count < line.size() && line.at(count) == u' ') ++count;
    return count;
}
}  // namespace

CodeEditor::CodeEditor(QWidget* parent) : usdcc::ui::TextView(parent) {
    setObjectName(QStringLiteral("scriptEditor"));  // monospace, from theme/dark.qss
    setTabChangesFocus(false);
}

std::pair<int, int> CodeEditor::selectedLines() const {
    const QTextCursor cursor = textCursor();
    QTextCursor start(document());
    start.setPosition(cursor.selectionStart());
    QTextCursor end(document());
    end.setPosition(cursor.selectionEnd());
    int last = end.blockNumber();
    // A selection ending at a line's very start doesn't include that line.
    if (cursor.hasSelection() && end.positionInBlock() == 0 && last > start.blockNumber()) --last;
    return {start.blockNumber(), last};
}

QString CodeEditor::selectedCode() const {
    const auto [first, last] = selectedLines();
    QStringList lines;
    int common = std::numeric_limits<int>::max();
    for (int number = first; number <= last; ++number) {
        const QString line = document()->findBlockByNumber(number).text();
        lines << line;
        if (!line.trimmed().isEmpty()) common = std::min(common, leadingSpaces(line));
    }
    if (common == std::numeric_limits<int>::max()) common = 0;
    for (QString& line : lines) line = line.mid(std::min(common, leadingSpaces(line)));
    return lines.join(u'\n') + u'\n';
}

void CodeEditor::indentSelection() {
    const auto [first, last] = selectedLines();
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    for (int number = first; number <= last; ++number) {
        QTextCursor line(document()->findBlockByNumber(number));
        line.insertText(QString(kIndent, u' '));
    }
    cursor.endEditBlock();
}

void CodeEditor::dedentSelection() {
    const auto [first, last] = selectedLines();
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    for (int number = first; number <= last; ++number) {
        const QTextBlock block = document()->findBlockByNumber(number);
        const int remove = std::min(kIndent, leadingSpaces(block.text()));
        if (remove == 0) continue;
        QTextCursor line(block);
        line.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, remove);
        line.removeSelectedText();
    }
    cursor.endEditBlock();
}

void CodeEditor::toggleComment() {
    const auto [first, last] = selectedLines();
    // Uncomment if every non-empty line is a comment, else comment them all,
    // at the lines' smallest indentation.
    bool allCommented = true;
    int indent = std::numeric_limits<int>::max();
    for (int number = first; number <= last; ++number) {
        const QString line = document()->findBlockByNumber(number).text();
        if (line.trimmed().isEmpty()) continue;
        indent = std::min(indent, leadingSpaces(line));
        allCommented = allCommented && line.trimmed().startsWith(u'#');
    }
    if (indent == std::numeric_limits<int>::max()) return;

    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    for (int number = first; number <= last; ++number) {
        const QTextBlock block = document()->findBlockByNumber(number);
        const QString line = block.text();
        if (line.trimmed().isEmpty()) continue;
        QTextCursor edit(block);
        if (allCommented) {
            const int hash = static_cast<int>(line.indexOf(u'#'));
            edit.setPosition(block.position() + hash);
            edit.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, line.mid(hash, 2) == QStringLiteral("# ") ? 2 : 1);
            edit.removeSelectedText();
        } else {
            edit.setPosition(block.position() + indent);
            edit.insertText(QStringLiteral("# "));
        }
    }
    cursor.endEditBlock();
}

void CodeEditor::insertNewline() {
    QTextCursor cursor = textCursor();
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    int indent = leadingSpaces(before);
    if (before.trimmed().endsWith(u':')) indent += kIndent;
    cursor.insertText(u'\n' + QString(indent, u' '));
    setTextCursor(cursor);
}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    const bool multiLine = textCursor().hasSelection() && selectedLines().first != selectedLines().second;

    if (event->key() == Qt::Key_Tab && modifiers == Qt::NoModifier) {
        if (multiLine) {
            indentSelection();
        } else {
            const int column = textCursor().positionInBlock();
            insertPlainText(QString(kIndent - column % kIndent, u' '));
        }
        return;
    }
    if (event->key() == Qt::Key_Backtab) {
        dedentSelection();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && modifiers == Qt::NoModifier) {
        insertNewline();
        return;
    }
    if (event->key() == Qt::Key_Slash && modifiers == Qt::ControlModifier) {
        toggleComment();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

}  // namespace usdcc::scripting
