#pragma once

#include "usdcc/core/export.h"

#include "usdcc/ui/dock_panel.h"

#include <QString>
#include <QVariantMap>

namespace usdcc::ui {

class MainWindow;

// Base class for dock panels that show no stage (the Log, the Script Editor, Peers, panels
// made in Python, ...). There is one of each, which the user closes and opens again; a stage
// or a workspace never deletes it. MainWindow::addSidePanel() adds one, and the View menu
// lists it.
//
// Docked, it's a dock area or a tab like any other panel. Collapsed, it is a tab on a bar at
// the window's edge (ADS's auto-hide) that slides it out over the other panels.
class USDCC_UI_API SidePanel : public DockPanel {
    Q_OBJECT

public:
    // `title` is its object name too, which layouts and workspaces know it by, until given
    // another one.
    explicit SidePanel(const QString& title, QWidget* parent = nullptr);
    ~SidePanel() override;

    // The content, filling the panel.
    void setWidget(QWidget* widget);

    // Opening and closing it, in place of QWidget's: ADS shows and hides the widget itself.
    void show();
    void hide();
    bool isHidden() const;
    // Opens it and brings it into view: its tab current, or slid out.
    void reveal();

    // Collapses it to the edge it's at (the last one it was at, when it's at none), or docks
    // it there. A closed panel opens; a floating one doesn't collapse.
    void setCollapsed(bool collapsed);
    void toggleCollapsed() { setCollapsed(!isCollapsed()); }

    // The window edge it's collapsed to or docked along. NoDockWidgetArea when it's closed,
    // floating or somewhere among the view panels.
    Qt::DockWidgetArea area() const;
    // Docks it along that edge, open: as a tab of another side panel docked there, else with
    // the whole edge its own.
    void dockAt(Qt::DockWidgetArea area);
    // Collapses it at that edge, open as a tab.
    void collapseAt(Qt::DockWidgetArea area);
    // Docked along an edge: gives it that share of the window's width (left, right) or height
    // (top, bottom); the panels beside it keep their proportions.
    void resizeToShare(double share);

    // Its place as a map: "state" ("shown", "collapsed" or "hidden"), "area" ("left", "right",
    // "top", "bottom"; missing at no edge) and, docked, "size", its share of the window.
    QVariantMap place() const;
    // Missing entries leave that as it is. A floating panel stays floating.
    void setPlace(const QVariantMap& place);

    // Whether `widget`, a dock area or a splitter, shows side panels and nothing else.
    static bool holdsOnlySidePanels(const QWidget* widget);

private:
    friend class MainWindow;

    // The window edge it's docked along, and the splitter's child there that holds it.
    struct Edge {
        Qt::DockWidgetArea area = Qt::NoDockWidgetArea;
        QWidget* child = nullptr;
    };
    Edge dockedEdge() const;
    // The edge it goes to.
    Qt::DockWidgetArea edge() const;

    MainWindow* m_window = nullptr;  // while in it
    Qt::DockWidgetArea m_lastArea = Qt::RightDockWidgetArea;
};

}  // namespace usdcc::ui
