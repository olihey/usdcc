#pragma once

#include "usdcc/core/export.h"

#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QRect>
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

class LogPanel;
class PanelMaximizer;
class ToastLogSink;
class ToastManager;
class SidePanel;
class TitleBar;

// QMainWindow shell hosting the Qt Advanced Docking System, with the side
// panels (so far the LogPanel) docked around it. The composition root (main.cpp) adds the real, USD-dependent
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
    LogPanel* logPanel() const;

    // The window's toast notifications (docs/NOTIFICATIONS.md). Log records
    // marked as toasts (USDCC_LOG_TOAST) show here too, from the moment the
    // window exists; warnings and errors with a Show Log button.
    ToastManager* toasts() const;

    // Adds a side panel from outside usdcc_ui (the Script Editor panel, panels made in
    // Python), lists it in the View menu, and takes ownership of it. `area` is the window
    // edge it's at until moved: collapsed there when the window isn't up yet, docked along
    // it otherwise. A saved layout places it by its objectName, like the built-in ones:
    // either through restoreLayout(), for a panel added before it, or, for one added later
    // (from a script), by putting it back where the layout last had it instead of in
    // `area`. A panel that is deleted leaves the menu. Adding the same panel again does
    // nothing.
    void addSidePanel(SidePanel* panel, Qt::DockWidgetArea area);
    // Takes it out, View menu entry included, and deletes it later. Adding another
    // with the same object name puts that one where this one was.
    void removeSidePanel(SidePanel* panel);

    // The side panels, in the order added.
    const QList<SidePanel*>& sidePanels() const { return m_sidePanels; }

    // Puts `widget` in the title bar, right of the title (TitleBar::addTrailingWidget()),
    // clickable rather than dragging the window.
    void addTitleBarWidget(QWidget* widget);

    // Puts `bar` under the dock area, above the bottom side panels, as wide as the dock
    // area: the timeline (docs/ANIMATION.md §4.3). The window takes ownership. The View
    // menu gets an entry named `name` that shows and hides it, which the saved layout
    // remembers; so call this before buildViewMenu() and restoreLayout(). One bar only.
    void setBottomBar(QWidget* bar, const QString& name);
    QWidget* bottomBar() const { return m_bottomBar; }
    // View > `name`: checked while the bar shows. Null without a bar.
    QAction* bottomBarAction() const { return m_bottomBarAction; }

    // Whether layoutSettled() has been emitted.
    bool isLayoutSettled() const { return m_layoutSettled; }

