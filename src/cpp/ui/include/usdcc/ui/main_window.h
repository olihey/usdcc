#pragma once

#include <QMainWindow>

namespace ads {
class CDockManager;
class CDockAreaWidget;
}

namespace usdcc::ui {

// QMainWindow shell hosting the Qt Advanced Docking System. Panel
// registration currently instantiates a stand-in SidePanel/ViewPanel to
// prove the docking wiring end to end; real subclasses land across later
// milestones (see docs/PLAN.md section 4 and section 7).
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Lets the app's composition root (main.cpp) add USD-dependent panels
    // without usdcc_ui itself depending on USD — see main.cpp.
    ads::CDockManager* dockManager() const;

    // The dock area occupied by the central stand-in ViewPanel ("Outliner").
    // Panels added elsewhere (e.g. main.cpp's real ViewportViewPanel) should
    // tab into this via CDockManager::addDockWidgetTabToArea() rather than
    // addDockWidget(CenterDockWidgetArea, ...) again, which would instead
    // split the central area into two cramped rows.
    ads::CDockAreaWidget* centerDockArea() const;

    // Restores the saved window geometry/dock layout. Must be called by the
    // composition root (main.cpp) only after *every* panel for this session
    // has been added (including USD-dependent ones like ViewportViewPanel) —
    // calling it any earlier, e.g. from this constructor, applies a saved
    // state captured with a *different* set of panels than currently exist
    // (any panel added later doesn't exist yet as far as ADS's restore is
    // concerned), which can leave ADS's internal layout state inconsistent
    // and cause later-added panels to not display at all. See docs/PLAN.md
    // milestone M3 for how this was diagnosed.
    void restoreLayout();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void saveLayout() const;

    ads::CDockManager* m_dockManager = nullptr;
    ads::CDockAreaWidget* m_centerDockArea = nullptr;
};

}  // namespace usdcc::ui
