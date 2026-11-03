#include "usdcc/usd/outliner_item_delegate.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

#include <algorithm>

namespace usdcc::usd {

namespace {
constexpr int kItemRadius = 3;  // dark.qss, QTreeView::item border-radius
constexpr int kCheckMargin = 6;  // from the row's right edge: dark.qss, QTreeView::item padding
constexpr int kCheckGap = 6;     // between the name and the checkbox

QStyle* styleOf(const QStyleOptionViewItem& option) {
    return option.widget ? option.widget->style() : QApplication::style();
}
}  // namespace

QStyleOptionViewItem OutlinerItemDelegate::rowOption(const QStyleOptionViewItem& option,
                                                     const QModelIndex& index) const {
    QStyleOptionViewItem row = option;
    initStyleOption(&row, index);
    row.features &= ~QStyleOptionViewItem::HasCheckIndicator;  // drawn at the right instead
    return row;
}

QRect OutlinerItemDelegate::checkRect(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (!(index.flags() & Qt::ItemIsUserCheckable) || !index.data(Qt::CheckStateRole).isValid()) return {};
    QStyleOptionViewItem withCheck = option;
    initStyleOption(&withCheck, index);
    // The style's size for it (dark.qss, QTreeView::indicator), at the right.
    const QSize size =
        styleOf(withCheck)->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &withCheck, withCheck.widget).size();
    const int right = option.rect.right() - usdcc::ui::scaled::pixels(kCheckMargin);
    return {QPoint(right - size.width() + 1, option.rect.center().y() - size.height() / 2), size};
}

QRect OutlinerItemDelegate::textRect(const QStyleOptionViewItem& rowOption, const QRect& check) const {
    QRect text = styleOf(rowOption)->subElementRect(QStyle::SE_ItemViewItemText, &rowOption, rowOption.widget);
    if (check.isValid()) text.setRight(std::min(text.right(), check.left() - usdcc::ui::scaled::pixels(kCheckGap)));
    return text;
}

void OutlinerItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    const QVariant tint = index.data(Qt::BackgroundRole);
    if (tint.canConvert<QColor>() && !(option.state & QStyle::State_Selected)) {
        const qreal radius = usdcc::ui::scaled::pixels(kItemRadius);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(tint.value<QColor>());
        painter->drawRoundedRect(QRectF(option.rect).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        painter->restore();
    }

    QStyleOptionViewItem row = rowOption(option, index);
    const QRect check = checkRect(option, index);
    // Cut the name short of the checkbox; the style lays it out to the row's end.
    row.text = row.fontMetrics.elidedText(row.text, row.textElideMode, textRect(row, check).width());
    QStyle* style = styleOf(row);
    style->drawControl(QStyle::CE_ItemViewItem, &row, painter, row.widget);

    if (check.isValid()) {
        QStyleOptionViewItem box = row;
        box.rect = check;
        box.features |= QStyleOptionViewItem::HasCheckIndicator;
        box.checkState = index.data(Qt::CheckStateRole).value<Qt::CheckState>();
        box.state &= ~(QStyle::State_On | QStyle::State_Off | QStyle::State_NoChange);
        box.state |= box.checkState == Qt::Checked     ? QStyle::State_On
                     : box.checkState == Qt::Unchecked ? QStyle::State_Off
                                                       : QStyle::State_NoChange;
        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &box, painter, row.widget);
    }
}

void OutlinerItemDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                                const QModelIndex& index) const {
    // Over the name only, clear of the checkbox.
    editor->setGeometry(textRect(rowOption(option, index), checkRect(option, index)));
}

bool OutlinerItemDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                       const QStyleOptionViewItem& option, const QModelIndex& index) {
    // Not QStyledItemDelegate's: it would look for the box where Qt draws it.
    const QRect check = checkRect(option, index);
    if (!check.isValid() || !(model->flags(index) & Qt::ItemIsEnabled)) return false;
    auto toggle = [&]() {
        const bool checked = index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
        return model->setData(index, checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    };
    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            // A little slack around the 11px box.
            const QRect target = check.adjusted(-2, -2, 2, 2);
            if (mouse->button() != Qt::LeftButton || !target.contains(mouse->position().toPoint())) return false;
            // Press and double-click are taken too, so they neither rename nor
            // select; the release toggles.
            if (event->type() == QEvent::MouseButtonRelease) toggle();
            return true;
        }
        case QEvent::KeyPress: {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key != Qt::Key_Space && key != Qt::Key_Select) return false;
            toggle();
            return true;
        }
        default:
            return false;
    }
}

}  // namespace usdcc::usd
