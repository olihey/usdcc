#pragma once

#include "usdcc/core/export.h"

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QToolButton;
class QVBoxLayout;

namespace usdcc::ui {

// A collapsible group of rows: a header with a chevron and the title, then
// the rows. The attribute editor's sections (the mockup's "▼ Transform"
// headers, docs/ATTRIBUTE_EDITOR.md §4.2) and the Settings dialog's groups.
// Whoever makes one persists the collapsed state per key().
class USDCC_UI_API CategorySection : public QWidget {
    Q_OBJECT

public:
    CategorySection(const QString& key, const QString& title, QWidget* parent = nullptr);

    const QString& key() const { return m_key; }

    void addRow(QWidget* row);
    // A control in the header, before the row count (e.g. the transform stack's "+ Add").
    void addHeaderWidget(QWidget* widget);
    // Around the rows, in unscaled pixels (scaled with the UI); 14, 2, 0, 10 by default.
    void setBodyMargins(int left, int top, int right, int bottom);

    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);
    // Shows the rows even when collapsed (while a search filters them),
    // without changing the remembered collapsed state.
    void setForceExpanded(bool force);

Q_SIGNALS:
    void collapsedChanged(bool collapsed);

private:
    void updateBody();

    QString m_key;
    QToolButton* m_header;
    QHBoxLayout* m_headerLayout;
    QWidget* m_body;
    QVBoxLayout* m_bodyLayout;
    bool m_collapsed = false;
    bool m_forceExpanded = false;
};

}  // namespace usdcc::ui
