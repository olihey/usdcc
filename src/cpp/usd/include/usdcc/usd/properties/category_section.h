#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace usdcc::usd::properties {

// A collapsible group of rows in the attribute editor (the mockup's
// "▼ Transform" headers, docs/ATTRIBUTE_EDITOR.md §4.2): a header with a
// chevron, the title and a row count, then the rows. The panel persists the
// collapsed state per key().
class CategorySection : public QWidget {
    Q_OBJECT

public:
    CategorySection(const QString& key, const QString& title, QWidget* parent = nullptr);

    const QString& key() const { return m_key; }

    void addRow(QWidget* row);

    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);
    // Shows the rows even when collapsed (while a search filters them),
    // without changing the remembered collapsed state.
    void setForceExpanded(bool force);
    void setCount(int count);

Q_SIGNALS:
    void collapsedChanged(bool collapsed);

private:
    void updateBody();

    QString m_key;
    QToolButton* m_header;
    QLabel* m_count;
    QWidget* m_body;
    QVBoxLayout* m_bodyLayout;
    bool m_collapsed = false;
    bool m_forceExpanded = false;
};

}  // namespace usdcc::usd::properties