Q_SIGNALS:
    // Once, after the window was shown and has stopped resizing for a moment.
    // Until then Qt may still re-apply the layout restoreLayout() restored
    // (it does so while a window opening maximized gets its final size), which
    // drops dock widgets added in between; so code that adds panels after
    // start-up (startup scripts) waits for this.
    void layoutSettled();
    // While the window closes, once every close check has agreed and while it's
    // still on screen, before its layout is saved: the last chance to look at it
    // (the Last session is saved from here, docs/SESSIONS.md §4.5).
    void closing();

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

    // The same layout as a session holds it (docs/SESSIONS.md §2.3): geometry and
    // window state as base64, the dock layout as XML, and the side panels' places.
    QJsonObject layoutState() const;
    // Restores one from layoutState(), the panels having been made first (as for
    // restoreLayout()). `withGeometry`: the window's size and position too (at start);
    // otherwise only whether it's maximised. Then floating panels are put back on a
    // screen (§5.2).
    void restoreLayoutState(const QJsonObject& state, bool withGeometry);

    // Moves every floating panel (view or side panel) that isn't wholly on a screen
    // onto the nearest one, shrinking it to fit. Done after every restore.
    void keepFloatingPanelsOnScreen();

    // Builds the "View" menu from m_sidePanels, one entry per instance —
    // a future LogPanel/ScriptEditorSidePanel/USDASidePanel just needs to
    // be appended to m_sidePanels before this runs. Public (rather than
    // called from the constructor) so the composition root (main.cpp)
    // controls menu-bar ordering — it calls this after adding its own
    // "File" menu, so "File" stays first.
    void buildViewMenu();

    // View > Full Screen (F11), under UI Scale: checked while the window is full
    // screen. Leaving it goes back to maximized if the window was, else normal.
    void setFullScreen(bool fullScreen);
    QAction* fullScreenAction() const { return m_fullScreenAction; }
    // The View menu, once buildViewMenu() has made it.
    QMenu* viewMenu() const { return m_viewMenu; }
    // The menu a right-click on a side panel's title bar or a toolbar shows:
    // the View menu's side panels, bottom bar and toolbars, in its order, so
    // the two list the same things. Qt's own lists dock widgets and toolbars
    // only, without the bottom bar (the timeline). The caller owns it.
    QMenu* createPopupMenu() override;
    // Whether the window is full screen, from the native window's state.
    bool isFullScreenNow() const;

    // Asked, in the order added, before the window closes (after the side
    // panels' canClose()); one returning false keeps it open. main.cpp adds
    // the unsaved-layers question (docs/LAYER_EDITOR.md §5).
    void addCloseCheck(std::function<bool()> check) { m_closeChecks.push_back(std::move(check)); }

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void saveLayout() const;
    // Ticks View > Full Screen as the window is.
    void syncFullScreenAction();
    // After restoring `dockLayout`: opens the panels it doesn't name, which
    // ADS would otherwise leave hidden for good (one deleted when the layout
    // was saved, then made again by the app at startup).
    // `closed`: the side panels that were closed before.
    void showPanelsMissingFrom(const QByteArray& dockLayout, const QSet<SidePanel*>& closed);
    // The dock layout, window state and places (below), shared by restoreLayout() and
    // restoreLayoutState().
    void applyLayout(const QByteArray& windowState, const QVariantMap& places, const QByteArray& dockLayout);
    // The places of the side panels there are, and the ones known of the rest.
    QVariantMap sidePanelPlaces() const;
    friend class SidePanel;
    // It's being deleted, or removed.
    void forgetSidePanel(SidePanel* panel);

    ads::CDockManager* m_dockManager = nullptr;
    // The central widget: the dock manager, with the bottom bar under it.
    QWidget* m_center = nullptr;
    QWidget* m_bottomBar = nullptr;
    QAction* m_bottomBarAction = nullptr;
    PanelMaximizer* m_panelMaximizer = nullptr;
    void setUpWindowAgent();

    QWK::WidgetWindowAgent* m_windowAgent = nullptr;  // null where there's no native window to adapt
    TitleBar* m_titleBar = nullptr;
    QList<SidePanel*> m_sidePanels;
    std::vector<std::function<bool()>> m_closeChecks;
    LogPanel* m_logPanel = nullptr;
    ToastManager* m_toasts = nullptr;
    std::shared_ptr<ToastLogSink> m_toastSink;
    bool m_layoutRestored = false;
    // objectName -> SidePanel::place(), of side panels gone or not added yet: the saved
    // layout's, and the removed ones'.
    QVariantMap m_places;
    bool m_layoutSettled = false;
    QTimer m_settleTimer;
    // Set by buildViewMenu(); panels added later go in before
    // m_viewMenuPanelsEnd (the separator above the toolbars, if any).
    QMenu* m_viewMenu = nullptr;
    QAction* m_viewMenuPanelsEnd = nullptr;
    QAction* m_fullScreenAction = nullptr;
    bool m_maximizedBeforeFullScreen = false;
    // Windows: the window's normal place (workspace coordinates, setCoords()) while
    // full screen from maximized stands it in for the maximized rect (setFullScreen()).
    QRect m_normalBeforeFullScreen;
    bool m_windowStatesWatched = false;
};

// `rect` moved, and shrunk if it must, so that it lies wholly inside one of
// `screens` (their available areas): the one it overlaps most, or else the nearest.
// Unchanged if it's inside one already, or there are none.
USDCC_UI_API QRect fitOnScreens(const QRect& rect, const QList<QRect>& screens);

}  // namespace usdcc::ui
