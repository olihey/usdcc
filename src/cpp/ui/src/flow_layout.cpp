#include "usdcc/ui/flow_layout.h"

#include <QWidget>

#include <algorithm>

namespace usdcc::ui {

FlowLayout::FlowLayout(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }

FlowLayout::~FlowLayout() {
    while (QLayoutItem* item = takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { m_items.append(item); }

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index < 0 || index >= m_items.size()) return nullptr;
    invalidate();
    return m_items.takeAt(index);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const {
    // No minimum width: a long item is cut short rather than widening the row.
    QSize size;
    for (const QLayoutItem* item : m_items) size = size.expandedTo(QSize(0, item->minimumSize().height()));
    return size;
}

int FlowLayout::heightForWidth(int width) const { return arrange(QRect(0, 0, width, 0), false); }

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect& rect, bool apply) const {
    const int gap = std::max(0, spacing());
    int x = rect.x();
    int y = rect.y();
    int lineHeight = 0;
    for (QLayoutItem* item : m_items) {
        if (item->isEmpty()) continue;
        QSize size = item->sizeHint();
        size.setWidth(std::min(size.width(), rect.width()));
        if (x > rect.x() && x + size.width() > rect.right() + 1) {
            x = rect.x();
            y += lineHeight + gap;
            lineHeight = 0;
        }
        if (apply) item->setGeometry(QRect(QPoint(x, y), size));
        x += size.width() + gap;
        lineHeight = std::max(lineHeight, size.height());
    }
    return y + lineHeight - rect.y();
}

}  // namespace usdcc::ui
