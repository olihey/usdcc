#include "usdcc/ui/text_view.h"

#include <QPainter>
#include <QTextBlock>

#include <algorithm>

namespace usdcc::ui {

// The gutter left of the text; paints through the view.
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(TextView* view) : QWidget(view), m_view(view) {}
    QSize sizeHint() const override { return {m_view->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { m_view->paintLineNumbers(event); }

private:
    TextView* m_view;
};

TextView::TextView(QWidget* parent) : QPlainTextEdit(parent), m_lineNumbers(new LineNumberArea(this)) {
    setObjectName(QStringLiteral("codeView"));  // monospace, from theme/dark.qss
    setLineWrapMode(QPlainTextEdit::NoWrap);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &TextView::updateLineNumberMargin);
    connect(this, &QPlainTextEdit::updateRequest, this, &TextView::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &TextView::highlightCurrentLine);
    updateLineNumberMargin();
    highlightCurrentLine();
}

int TextView::lineNumberAreaWidth() const {
    const int digits = std::max(3, static_cast<int>(QString::number(std::max(1, blockCount())).size()));
    return fontMetrics().horizontalAdvance(u'9') * digits + fontMetrics().horizontalAdvance(u' ') * 2;
}

void TextView::updateLineNumberMargin() { setViewportMargins(lineNumberAreaWidth(), 0, 0, 0); }

void TextView::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy) {
        m_lineNumbers->scroll(0, dy);
    } else {
        m_lineNumbers->update(0, rect.y(), m_lineNumbers->width(), rect.height());
    }
    if (rect.contains(viewport()->rect())) updateLineNumberMargin();
}

void TextView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    m_lineNumbers->setGeometry(QRect(area.left(), area.top(), lineNumberAreaWidth(), area.height()));
}

void TextView::paintLineNumbers(QPaintEvent* event) {
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

void TextView::highlightCurrentLine() {
    QTextEdit::ExtraSelection line;
    line.format.setBackground(QColor(255, 255, 255, 10));
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    setExtraSelections({line});
    m_lineNumbers->update();
}

}  // namespace usdcc::ui
