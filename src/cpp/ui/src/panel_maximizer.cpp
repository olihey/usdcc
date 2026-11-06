#include "usdcc/ui/panel_maximizer.h"

#include <DockAreaWidget.h>
#include <DockContainerWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>

#include <QDockWidget>
#include <QEvent>
#include <QLayout>
#include <QMainWindow>

#include <algorithm>
#include <utility>

namespace usdcc::ui {

namespace {
// A part ADS would have hidden itself by now, had it not been hidden already:
// the panels in it were closed or deleted meanwhile.
bool hasOpenPanels(const QWidget* widget) {
    if (auto* area = qobject_cast<const ads::CDockAreaWidget*>(widget)) return area->openDockWidgetsCount() > 0;
    if (auto* splitter = qobject_cast<const ads::CDockSplitter*>(widget)) return splitter->hasVisibleContent();
    return true;
}
}  // namespace

PanelMaximizer::PanelMaximizer(QMainWindow* window, ads::CDockManager* dockManager)
    : QObject(window), m_window(window), m_dockManager(dockManager) {
    using ads::CDockContainerWidget;
    using ads::CDockManager;
    connect(dockManager, &CDockManager::restoringState, this, &PanelMaximizer::restore);
    connect(dockManager, &CDockManager::dockWidgetAdded, this, &PanelMaximizer::restoreLater);
    connect(dockManager, &CDockContainerWidget::dockAreasAdded, this, &PanelMaximizer::restoreLater);
    connect(dockManager, &CDockContainerWidget::dockAreasRemoved, this, &PanelMaximizer::restoreLater);
}

bool PanelMaximizer::canMaximize(const ads::CDockWidget* panel) const {
    return panel && !panel->isClosed() && !panel->isAutoHide() && panel->dockAreaWidget() &&
           panel->dockContainer() == m_dockManager;
}

bool PanelMaximizer::isMaximized(const ads::CDockWidget* panel) const {
    return m_active && panel && m_area && panel->dockAreaWidget() == m_area;
}

void PanelMaximizer::maximize(ads::CDockWidget* panel) {
    restore();
    if (!canMaximize(panel)) return;

    panel->setAsCurrentTab();
    m_area = panel->dockAreaWidget();
    m_active = true;

    // QDockWidgets only: ADS's panels are plain widgets further down.
    m_windowState = m_window->saveState();
    const QList<QDockWidget*> sidePanels = m_window->findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QDockWidget* sidePanel : sidePanels) {
        if (!sidePanel->isFloating()) sidePanel->hide();
    }

    QWidget* keep = m_area;
    while (auto* splitter = qobject_cast<QSplitter*>(keep->parentWidget())) {
        m_splitterSizes.append({splitter, splitter->sizes()});
        for (int i = 0; i < splitter->count(); ++i) {
            QWidget* child = splitter->widget(i);
            if (child == keep || child->isHidden()) continue;
            child->hide();
            child->installEventFilter(this);
            m_hiddenWidgets.append(child);
        }
        keep = splitter;
    }
    m_area->installEventFilter(this);
    m_dockManager->setDockWidgetFocused(panel);
}

void PanelMaximizer::restore() {
    if (!m_active) return;
    m_active = false;

    if (m_area) m_area->removeEventFilter(this);
    m_area.clear();
    const QList<QPointer<QWidget>> hidden = std::exchange(m_hiddenWidgets, {});
    for (const QPointer<QWidget>& widget : hidden) {
        if (widget) widget->removeEventFilter(this);
    }

    // The side panels first: the splitter sizes are for the room they leave.
    m_window->restoreState(std::exchange(m_windowState, {}));
    if (QLayout* layout = m_window->layout()) layout->activate();

    for (const QPointer<QWidget>& widget : hidden) {
        if (widget && hasOpenPanels(widget)) widget->show();
    }
    const QList<SplitterSizes> sizes = std::exchange(m_splitterSizes, {});
    // With the area closed they looked empty to ADS, which hid them. From the
    // area outwards, as hasOpenPanels() goes by what's in them.
    for (const SplitterSizes& entry : sizes) {
        if (entry.splitter && entry.splitter->isHidden() && hasOpenPanels(entry.splitter)) entry.splitter->show();
    }
    // From the outside in: each splitter's size depends on its parent's.
    for (auto it = sizes.crbegin(); it != sizes.crend(); ++it) {
        if (it->splitter && it->splitter->count() == it->sizes.size()) it->splitter->setSizes(it->sizes);
    }
}

void PanelMaximizer::toggle(ads::CDockWidget* panel) {
    if (isMaximized(panel)) {
        restore();
    } else {
        maximize(panel);
    }
}

void PanelMaximizer::restoreLater() {
    if (m_active) QMetaObject::invokeMethod(this, &PanelMaximizer::restore, Qt::QueuedConnection);
}

bool PanelMaximizer::eventFilter(QObject* watched, QEvent* event) {
    // Sent only when the widget itself is shown or hidden, not its window.
    if (m_active && event->type() == (watched == m_area.data() ? QEvent::HideToParent : QEvent::ShowToParent)) {
        restoreLater();
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace usdcc::ui
