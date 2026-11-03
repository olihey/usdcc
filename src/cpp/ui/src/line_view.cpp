#include "usdcc/ui/line_view.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextLine>

#include <algorithm>
#include <cstring>

namespace usdcc::ui {

namespace {
constexpr int kTextMargin = 6;      // between the gutter and column 0
constexpr int kMaxLayouts = 4000;   // cached lines before the cache is cleared
const QColor kBackground(0x1c, 0x1c, 0x1e);
const QColor kGutter(0x16, 0x16, 0x18);
const QColor kCurrentLine(255, 255, 255, 10);
const QColor kSelection(0x3d, 0x8b, 0xfd, 90);

char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == u'_' || c == u':' || c == u'.'; }
}  // namespace

LineView::LineView(QWidget* parent) : QAbstractScrollArea(parent) {
    setObjectName(QStringLiteral("codeView"));  // monospace, from theme/dark.qss
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setText(std::string());
}

void LineView::setText(std::string utf8) {
    m_text = std::move(utf8);
    m_starts.clear();
    m_starts.reserve(m_text.size() / 40 + 1);
    m_starts.push_back(0);
    m_longestLine = 0;
    const char* data = m_text.data();
    const std::size_t size = m_text.size();
    std::size_t start = 0;
    // memchr is the fast part: a line index for 80 MB in tens of milliseconds.
    while (const void* found = std::memchr(data + start, '\n', size - start)) {
        const std::size_t end = static_cast<const char*>(found) - data;
        m_longestLine = std::max(m_longestLine, end - start);
        start = end + 1;
        m_starts.push_back(start);
    }
    m_longestLine = std::max(m_longestLine, size - start);
    m_anchor = m_cursor = Position();
    m_preferredColumn = -1;
    resetLayouts();
    m_formatted = 0;
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    updateScrollBars();
    viewport()->update();
}

void LineView::setLineFormatter(LineFormatter formatter) {
    m_formatter = std::move(formatter);
    resetLayouts();
    viewport()->update();
}

QString LineView::lineText(int line) const {
    if (line < 0 || line >= lineCount()) return {};
    const std::size_t start = m_starts[line];
    std::size_t end = line + 1 < lineCount() ? m_starts[line + 1] - 1 : m_text.size();
    if (end > start && m_text[end - 1] == '\r') --end;
    return QString::fromUtf8(m_text.data() + start, qsizetype(end - start));
}

void LineView::resetLayouts() { m_layouts.clear(); }

QTextLayout* LineView::layoutFor(int line) {
    if (auto it = m_layouts.find(line); it != m_layouts.end()) return it->second.get();
    if (int(m_layouts.size()) >= kMaxLayouts) m_layouts.clear();  // far from the screen by now
    auto layout = std::make_unique<QTextLayout>(lineText(line), font());
    layout->setCacheEnabled(true);
    if (m_formatter) layout->setFormats(m_formatter(line, layout->text()));
    layout->beginLayout();
    QTextLine textLine = layout->createLine();
    textLine.setLineWidth(1e7);  // never wraps
    textLine.setPosition(QPointF(0, 0));
    layout->endLayout();
    ++m_formatted;
    QTextLayout* result = layout.get();
    m_layouts.emplace(line, std::move(layout));
    return result;
}

int LineView::lineHeight() const { return std::max(1, fontMetrics().height()); }

int LineView::gutterWidth() const {
    const int digits = std::max(3, int(QString::number(std::max(1, lineCount())).size()));
    return fontMetrics().horizontalAdvance(u'9') * digits + fontMetrics().horizontalAdvance(u' ') * 2;
}

int LineView::textLeft() const { return gutterWidth() + kTextMargin - horizontalScrollBar()->value(); }

int LineView::visibleLines() const { return std::max(1, viewport()->height() / lineHeight()); }

int LineView::firstVisibleLine() const { return verticalScrollBar()->value(); }

