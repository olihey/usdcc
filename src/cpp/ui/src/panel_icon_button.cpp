#include "usdcc/ui/panel_icon_button.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QIcon>

namespace usdcc::ui {

PanelIconButton::PanelIconButton(const QString& iconPath, const QString& toolTip, QWidget* parent)
    : QToolButton(parent) {
    setObjectName(QStringLiteral("panelIconButton"));  // styled by theme/dark.qss
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setIcon(QIcon(iconPath));
    scaled::iconSize(this, 16, 16);
    setToolTip(toolTip);
}

void PanelIconButton::resizeEvent(QResizeEvent* event) {
    QToolButton::resizeEvent(event);
    if (width() != height()) {
        setFixedWidth(height());
    }
}

}  // namespace usdcc::ui
