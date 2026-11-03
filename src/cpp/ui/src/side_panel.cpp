#include "usdcc/ui/side_panel.h"

#include <QApplication>
#include <QList>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTimer>

namespace usdcc::ui {

namespace {
// The tab bar of the spot `dock` shares with other panels (QMainWindow keeps
// each tab's dock widget's address as the tab's data).
const QTabBar* tabBarOf(const QMainWindow* window, const QDockWidget* dock) {
    for (const QTabBar* tabBar : window->findChildren<QTabBar*>(Qt::FindDirectChildrenOnly)) {
        if (!tabBar->isVisible()) continue;
        for (int i = 0; i < tabBar->count(); ++i) {
            if (tabBar->tabData(i).value<quintptr>() == reinterpret_cast<quintptr>(dock)) return tabBar;
        }
    }
    return nullptr;
}
}  // namespace

SidePanel::SidePanel(const QString& title, QWidget* parent) : QDockWidget(title, parent) {
    setObjectName(title);
    // A floating panel has no spot to collapse into: it shows all of itself.
    connect(this, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        if (floating && expand()) Q_EMIT collapsedChanged(false);
    });
}

QMainWindow* SidePanel::dockedIn() const {
    auto* window = qobject_cast<QMainWindow*>(parentWidget());
    if (!window || isFloating()) return nullptr;
    return window->dockWidgetArea(const_cast<SidePanel*>(this)) != Qt::NoDockWidgetArea ? window : nullptr;
}

void SidePanel::setCollapsed(bool collapsed, int expandedSize) {
    QMainWindow* window = dockedIn();
    if (!window || collapsed == m_collapsed) return;

    // The panels sharing this spot, and the one on show: its size is theirs.
    // The spot is what QMainWindow::resizeDocks() takes on expanding: the
    // panel and, when tabbed, its tab bar.
    QList<SidePanel*> group{this};
    for (QDockWidget* dock : window->tabifiedDockWidgets(this)) {
        if (auto* panel = qobject_cast<SidePanel*>(dock)) group.append(panel);
    }
    SidePanel* shown = this;
    QRect spot = geometry();
    if (const QTabBar* tabBar = group.size() > 1 ? tabBarOf(window, this) : nullptr) {
        // The current tab's: Qt leaves the other tabs' panels visible, out of
        // the way, with whatever size they had.
        const auto current = tabBar->tabData(tabBar->currentIndex()).value<quintptr>();
        for (SidePanel* panel : group) {
            if (reinterpret_cast<quintptr>(panel) == current) shown = panel;
        }
        spot = shown->geometry() | tabBar->geometry();
    }

    QList<SidePanel*> changed;
    if (collapsed) {
        // To a title bar's height, which at the left or right edge frees
        // space only for panels stacked above or below. Alone there, to a
        // vertical title bar's width instead, which gives it to the
        // viewports.
        Qt::Orientation along = Qt::Vertical;
        const Qt::DockWidgetArea area = window->dockWidgetArea(this);
        if ((area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea) && group.size() == 1) {
            along = Qt::Horizontal;
            for (QDockWidget* dock : window->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly)) {
                // isHidden(), not isVisible(): a saved layout is restored
                // before the window is shown.
                if (dock != this && !dock->isHidden() && !dock->isFloating() && window->dockWidgetArea(dock) == area) {
                    along = Qt::Vertical;
                }
            }
        }
        const int size = expandedSize >= 0 ? expandedSize : along == Qt::Vertical ? spot.height() : spot.width();
        for (SidePanel* panel : group) {
            if (panel->m_collapsed) continue;
            panel->collapse(along, size);
            changed.append(panel);
        }
    } else {
        const Qt::Orientation along = shown->m_collapsed ? shown->m_collapsedAlong : m_collapsedAlong;
        const int size = shown->m_collapsed ? shown->m_expandedSize : m_expandedSize;
        for (SidePanel* panel : group) {
            if (panel->expand()) changed.append(panel);
        }
        // The dock area keeps its collapsed size until told otherwise, which
        // works once the layout has seen the content's limits lifted.
        QTimer::singleShot(0, window, [panel = QPointer<SidePanel>(shown), along, size]() {
            QMainWindow* window = panel ? panel->dockedIn() : nullptr;
            if (window && size > 0) window->resizeDocks({panel.data()}, {size}, along);
        });
    }
    for (SidePanel* panel : changed) Q_EMIT panel->collapsedChanged(collapsed);
}

void SidePanel::collapse(Qt::Orientation orientation, int expandedSize) {
    m_collapsed = true;
    m_collapsedAlong = orientation;
    m_expandedSize = expandedSize;
    // The content stays, squeezed to nothing: hidden, it would also take the
    // panel's width (or height) down to the title's.
    if (QWidget* content = widget()) {
        m_contentMinimum = content->minimumSize();
        m_contentMaximum = content->maximumSize();
        if (orientation == Qt::Vertical) {
            content->setMinimumHeight(0);
            content->setMaximumHeight(0);
        } else {
            content->setMinimumWidth(0);
            content->setMaximumWidth(0);
        }
        // Typing would go on into a field nobody can see.
        if (QWidget* focus = QApplication::focusWidget(); focus && content->isAncestorOf(focus)) focus->clearFocus();
    }
    m_addedVerticalTitleBar = orientation == Qt::Horizontal && !features().testFlag(DockWidgetVerticalTitleBar);
    if (m_addedVerticalTitleBar) setFeatures(features() | DockWidgetVerticalTitleBar);
}

bool SidePanel::expand() {
    if (!m_collapsed) return false;
    m_collapsed = false;
    if (QWidget* content = widget()) {
        content->setMinimumSize(m_contentMinimum);
        content->setMaximumSize(m_contentMaximum);
    }
    if (m_addedVerticalTitleBar) setFeatures(features() & ~DockWidgetVerticalTitleBar);
    m_addedVerticalTitleBar = false;
    return true;
}

bool SidePanel::event(QEvent* event) {
    // Anywhere outside the content is the title bar (or the frame around
    // it). Docked, double-clicking it collapses instead of floating the panel
    // (its float button still does that).
    if (event->type() == QEvent::MouseButtonDblClick && dockedIn()) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QWidget* content = widget();
        if (mouse->button() == Qt::LeftButton &&
            !(content && content->geometry().contains(mouse->position().toPoint()))) {
            toggleCollapsed();
            m_ignoreDrag = true;
            return true;
        }
    }
    // The double-click's second press is still held, and the title bar has
    // just moved away from under the cursor: Qt would take the next mouse
    // move for a drag of the title bar, unplug the panel and drop it
    // elsewhere. The release goes through, to end Qt's drag-to-be.
    if (m_ignoreDrag) {
        if (event->type() == QEvent::MouseMove) return true;
        if (event->type() == QEvent::MouseButtonRelease) m_ignoreDrag = false;
    }
    return QDockWidget::event(event);
}

}  // namespace usdcc::ui
