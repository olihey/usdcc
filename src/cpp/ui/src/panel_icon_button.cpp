#include "usdcc/ui/panel_icon_button.h"
#include "usdcc/ui/theme.h"

#include "usdcc/ui/scaled_metrics.h"

#include <QIcon>

namespace usdcc::ui {

PanelIconButton::PanelIconButton(const QString& iconPath, const QString& toolTip, QWidget* parent)
    : QToolButton(parent) {
    setObjectName(QStringLiteral("panelIconButton"));  // styled by theme/app.qss
    // Also by this, for one that takes a name of its own (a render view panel's Render).
    setProperty("panelIcon", true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setIcon(usdcc::ui::theme::icon(iconPath, iconPath));
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
