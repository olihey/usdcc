#include "usdcc/ui/panel_menu_button.h"

#include <QMenu>

#include <utility>

namespace usdcc::ui {

PanelMenuButton::PanelMenuButton(const QString& toolTip, std::function<void(QMenu*)> populate, QWidget* parent)
    : PanelIconButton(QStringLiteral(":/usdcc/theme/menu.png"), toolTip, parent), m_populate(std::move(populate)) {
    setObjectName(QStringLiteral("panelMenuButton"));  // styled by theme/dark.qss
    connect(this, &QToolButton::clicked, this, &PanelMenuButton::showMenu);
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
