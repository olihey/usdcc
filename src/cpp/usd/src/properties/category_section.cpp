#include "usdcc/usd/properties/category_section.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace usdcc::usd::properties {

CategorySection::CategorySection(const QString& key, const QString& title, QWidget* parent)
    : QWidget(parent), m_key(key) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* separator = new QFrame(this);
    separator->setObjectName(QStringLiteral("sectionSeparator"));
    separator->setFixedHeight(1);  // a hairline at every UI scale, as in the stylesheets
    layout->addWidget(separator);

    auto* headerRow = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(headerRow);
    m_headerLayout = headerLayout;
    usdcc::ui::scaled::margins(headerLayout, 4, 4, 8, 4);
    usdcc::ui::scaled::spacing(headerLayout, 4);
    m_header = new QToolButton(headerRow);
    m_header->setObjectName(QStringLiteral("sectionHeader"));
    m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_header->setArrowType(Qt::DownArrow);
    m_header->setText(title);
    m_header->setCursor(Qt::PointingHandCursor);
    m_count = new QLabel(headerRow);
    m_count->setObjectName(QStringLiteral("sectionCount"));
    headerLayout->addWidget(m_header);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_count);
    layout->addWidget(headerRow);

    m_body = new QWidget(this);
    m_bodyLayout = new QVBoxLayout(m_body);
    usdcc::ui::scaled::margins(m_bodyLayout, 14, 2, 10, 10);
    usdcc::ui::scaled::spacing(m_bodyLayout, 6);
    layout->addWidget(m_body);

    connect(m_header, &QToolButton::clicked, this, [this]() {
        setCollapsed(!m_collapsed);
        Q_EMIT collapsedChanged(m_collapsed);
    });
}

void CategorySection::addRow(QWidget* row) { m_bodyLayout->addWidget(row); }

void CategorySection::addHeaderWidget(QWidget* widget) {
    m_headerLayout->insertWidget(m_headerLayout->count() - 1, widget);  // before the count
}

void CategorySection::setCollapsed(bool collapsed) {
    m_collapsed = collapsed;
    updateBody();
}

void CategorySection::setForceExpanded(bool force) {
    m_forceExpanded = force;
    updateBody();
}

void CategorySection::setCount(int count) { m_count->setText(QString::number(count)); }

void CategorySection::updateBody() {
    const bool open = !m_collapsed || m_forceExpanded;
    m_body->setVisible(open);
    m_header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
}

}  // namespace usdcc::usd::properties
