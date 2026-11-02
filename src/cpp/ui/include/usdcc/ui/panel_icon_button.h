#pragma once

#include <QToolButton>

namespace usdcc::ui {

// A square, icon-only button for a panel's toolbar: as tall as the toolbar
// row and as wide as it is tall, at any UI scale, with a 16px icon from the
// theme (`iconPath`, e.g. ":/usdcc/theme/trash.png") and a tooltip saying
// what it does. Styled as #panelIconButton in theme/dark.qss. With an empty
// `iconPath` it shows its text instead (keep it to a glyph or two, e.g. "Aa").
class PanelIconButton : public QToolButton {
    Q_OBJECT

public:
    PanelIconButton(const QString& iconPath, const QString& toolTip, QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;
};

}  // namespace usdcc::ui
