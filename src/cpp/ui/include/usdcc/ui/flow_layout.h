#pragma once

#include <QLayout>
#include <QList>

namespace usdcc::ui {

// Lays widgets out left to right, wrapping to more lines when they don't fit
// (Qt has none). Its height depends on its width: hasHeightForWidth(). An item
// wider than the layout is given the layout's width.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override { return int(m_items.size()); }
    QLayoutItem* itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem* takeAt(int index) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;

private:
    // Places the items within `rect` (only measuring, without `apply`) and
    // returns the height they took.
    int arrange(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> m_items;
};

}  // namespace usdcc::ui
