#include "usdcc/ui/side_panel.h"

#include "usdcc/ui/main_window.h"

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QSplitter>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace usdcc::ui {

namespace {

// Of the window, until the user says otherwise.
constexpr double kDefaultShare = 0.25;

bool isVerticalEdge(Qt::DockWidgetArea area) { return area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea; }

ads::DockWidgetArea adsArea(Qt::DockWidgetArea area) {
    switch (area) {
        case Qt::LeftDockWidgetArea:
            return ads::LeftDockWidgetArea;
        case Qt::RightDockWidgetArea:
            return ads::RightDockWidgetArea;
        case Qt::TopDockWidgetArea:
            return ads::TopDockWidgetArea;
        default:
            return ads::BottomDockWidgetArea;
    }
}

constexpr std::pair<Qt::DockWidgetArea, const char*> kAreaNames[] = {{Qt::LeftDockWidgetArea, "left"},
                                                                     {Qt::RightDockWidgetArea, "right"},
                                                                     {Qt::TopDockWidgetArea, "top"},
                                                                     {Qt::BottomDockWidgetArea, "bottom"}};

// Whether ADS shows anything in it: also right before the window is shown, when nothing in it
// counts as visible yet.
bool showsPanels(const QWidget* widget) {
    if (auto* area = qobject_cast<const ads::CDockAreaWidget*>(widget)) return area->openDockWidgetsCount() > 0;
    auto* splitter = qobject_cast<const QSplitter*>(widget);
    if (!splitter) return false;
    for (int i = 0; i < splitter->count(); ++i) {
        if (showsPanels(splitter->widget(i))) return true;
    }
    return false;
}

}  // namespace

SidePanel::SidePanel(const QString& title, QWidget* parent) : DockPanel(title, parent) { setObjectName(title); }

SidePanel::~SidePanel() {
    if (m_window) m_window->forgetSidePanel(this);
}

void SidePanel::setWidget(QWidget* widget) { CDockWidget::setWidget(widget, ads::CDockWidget::ForceNoScrollArea); }

void SidePanel::show() {
    if (!dockManager()) {
        QWidget::show();
    } else if (!dockAreaWidget()) {
        collapseAt(edge());
    } else if (isClosed()) {
        toggleView(true);
    }
}

void SidePanel::hide() {
    if (!dockManager()) {
        QWidget::hide();
    } else if (!isClosed()) {
        toggleView(false);
    }
}

bool SidePanel::isHidden() const { return dockManager() ? isClosed() : QWidget::isHidden(); }

void SidePanel::reveal() {
    show();
    raise();
}

bool SidePanel::holdsOnlySidePanels(const QWidget* widget) {
    if (!showsPanels(widget)) return false;
    if (auto* area = qobject_cast<const ads::CDockAreaWidget*>(widget)) {
        const QList<ads::CDockWidget*> open = area->openedDockWidgets();
        return std::all_of(open.begin(), open.end(), [](ads::CDockWidget* dock) { return qobject_cast<SidePanel*>(dock); });
    }
    auto* splitter = qobject_cast<const QSplitter*>(widget);
    for (int i = 0; splitter && i < splitter->count(); ++i) {
        if (showsPanels(splitter->widget(i)) && !holdsOnlySidePanels(splitter->widget(i))) return false;
    }
    return true;
}

SidePanel::Edge SidePanel::dockedEdge() const {
    if (isClosed() || isAutoHide() || !dockAreaWidget() || dockContainer() != dockManager()) return {};
    // The splitters around it, the outermost first.
    QList<QSplitter*> around;
    QWidget* parent = dockAreaWidget()->parentWidget();
    while (auto* splitter = qobject_cast<QSplitter*>(parent)) {
        around.prepend(splitter);
        parent = splitter->parentWidget();
    }
    for (int level = 0; level < around.size(); ++level) {
        QSplitter* splitter = around[level];
        QWidget* mine = level + 1 < around.size() ? static_cast<QWidget*>(around[level + 1]) : dockAreaWidget();
        // Where it is among the children, and the first and last that aren't side panels.
        int index = -1;
        int first = -1;
        int last = -1;
        int shown = 0;
        for (int i = 0; i < splitter->count(); ++i) {
            QWidget* child = splitter->widget(i);
            if (!showsPanels(child)) continue;
            ++shown;
            if (child == mine) index = i;
            if (holdsOnlySidePanels(child)) continue;
            if (first < 0) first = i;
            last = i;
        }
        if (shown < 2) continue;
        if (!holdsOnlySidePanels(mine)) {
            // Further in: at an edge still, if all that's beside it out here are side panels.
            if (first != last) return {};
            continue;
        }
        const bool horizontal = splitter->orientation() == Qt::Horizontal;
        if (index < first || (first < 0 && index == 0)) {
            return {horizontal ? Qt::LeftDockWidgetArea : Qt::TopDockWidgetArea, mine};
        }
        if (index > last) return {horizontal ? Qt::RightDockWidgetArea : Qt::BottomDockWidgetArea, mine};
        return {};  // between view panels
    }
    return {m_lastArea, nullptr};  // alone: nothing to be at an edge of
}

Qt::DockWidgetArea SidePanel::area() const {
    if (isClosed()) return Qt::NoDockWidgetArea;
    return isCollapsed() ? collapsedArea() : dockedEdge().area;
}

Qt::DockWidgetArea SidePanel::edge() const {
    const Qt::DockWidgetArea at = area();
    return at != Qt::NoDockWidgetArea ? at : m_lastArea;
}

void SidePanel::setCollapsed(bool collapsed) {
    if (collapsed) {
        collapseAt(edge());
    } else if (isAutoHide()) {
        dockAt(edge());
    }
}

void SidePanel::collapseAt(Qt::DockWidgetArea area) {
    if (!dockManager() || isFloating()) return;
    m_lastArea = area;
    DockPanel::collapseAt(area, dockManager());
}

void SidePanel::dockAt(Qt::DockWidgetArea area) {
    ads::CDockManager* manager = dockManager();
    if (!manager) return;
    m_lastArea = area;
    if (!dockAreaWidget()) collapseAt(area);  // it has no place to open in
    if (isClosed()) toggleView(true);
    if (!isAutoHide() && !isFloating() && this->area() == area) return;
    leavingTabBar();
    if (isAutoHide()) {
        setAutoHide(false);
        leavingTabBar();
    }

    ads::CDockAreaWidget* with = nullptr;
    if (m_window) {
        for (SidePanel* other : m_window->sidePanels()) {
            if (other == this || other->isClosed() || other->isAutoHide() || other->area() != area) continue;
            if (other->dockAreaWidget() != dockAreaWidget()) with = other->dockAreaWidget();
        }
    }
    if (with) {
        manager->addDockWidgetTabToArea(this, with);
    } else {
        manager->addDockWidget(adsArea(area), this);
    }
    leavingTabBar(true);
    if (isClosed()) toggleView(true);
    if (!with) resizeToShare(kDefaultShare);
}

void SidePanel::resizeToShare(double share) {
    const Edge at = dockedEdge();
    QSplitter* splitter = at.child ? qobject_cast<QSplitter*>(at.child->parentWidget()) : nullptr;
    if (!splitter || share <= 0.0 || share >= 1.0) return;
    const QSize window = dockManager()->window()->size();
    const QList<int> current = splitter->sizes();
    int total = 0;
    int others = 0;
    for (int i = 0; i < splitter->count(); ++i) {
        if (!showsPanels(splitter->widget(i))) continue;
        total += current[i];
        if (splitter->widget(i) != at.child) others += current[i];
    }
    const bool laidOut = total >= 100;
    const int want = laidOut ? std::min(total - 1, int(share * (isVerticalEdge(at.area) ? window.width() : window.height())))
                             : int(share * 10000);
    if (!laidOut) total = 10000;
    QList<int> sizes;
    for (int i = 0; i < splitter->count(); ++i) {
        QWidget* child = splitter->widget(i);
        // Only the view panels grow and shrink with the window.
        splitter->setStretchFactor(i, !showsPanels(child) || holdsOnlySidePanels(child) ? 0 : 1);
        if (!showsPanels(child)) {
            sizes << 0;
        } else if (child == at.child) {
            sizes << want;
        } else {
            sizes << std::max(1, others > 0 ? int(double(total - want) * current[i] / others) : total - want);
        }
    }
    splitter->setSizes(sizes);
}

QVariantMap SidePanel::place() const {
    QVariantMap place;
    place.insert(QStringLiteral("state"), isClosed()      ? QStringLiteral("hidden")
                                          : isCollapsed() ? QStringLiteral("collapsed")
                                                          : QStringLiteral("shown"));
    const Edge at = dockedEdge();
    const Qt::DockWidgetArea where = isClosed() ? m_lastArea : area();
    for (const auto& [area, name] : kAreaNames) {
        if (area == where) place.insert(QStringLiteral("area"), QString::fromLatin1(name));
    }
    if (at.child && dockManager()) {
        const QSize window = dockManager()->window()->size();
        const double share = isVerticalEdge(at.area) ? double(at.child->width()) / std::max(1, window.width())
                                                     : double(at.child->height()) / std::max(1, window.height());
        if (share > 0.0 && share < 1.0) place.insert(QStringLiteral("size"), share);
    }
    return place;
}

void SidePanel::setPlace(const QVariantMap& place) {
    const QString state = place.value(QStringLiteral("state"), QStringLiteral("shown")).toString();
    const bool collapsed = state == QLatin1String("collapsed");
    const QString name = place.value(QStringLiteral("area")).toString();
    const bool floating = isFloating();
    for (const auto& [area, areaName] : kAreaNames) {
        if (name == QLatin1String(areaName)) m_lastArea = area;
    }
    if (state == QLatin1String("hidden")) {
        hide();
        return;
    }
    if (floating) {
        show();
        return;
    }
    if (!name.isEmpty()) {
        const Qt::DockWidgetArea area = m_lastArea;
        if (collapsed) {
            collapseAt(area);
        } else {
            dockAt(area);
        }
    } else {
        show();
        setCollapsed(collapsed);
    }
    const double size = place.value(QStringLiteral("size")).toDouble();
    if (size <= 0.0 || isCollapsed()) return;
    // Now, and once the window has laid it out.
    resizeToShare(size);
    QTimer::singleShot(0, this, [this, size]() { resizeToShare(size); });
}

}  // namespace usdcc::ui