void LineView::updateScrollBars() {
    const int visible = visibleLines();
    verticalScrollBar()->setRange(0, std::max(0, lineCount() - visible));
    verticalScrollBar()->setPageStep(visible);
    verticalScrollBar()->setSingleStep(1);
    // Monospace: the longest line in bytes is near enough its width in columns.
    const int textWidth = viewport()->width() - gutterWidth() - kTextMargin;
    const qint64 contentWidth = qint64(m_longestLine) * fontMetrics().horizontalAdvance(u'9') + kTextMargin * 2;
    horizontalScrollBar()->setRange(0, int(std::clamp<qint64>(contentWidth - textWidth, 0, 1 << 30)));
    horizontalScrollBar()->setPageStep(std::max(1, textWidth));
    horizontalScrollBar()->setSingleStep(fontMetrics().horizontalAdvance(u'9') * 4);
}

void LineView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void LineView::scrollContentsBy(int, int) { viewport()->update(); }

void LineView::changeEvent(QEvent* event) {
    QAbstractScrollArea::changeEvent(event);
    // The theme's font, or the UI scale changing it: lay out again.
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        resetLayouts();
        updateScrollBars();
        viewport()->update();
    }
}

void LineView::focusInEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void LineView::focusOutEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

void LineView::paintEvent(QPaintEvent* event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), kBackground);
    const int height = lineHeight();
    const int gutter = gutterWidth();
    const int left = textLeft();
    const int first = firstVisibleLine();
    const int last = std::min(lineCount() - 1, first + viewport()->height() / height + 1);
    const Position start = std::min(m_anchor, m_cursor);
    const Position end = std::max(m_anchor, m_cursor);
    QTextCharFormat selectionFormat;
    selectionFormat.setBackground(kSelection);

    painter.setClipRect(QRect(gutter, 0, viewport()->width() - gutter, viewport()->height()));
    for (int line = first; line <= last; ++line) {
        const int y = (line - first) * height;
        if (line == m_cursor.line) painter.fillRect(QRect(gutter, y, viewport()->width(), height), kCurrentLine);
        QTextLayout* layout = layoutFor(line);
        QList<QTextLayout::FormatRange> selections;
        if (hasSelection() && line >= start.line && line <= end.line) {
            const int from = line == start.line ? start.column : 0;
            const int to = line == end.line ? end.column : int(layout->text().size());
            if (to > from) selections.append({from, to - from, selectionFormat});
            // The line break of a selected line: a sliver past its end.
            if (line < end.line) {
                const qreal x = left + layout->lineAt(0).cursorToX(int(layout->text().size()));
                painter.fillRect(QRectF(x, y, fontMetrics().horizontalAdvance(u' '), height), kSelection);
            }
        }
        layout->draw(&painter, QPointF(left, y), selections);
        if (line == m_cursor.line && hasFocus()) {
            const qreal x = left + layout->lineAt(0).cursorToX(std::min(m_cursor.column, int(layout->text().size())));
            painter.fillRect(QRectF(x, y, 1, height), QColor(255, 255, 255, 191));
        }
    }

    // The gutter, over the text's left edge.
    painter.setClipping(false);
    painter.fillRect(QRect(0, 0, gutter, viewport()->height()), kGutter);
    const int right = gutter - fontMetrics().horizontalAdvance(u' ');
    for (int line = first; line <= last; ++line) {
        painter.setPen(line == m_cursor.line ? QColor(255, 255, 255, 191) : QColor(255, 255, 255, 89));
        painter.drawText(0, (line - first) * height, right, height, Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(line + 1));
    }
}

LineView::Position LineView::positionAt(const QPoint& point) {
    Position position;
    position.line = std::clamp(firstVisibleLine() + point.y() / lineHeight(), 0, std::max(0, lineCount() - 1));
    if (point.y() < 0) position.line = std::max(0, firstVisibleLine() - 1);
    QTextLayout* layout = layoutFor(position.line);
    position.column = layout->lineAt(0).xToCursor(point.x() - textLeft());
    return position;
}

