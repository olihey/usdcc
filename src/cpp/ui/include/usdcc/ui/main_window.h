#pragma once

#include <QMainWindow>

namespace ads {
class CDockManager;
}

namespace usdcc::ui {

// QMainWindow shell hosting the Qt Advanced Docking System. Panel
// registration currently instantiates a stand-in SidePanel to prove the
// docking wiring end to end; a real LogSidePanel lands in a later milestone
// (M8). The composition root (main.cpp) adds the real, USD-dependent
// ViewPanel subclasses (Viewport/Outliner/Attributes — see docs/PLAN.md
// section 4 and section 7) directly via dockManager(), including
// establishing the central dock area itself.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Lets the app's composition root (main.cpp) add panels directly, rather
    // than MainWindow needing to know about a specific StageManager instance
    // (or any particular set of panels) itself — see main.cpp.
    ads::CDockManager* dockManager() const;

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
};

}  // namespace usdcc::ui
