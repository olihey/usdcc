#pragma once

#include "usdcc/core/export.h"

#include <QList>
#include <QMainWindow>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <vector>

class QAction;
class QMenu;
class QMenuBar;
class QTabBar;

namespace ads {
class CDockManager;
}

namespace QWK {
class WidgetWindowAgent;
}

namespace usdcc::ui {

class LogSidePanel;
class PanelMaximizer;
class ToastLogSink;
class ToastManager;
class SidePanel;
class TitleBar;

// QMainWindow shell hosting the Qt Advanced Docking System, with the side
// panels (so far the LogSidePanel) docked around it. The composition root (main.cpp) adds the real, USD-dependent
// ViewPanel subclasses (Viewport/Outliner/Attributes — see docs/PLAN.md
// section 4 and section 7) directly via dockManager(), including
// establishing the central dock area itself.
//
// Frameless: instead of the operating system's title bar, the window shows a
// TitleBar (logo, menu bar, title, caption buttons) driven by QWindowKit,
// which supplies the native-feeling drag, resize, snap and shadow behaviour.
class USDCC_UI_API MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // The window's menu bar, which lives inside the title bar. Use this, not
    // QMainWindow::menuBar(): that would see the title bar isn't a QMenuBar
    // and replace it with a fresh, empty one.
    QMenuBar* mainMenuBar() const;

    // Lets the app's composition root (main.cpp) add panels directly, rather
    // than MainWindow needing to know about a specific StageManager instance
    // (or any particular set of panels) itself — see main.cpp.
    ads::CDockManager* dockManager() const;

    // Gives one dock area all the room for a while; ViewPanels use it for their
    // tab's double-click and context menu. Always exists.
    PanelMaximizer* panelMaximizer() const;

    // The Log side panel; always exists.
    LogSidePanel* logPanel() const;

    // The window's toast notifications (docs/NOTIFICATIONS.md). Log records
    // marked as toasts (USDCC_LOG_TOAST) show here too, from the moment the
    // window exists; warnings and errors with a Show Log button.
    ToastManager* toasts() const;

    // Docks a side panel from outside usdcc_ui (the Script Editor panel, panels
    // made in Python) in `area`, lists it in the View menu, and takes
    // ownership of it. A saved layout places it by its objectName, like the
    // built-in ones: either through restoreLayout(), for a panel added before
    // it, or, for one added later (from a script), by putting it back where
    // the layout last had it instead of in `area`. A panel that is deleted
    // leaves the menu. Adding the same panel again does nothing.
    //
    // Between show() and layoutSettled(), don't add one: see there.
    void addSidePanel(SidePanel* panel, Qt::DockWidgetArea area);
    // Takes it out, View menu entry included, and deletes it later. Adding another
    // with the same object name puts that one where this one was.
    void removeSidePanel(SidePanel* panel);

    // Whether layoutSettled() has been emitted.
    bool isLayoutSettled() const { return m_layoutSettled; }

Q_SIGNALS:
    // Once, after the window was shown and has stopped resizing for a moment.
    // Until then Qt may still re-apply the layout restoreLayout() restored
    // (it does so while a window opening maximized gets its final size), which
    // drops dock widgets added in between; so code that adds panels after
    // start-up (startup scripts) waits for this.
    void layoutSettled();

public:

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

    // Builds the "View" menu from m_sidePanels, one entry per instance —
    // a future LogSidePanel/ScriptEditorSidePanel/USDASidePanel just needs to
    // be appended to m_sidePanels before this runs. Public (rather than
    // called from the constructor) so the composition root (main.cpp)
    // controls menu-bar ordering — it calls this after adding its own
    // "File" menu, so "File" stays first.
    void buildViewMenu();

    // Asked, in the order added, before the window closes (after the side
    // panels' canClose()); one returning false keeps it open. main.cpp adds
    // the unsaved-layers question (docs/LAYER_EDITOR.md §5).
    void addCloseCheck(std::function<bool()> check) { m_closeChecks.push_back(std::move(check)); }

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Watches the tab bars of side panels sharing a spot: double-clicking a
    // tab collapses the panels (SidePanel::setCollapsed()).
    void childEvent(QChildEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void saveLayout() const;
    // After restoring `dockLayout`: opens the panels it doesn't name, which
    // ADS would otherwise leave hidden for good (one deleted when the layout
    // was saved, then made again by the app at startup).
    void showPanelsMissingFrom(const QByteArray& dockLayout);
    SidePanel* sidePanelOfTab(const QTabBar* tabBar, int index) const;
    // Collapses `panel` if the saved layout had it collapsed.
    void restoreCollapsed(SidePanel* panel);

    ads::CDockManager* m_dockManager = nullptr;
    PanelMaximizer* m_panelMaximizer = nullptr;
    QWK::WidgetWindowAgent* m_windowAgent = nullptr;
    TitleBar* m_titleBar = nullptr;
    QList<SidePanel*> m_sidePanels;
    std::vector<std::function<bool()>> m_closeChecks;
    LogSidePanel* m_logPanel = nullptr;
    ToastManager* m_toasts = nullptr;
    std::shared_ptr<ToastLogSink> m_toastSink;
    bool m_layoutRestored = false;
    // objectName -> the size to expand to, of the side panels the saved
    // layout had collapsed.
    QVariantMap m_savedCollapsed;
    bool m_layoutSettled = false;
    QTimer m_settleTimer;
    // Set by buildViewMenu(); panels added later go in before
    // m_viewMenuPanelsEnd (the separator above the toolbars, if any).
    QMenu* m_viewMenu = nullptr;
    QAction* m_viewMenuPanelsEnd = nullptr;
};

}  // namespace usdcc::ui
