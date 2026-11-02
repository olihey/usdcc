#pragma once

#include "usdcc/ui/panel_icon_button.h"

#include <functional>

class QMenu;

namespace usdcc::ui {

// The square three-lines button that starts a panel's toolbar (styled as
// #panelMenuButton in theme/dark.qss). A click opens a menu below it, built
// afresh by `populate` each time so it reflects the current state. Used by
// every ViewPanel for its options and by the LogSidePanel for its filters.
class PanelMenuButton : public PanelIconButton {
    Q_OBJECT

public:
    PanelMenuButton(const QString& toolTip, std::function<void(QMenu*)> populate, QWidget* parent = nullptr);

private:
    void showMenu();

    std::function<void(QMenu*)> m_populate;
};

}  // namespace usdcc::ui