std::size_t LineView::offsetOf(const Position& position) const {
    if (lineCount() == 0) return 0;
    const int line = std::clamp(position.line, 0, lineCount() - 1);
    return m_starts[line] + std::size_t(lineText(line).left(position.column).toUtf8().size());
}

LineView::Position LineView::positionOf(std::size_t offset) const {
    const auto it = std::upper_bound(m_starts.begin(), m_starts.end(), offset);
    Position position;
    position.line = int(std::distance(m_starts.begin(), it)) - 1;
    const std::size_t start = m_starts[position.line];
    position.column = int(QString::fromUtf8(m_text.data() + start, qsizetype(offset - start)).size());
    return position;
}

void LineView::setCursorPosition(const Position& position, bool keepAnchor) {
    Position clamped = position;
    clamped.line = std::clamp(clamped.line, 0, std::max(0, lineCount() - 1));
    clamped.column = std::clamp(clamped.column, 0, int(lineText(clamped.line).size()));
    m_cursor = clamped;
    if (!keepAnchor) m_anchor = clamped;
    ensureCursorVisible();
    viewport()->update();
}

void LineView::setSelection(const Position& anchor, const Position& cursor) {
    setCursorPosition(anchor);
    setCursorPosition(cursor, true);
}

void LineView::selectAll() {
    Position end;
    end.line = std::max(0, lineCount() - 1);
    end.column = int(lineText(end.line).size());
    m_anchor = Position();
    m_cursor = end;
    viewport()->update();
}

QString LineView::selectedText() const {
    if (!hasSelection()) return {};
    const std::size_t from = offsetOf(std::min(m_anchor, m_cursor));
    const std::size_t to = offsetOf(std::max(m_anchor, m_cursor));
    return QString::fromUtf8(m_text.data() + from, qsizetype(to - from));
}

void LineView::copy() const {
    if (hasSelection()) QApplication::clipboard()->setText(selectedText());
}

void LineView::scrollToLine(int line, bool center) {
    const int first = firstVisibleLine();
    const int visible = visibleLines();
    if (center) {
        verticalScrollBar()->setValue(line - visible / 2);
    } else if (line < first) {
        verticalScrollBar()->setValue(line);
    } else if (line >= first + visible) {
        verticalScrollBar()->setValue(line - visible + 1);
    }
}

void LineView::ensureCursorVisible() {
    scrollToLine(m_cursor.line);
    QTextLayout* layout = layoutFor(m_cursor.line);
    const int x = int(layout->lineAt(0).cursorToX(m_cursor.column));
    const int width = viewport()->width() - gutterWidth() - kTextMargin * 2;
    const int scroll = horizontalScrollBar()->value();
    if (x < scroll) {
        horizontalScrollBar()->setValue(x);
    } else if (x > scroll + width) {
        horizontalScrollBar()->setValue(x - width);
    }
}

bool LineView::find(const QString& needleText, bool backward) {
    const std::string needle = needleText.toUtf8().toStdString();
    if (needle.empty() || m_text.size() < needle.size()) return false;
    std::string lowered = needle;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), asciiLower);
    const std::size_t last = m_text.size() - needle.size();
    auto matches = [&](std::size_t at) {
        for (std::size_t k = 0; k < lowered.size(); ++k) {
            if (asciiLower(m_text[at + k]) != lowered[k]) return false;
        }
        return true;
    };
    const std::size_t from = offsetOf(std::min(m_anchor, m_cursor));
    const std::size_t to = offsetOf(std::max(m_anchor, m_cursor));
    std::size_t found = std::string::npos;
    if (!backward) {
        // From the selection's end (its start if empty, so a match under the cursor counts).
        const std::size_t start = hasSelection() ? from + 1 : to;
        for (std::size_t at = start; at <= last && found == std::string::npos; ++at) {
            if (matches(at)) found = at;
        }
        for (std::size_t at = 0; at < std::min(start, last + 1) && found == std::string::npos; ++at) {
            if (matches(at)) found = at;  // wrapped around
        }
    } else {
        for (std::size_t at = std::min(from, last + 1); at-- > 0 && found == std::string::npos;) {
            if (matches(at)) found = at;
        }
        for (std::size_t at = last + 1; at-- > from && found == std::string::npos;) {
            if (matches(at)) found = at;
        }
    }
    if (found == std::string::npos) return false;
    const Position start = positionOf(found);
    const Position end = positionOf(found + needle.size());
    m_anchor = start;
    m_cursor = end;
    scrollToLine(start.line, true);
    ensureCursorVisible();
    viewport()->update();
    return true;
}

