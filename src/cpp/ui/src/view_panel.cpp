#include "usdcc/ui/view_panel.h"

#include <QtCore/qcompilerdetection.h>

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED

namespace usdcc::ui {

ViewPanel::ViewPanel(const QString& title, QWidget* parent) : ads::CDockWidget(title, parent) {}

void ViewPanel::setContentWidget(QWidget* widget) { setWidget(widget); }

QWidget* ViewPanel::contentWidget() const { return widget(); }

}  // namespace usdcc::ui

QT_WARNING_POP
