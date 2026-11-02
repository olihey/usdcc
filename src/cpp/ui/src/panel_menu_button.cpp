#include "usdcc/ui/panel_menu_button.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QIcon>
#include <QMenu>

#include <utility>

namespace usdcc::ui {

PanelMenuButton::PanelMenuButton(const QString& toolTip, std::function<void(QMenu*)> populate, QWidget* parent)
    : QToolButton(parent), m_populate(std::move(populate)) {
    setObjectName(QStringLiteral("panelMenuButton"));  // styled by theme/dark.qss
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setIcon(QIcon(QStringLiteral(":/usdcc/theme/menu.png")));
    scaled::iconSize(this, 16, 16);
    setToolTip(toolTip);
    connect(this, &QToolButton::clicked, this, &PanelMenuButton::showMenu);
}

void PanelMenuButton::resizeEvent(QResizeEvent* event) {
    QToolButton::resizeEvent(event);
    if (width() != height()) {
        setFixedWidth(height());
    }
}

void PanelMenuButton::showMenu() {
    // Built per opening and destroyed afterwards, so submenus and action
    // groups don't pile up.
    QMenu menu(this);
    m_populate(&menu);
    setDown(true);
    menu.exec(mapToGlobal(QPoint(0, height())));
    setDown(false);
}

}  // namespace usdcc::ui
