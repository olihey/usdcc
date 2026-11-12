#include "usdcc/usd/outliner_item_delegate.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/usd/prim_icons.h"

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
constexpr int kCheckGap = 6;     // between the name and the eye
constexpr int kEyeSize = 16;     // the prim icons' size (docs/PRIM_ICONS.md §4)

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
    // The eye, as big as the prim's icon, at the right.
    const int side = std::min(usdcc::ui::scaled::pixels(kEyeSize), option.rect.height());
    const QSize size(side, side);
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
    const QRect text = textRect(row, check);
    // Cut the name short of the eye; the style lays it out to the row's end.
    const QString name = row.fontMetrics.elidedText(row.text, row.textElideMode, text.width());
    // A selected row's name is white (dark.qss), but a coloured one (an
    // instance, a prim with arcs) keeps its colour, so selecting it doesn't
    // hide what it is: the style draws the row without it, and the name is
    // drawn here.
    const QVariant foreground = index.data(Qt::ForegroundRole);
    const bool ownColor = (row.state & QStyle::State_Selected) && foreground.canConvert<QColor>();
    row.text = ownColor ? QString() : name;
    QStyle* style = styleOf(row);
    style->drawControl(QStyle::CE_ItemViewItem, &row, painter, row.widget);
    if (ownColor) {
        // Inset as the style insets item text.
        const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, row.widget) + 1;
        painter->save();
        painter->setFont(row.font);
        painter->setPen(foreground.value<QColor>());
        painter->drawText(text.adjusted(margin, 0, -margin, 0), Qt::AlignLeft | Qt::AlignVCenter, name);
        painter->restore();
    }

    if (check.isValid()) {
        // An eye while the prim is active, the eye struck through once it's not; white on a
        // selected row, like the prim's icon.
        const bool active = index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
        const QIcon eye = prim_icons::icon(active ? QStringLiteral("eye") : QStringLiteral("eye-off"));
        eye.paint(painter, check, Qt::AlignCenter, (row.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal);
    }
}

void OutlinerItemDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                                const QModelIndex& index) const {
    // Over the name only, clear of the eye.
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
            // A little slack around the eye.
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
