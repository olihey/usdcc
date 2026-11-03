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
    // groups don't pile up. Parented to the top-level window, not this
    // button: Qt makes the menu's nearest native ancestor its transient
    // parent, which must be a top-level window, and in a viewport panel that
    // is a native child (the Hydra window's container makes its ancestors
    // native), so Qt would warn "... must be a top level window".
    QMenu menu(window());
    m_populate(&menu);
    setDown(true);
    menu.exec(mapToGlobal(QPoint(0, height())));
    setDown(false);
}

}  // namespace usdcc::ui