int LineView::countMatches(const QString& needleText) const {
    std::string needle = needleText.toUtf8().toStdString();
    if (needle.empty() || m_text.size() < needle.size()) return 0;
    std::transform(needle.begin(), needle.end(), needle.begin(), asciiLower);
    const char first = needle[0];
    const char firstUpper = (first >= 'a' && first <= 'z') ? char(first - 'a' + 'A') : first;
    int count = 0;
    const std::size_t last = m_text.size() - needle.size();
    for (std::size_t at = 0; at <= last; ++at) {
        const char c = m_text[at];
        if (c != first && c != firstUpper) continue;
        std::size_t k = 1;
        while (k < needle.size() && asciiLower(m_text[at + k]) == needle[k]) ++k;
        if (k == needle.size()) {
            ++count;
            at += needle.size() - 1;  // matches don't overlap
        }
    }
    return count;
}

void LineView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return QAbstractScrollArea::mousePressEvent(event);
    m_preferredColumn = -1;
    setCursorPosition(positionAt(event->position().toPoint()), event->modifiers() & Qt::ShiftModifier);
}

void LineView::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) return;
    setCursorPosition(positionAt(event->position().toPoint()), true);  // scrolls when dragged past an edge
}

void LineView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const Position at = positionAt(event->position().toPoint());
    const QString text = lineText(at.line);
    int from = std::min(at.column, int(text.size()));
    int to = from;
    while (from > 0 && isWordChar(text[from - 1])) --from;
    while (to < text.size() && isWordChar(text[to])) ++to;
    setSelection({at.line, from}, {at.line, to});
}

void LineView::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) return copy();
    if (event->matches(QKeySequence::SelectAll)) return selectAll();
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const bool control = event->modifiers() & Qt::ControlModifier;
    Position to = m_cursor;
    const int length = int(lineText(to.line).size());
    switch (event->key()) {
        case Qt::Key_Left:
            if (to.column > 0) {
                --to.column;
            } else if (to.line > 0) {
                --to.line;
                to.column = int(lineText(to.line).size());
            }
            break;
        case Qt::Key_Right:
            if (to.column < length) {
                ++to.column;
            } else if (to.line + 1 < lineCount()) {
                ++to.line;
                to.column = 0;
            }
            break;
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown: {
            if (m_preferredColumn < 0) m_preferredColumn = to.column;
            const int page = visibleLines();
            const int step = event->key() == Qt::Key_Up ? -1 : event->key() == Qt::Key_Down ? 1
                             : event->key() == Qt::Key_PageUp ? -page : page;
            to.line = std::clamp(to.line + step, 0, std::max(0, lineCount() - 1));
            to.column = m_preferredColumn;
            if (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown) {
                verticalScrollBar()->setValue(verticalScrollBar()->value() + step);
            }
            setCursorPosition(to, shift);
            return;
        }
        case Qt::Key_Home:
            to = control ? Position() : Position{to.line, 0};
            break;
        case Qt::Key_End:
            if (control) to.line = std::max(0, lineCount() - 1);
            to.column = int(lineText(to.line).size());
            break;
        default:
            return QAbstractScrollArea::keyPressEvent(event);
    }
    m_preferredColumn = -1;
    setCursorPosition(to, shift);
}

void LineView::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* copyAction = menu.addAction(tr("Copy"), QKeySequence::Copy, this, &LineView::copy);
    copyAction->setEnabled(hasSelection());
    menu.addAction(tr("Select All"), QKeySequence::SelectAll, this, &LineView::selectAll);
    menu.exec(event->globalPos());
}

}  // namespace usdcc::ui
