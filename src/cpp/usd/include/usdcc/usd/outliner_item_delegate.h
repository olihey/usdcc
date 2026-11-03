#pragma once

#include <QStyledItemDelegate>

namespace usdcc::usd {

// Draws the Outliner's rows: the prim's icon and name, and its active
// checkbox at the row's right edge rather than before the icon, so the icons
// line up down the tree. A click on the box (or Space) toggles it; elsewhere
// in the row clicks select and rename as usual, and the name is cut short
// before it reaches the box.
//
// Also paints the model's BackgroundRole (the selection-ancestor tint)
// itself: with the stylesheet's QTreeView::item rule (padding, rounded
// corners) in effect, the style draws the item panel from that rule and
// ignores the model's brush.
class OutlinerItemDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override;

    // Where the checkbox is in a row (public for tests).
    QRect checkRect(const QStyleOptionViewItem& option, const QModelIndex& index) const;

protected:
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

private:
    // `option` for the row without Qt's own (left) checkbox, and the room
    // its name has, up to the checkbox.
    QStyleOptionViewItem rowOption(const QStyleOptionViewItem& option, const QModelIndex& index) const;
    QRect textRect(const QStyleOptionViewItem& rowOption, const QRect& check) const;
};

}  // namespace usdcc::usd
