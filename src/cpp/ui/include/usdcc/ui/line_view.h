#pragma once

#include "usdcc/core/export.h"

#include <QAbstractScrollArea>
#include <QList>
#include <QMenu>
#include <QTextLayout>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace usdcc::ui {

// A read-only text view for very big text (docs/USDA_VIEW.md §5, §9.2): the
// text stays one UTF-8 buffer with an index of where each line starts, and
// only the lines on screen are decoded, coloured (by setLineFormatter()) and
// laid out, each its own QTextLayout, cached while it's near the screen. So
// setting 80 MB takes as long as finding its line ends, where
// QPlainTextEdit builds a block per line up front (seconds).
//
// Monospace, without wrapping (#codeView in theme/dark.qss), with a
// line-number gutter and the cursor's line highlighted, like TextView. It
// selects with the mouse (drag, double-click a word, Shift+click) and the
// keyboard (arrows, Page Up/Down, Home/End, with Ctrl for the whole text,
// Shift to extend), copies (Ctrl+C) and finds (find(), case-insensitive for
// ASCII). Positions are a line and a column in UTF-16 units of that line.
class USDCC_UI_API LineView : public QAbstractScrollArea {
    Q_OBJECT

public:
    struct Position {
        int line = 0;
        int column = 0;
        bool operator==(const Position& o) const { return line == o.line && column == o.column; }
        bool operator!=(const Position& o) const { return !(*this == o); }
        bool operator<(const Position& o) const { return line < o.line || (line == o.line && column < o.column); }
    };
    // Colours line `line` (its text decoded): format ranges in UTF-16 columns.
    using LineFormatter = std::function<QList<QTextLayout::FormatRange>(int line, const QString& text)>;

    // A link in the text (docs/USDA_VIEW.md §6): where it is, its text and
    // its kind (the owner's: e.g. an asset path or a prim path).
    struct Link {
        int line = 0;
        int start = 0;
        int length = 0;
        QString text;
        int kind = 0;
    };
    // The links in line `line`: start, length and kind of each. Asked for
    // when a line is laid out, so only for lines on screen.
    using LinkFinder = std::function<QList<Link>(int line, const QString& text)>;

    explicit LineView(QWidget* parent = nullptr);

    // Replaces the text ('\n' line ends; a '\r' before one is dropped); the
    // cursor and selection go to the start.
    void setText(std::string utf8);
    const std::string& text() const { return m_text; }
    void setLineFormatter(LineFormatter formatter);
    // Links: over one the cursor is a pointing hand with `toolTip(link)` as
    // its tooltip. A click on one (not a drag from it) sends linkActivated;
    // a middle-click too, with Qt::MiddleButton. Right-clicking one sends
    // linkMenuRequested with the menu, before Copy and Select All.
    void setLinkFinder(LinkFinder finder, std::function<QString(const Link&)> toolTip = {});
    // The link at a position, if there's one (lays out that line).
    bool linkAt(const Position& position, Link* link);
    int lineCount() const { return int(m_starts.size()); }
    QString lineText(int line) const;

    Position cursorPosition() const { return m_cursor; }
    // The selection's other end (the cursor's position when nothing is selected).
    Position anchorPosition() const { return m_anchor; }
    // Moves the cursor; with `keepAnchor` the selection runs from where it was.
    void setCursorPosition(const Position& position, bool keepAnchor = false);
    void setSelection(const Position& anchor, const Position& cursor);
    bool hasSelection() const { return m_anchor != m_cursor; }
    QString selectedText() const;
    void selectAll();
    void copy() const;

    int firstVisibleLine() const;
    // Scrolls so `line` is on screen (in the middle with `center`).
    void scrollToLine(int line, bool center = false);

    // Selects the next match of `needle` after the selection (before it with
    // `backward`), wrapping around; false if there's none.
    bool find(const QString& needle, bool backward = false);
    int countMatches(const QString& needle) const;

    // Lines laid out (and coloured) since setText(), and whether `line` is
    // laid out now: for tests of drawing only what's on screen.
    int formattedLines() const { return m_formatted; }
    bool isLaidOut(int line) const { return m_layouts.count(line) != 0; }

Q_SIGNALS:
    void linkActivated(const usdcc::ui::LineView::Link& link, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void linkMenuRequested(QMenu* menu, const usdcc::ui::LineView::Link& link);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void changeEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    QTextLayout* layoutFor(int line);
    int lineHeight() const;
    int gutterWidth() const;
    int textLeft() const;  // x of column 0, scrolled
    int visibleLines() const;
    Position positionAt(const QPoint& point);
    std::size_t offsetOf(const Position& position) const;
    Position positionOf(std::size_t offset) const;
    void ensureCursorVisible();
    void updateScrollBars();
    void resetLayouts();

    std::string m_text;
    std::vector<std::size_t> m_starts;  // byte offset of each line
    std::size_t m_longestLine = 0;      // in bytes, for the horizontal range
    LineFormatter m_formatter;
    // A laid-out line and the links in it.
    struct LaidOutLine {
        std::unique_ptr<QTextLayout> layout;
        QList<Link> links;
    };
    LaidOutLine* laidOut(int line);
    std::unordered_map<int, LaidOutLine> m_layouts;
    int m_formatted = 0;
    LinkFinder m_linkFinder;
    std::function<QString(const Link&)> m_linkToolTip;
    // A press on a link, activated on release if the mouse didn't drag.
    bool m_pressedOnLink = false;
    Link m_pressedLink;
    QPoint m_pressPoint;
    Position m_anchor;
    Position m_cursor;
    int m_preferredColumn = -1;  // kept across Up/Down through shorter lines
};

}  // namespace usdcc::ui
