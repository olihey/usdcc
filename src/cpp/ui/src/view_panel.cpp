#include "usdcc/ui/view_panel.h"

#include <DockManager.h>
#include <DockWidgetTab.h>
#include <QtCore/qcompilerdetection.h>

#include <QContextMenuEvent>
#include <QMenu>

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED

namespace usdcc::ui {

ViewPanel::ViewPanel(const QString& title, QWidget* parent) : ads::CDockWidget(title, parent) {
    // ADS shows this panel as a tab (ads::CDockWidgetTab) in its dock area's
    // title bar, and that tab already has its own context menu
    // (Detach/Pin/Close/...) via CDockWidgetTab::contextMenuEvent(). An
    // event filter lets us extend that menu with our own entries without
    // subclassing CDockWidgetTab — ADS creates that internally via its own
    // component factory, with no hook to inject a custom subclass.
    // tabWidget() is already valid here: CDockWidget's constructor creates
    // it before returning.
    tabWidget()->installEventFilter(this);
}

void ViewPanel::setContentWidget(QWidget* widget) { setWidget(widget); }

QWidget* ViewPanel::contentWidget() const { return widget(); }

ViewPanel* ViewPanel::duplicate(QWidget* parent) const { return new ViewPanel(windowTitle(), parent); }

void ViewPanel::populateContextMenu(QMenu* menu) {
    menu->addSeparator();
    menu->addAction(tr("Duplicate"), this, [this]() {
        auto* copy = duplicate(window());

        // ads::CDockManager looks dock widgets up by objectName() (CDockWidget's
        // constructor sets that to its title); giving the duplicate the exact
        // same title/objectName as this panel would silently overwrite this
        // panel's entry in that lookup map once both are docked, since
        // QMap::insert() replaces same-key entries rather than rejecting
        // them. Find a free "<title> (Copy N)" name instead.
        QString candidateTitle = tr("%1 (Copy)").arg(windowTitle());
        for (int suffix = 2; dockManager() && dockManager()->findDockWidget(candidateTitle); ++suffix) {
            candidateTitle = tr("%1 (Copy %2)").arg(windowTitle()).arg(suffix);
        }
        copy->setWindowTitle(candidateTitle);
        copy->setObjectName(candidateTitle);

        if (auto* area = dockAreaWidget()) {
            dockManager()->addDockWidgetTabToArea(copy, area);
        }
    });
}

bool ViewPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tabWidget() && event->type() == QEvent::ContextMenu) {
        auto* menuEvent = static_cast<QContextMenuEvent*>(event);
        QMenu* menu = tabWidget()->buildContextMenu(nullptr);
        populateContextMenu(menu);
        menu->exec(menuEvent->globalPos());
        menu->deleteLater();
        return true;
    }
    return ads::CDockWidget::eventFilter(watched, event);
}

}  // namespace usdcc::ui

QT_WARNING_POP
