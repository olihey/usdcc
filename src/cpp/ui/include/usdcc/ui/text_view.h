#pragma once

#include "usdcc/core/export.h"

#include <QPlainTextEdit>

namespace usdcc::ui {

// A monospace QPlainTextEdit for code (font family from theme/dark.qss,
// #codeView, or a subclass's own object name), without wrapping, with
//   - a line-number gutter, painting only the lines on screen;
//   - the cursor's line highlighted.
// The Script Editor's CodeEditor is built on it. For very big read-only text
// see LineView, whose gutter and current line look the same.
class USDCC_UI_API TextView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit TextView(QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    friend class LineNumberArea;
    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent* event);
    void updateLineNumberMargin();
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightCurrentLine();

    QWidget* m_lineNumbers;
};

}  // namespace usdcc::ui
