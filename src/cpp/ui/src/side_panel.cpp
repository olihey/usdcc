#include "usdcc/ui/side_panel.h"

namespace usdcc::ui {

SidePanel::SidePanel(const QString& title, QWidget* parent) : QDockWidget(title, parent) {
    setObjectName(title);
}

}  // namespace usdcc::ui
