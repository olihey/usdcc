#include "usdcc/scripting/code_editor.h"

#include <QKeyEvent>
#include <QPainter>
#include <QTextBlock>

#include <algorithm>
#include <limits>

namespace usdcc::scripting {

// The gutter left of the text; paints through the editor.
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor* editor) : QWidget(editor), m_editor(editor) {}
    QSize sizeHint() const override { return {m_editor->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { m_editor->paintLineNumbers(event); }

private:
    CodeEditor* m_editor;
};

namespace {
int leadingSpaces(const QString& line) {
    int count = 0;
    while (count < line.size() && line.at(count) == u' ') ++count;
    return count;
}
}  // namespace

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent), m_lineNumbers(new LineNumberArea(this)) {
    setObjectName(QStringLiteral("scriptEditor"));  // monospace, from theme/dark.qss
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabChangesFocus(false);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberMargin);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);
    updateLineNumberMargin();
    highlightCurrentLine();
}

int CodeEditor::lineNumberAreaWidth() const {
    const int digits = std::max(3, static_cast<int>(QString::number(std::max(1, blockCount())).size()));
    return fontMetrics().horizontalAdvance(u'9') * digits + fontMetrics().horizontalAdvance(u' ') * 2;
}

void CodeEditor::updateLineNumberMargin() { setViewportMargins(lineNumberAreaWidth(), 0, 0, 0); }

void CodeEditor::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy) {
        m_lineNumbers->scroll(0, dy);
    } else {
        m_lineNumbers->update(0, rect.y(), m_lineNumbers->width(), rect.height());
    }
    if (rect.contains(viewport()->rect())) updateLineNumberMargin();
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    m_lineNumbers->setGeometry(QRect(area.left(), area.top(), lineNumberAreaWidth(), area.height()));
}

void CodeEditor::paintLineNumbers(QPaintEvent* event) {
    // The theme's header surface and muted text (resources/theme/dark.qss).
    QPainter painter(m_lineNumbers);
    painter.fillRect(event->rect(), QColor(0x16, 0x16, 0x18));
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    const int right = m_lineNumbers->width() - fontMetrics().horizontalAdvance(u' ');
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(block.blockNumber() == current ? QColor(255, 255, 255, 191) : QColor(255, 255, 255, 89));
            painter.drawText(0, top, right, fontMetrics().height(), Qt::AlignRight,
                             QString::number(block.blockNumber() + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
    }
}

void CodeEditor::highlightCurrentLine() {
    QTextEdit::ExtraSelection line;
    line.format.setBackground(QColor(255, 255, 255, 10));
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    setExtraSelections({line});
    m_lineNumbers->update();
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
