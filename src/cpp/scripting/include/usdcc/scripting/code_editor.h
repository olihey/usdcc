#pragma once

#include <QPlainTextEdit>

#include <utility>

namespace usdcc::scripting {

// The Script Editor panel's text editor (docs/SCRIPTING.md §7): a monospace
// QPlainTextEdit (font family from theme/dark.qss, #scriptEditor) with
//   - a line-number gutter and the cursor's line highlighted;
//   - indentation in spaces, 4 per level: Tab and Shift+Tab indent and
//     dedent the selected lines (or insert up to the next tab stop);
//   - Enter keeps the line's indentation, one level more after a ':';
//   - Ctrl+/ toggles '# ' comments on the selected lines.
// Ctrl+Enter is left alone for the panel's Run shortcut.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    static constexpr int kIndent = 4;

    explicit CodeEditor(QWidget* parent = nullptr);

    // What Run Selection runs: the selected lines, or the cursor's line when
    // nothing is selected, with their common indentation removed.
    QString selectedCode() const;

    void indentSelection();
    void dedentSelection();
    void toggleComment();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    friend class LineNumberArea;
    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent* event);
    void updateLineNumberMargin();
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightCurrentLine();
    void insertNewline();
    // The first and last block numbers the selection (or the cursor) covers.
    std::pair<int, int> selectedLines() const;

    QWidget* m_lineNumbers;
};

}  // namespace usdcc::scripting
