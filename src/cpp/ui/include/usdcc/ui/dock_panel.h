#pragma once

#include "usdcc/core/export.h"

#include "usdcc/ui/busy.h"

#include <DockWidget.h>

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPointer>

#include <functional>
#include <memory>

class QMenu;
class QSplitter;

namespace usdcc::ui {

class BusyOverlay;
class OptionsMenuView;
class PanelMaximizer;
class PanelMenuButton;

// Base class for every panel inside the Qt Advanced Docking System container: what a
// panel has whether it shows a stage (ViewPanel) or not (SidePanel). Its content, its
// options and pinning them, being busy, its tab's menu, maximizing, and what a session
// keeps of it. Nothing here knows about stages.
//
// Uses ADS's deprecated (title, parent) CDockWidget constructor rather than
// the (manager, title, parent) one: the manager association still happens
// when the panel is added via CDockManager::addDockWidget().
class USDCC_UI_API DockPanel : public ads::CDockWidget {
    Q_OBJECT

public:
    explicit DockPanel(const QString& title, QWidget* parent = nullptr);
    // Cancels any work the panel is busy with (beginBusy()'s onCancel).
    ~DockPanel() override;

    // "I'm working on something" (docs/PANEL_BUSY.md §3): while the returned handle lives,
    // the panel is busy. After options.showAfterMs it dims, the whole panel, toolbar included,
    // and a card shows `text`, a progress bar, the handle's detail, the time so far and, with
    // options.onCancel, Cancel (Esc too). The panel takes no input meanwhile; its tab stays
    // usable. Several handles may live at once: the newest one's card shows. Closing the
    // panel cancels them all. The work must reach the panel only through the handle or a
    // QPointer: a handle outliving its panel does nothing. May be called from any thread;
    // from another than the UI thread, the panel takes the handle up there.
    std::shared_ptr<Busy> beginBusy(const QString& text, BusyOptions options = {});
    bool isBusy() const;
    // A still of `container`, one of the panel's native windows (createWindowContainer()), to
    // show in its place while the panel is busy: a native window draws over any widget, so it
    // steps aside (§4). The default grabs it from the screen; a viewport returns its last frame.
    virtual QImage busySnapshot(QWidget* container);

    // Brings its tab, or its floating window, to the front, or slides it out when collapsed
    // (in place of QWidget's).
    void raise();

    // Whether it's a tab on the bar at a window edge (ADS's auto-hide), slid out or not. Every
    // panel can be, but those that show a viewport or render (they're native windows).
    bool isCollapsed() const { return isAutoHide(); }
    // The edge it's collapsed to, or NoDockWidgetArea.
    Qt::DockWidgetArea collapsedArea() const;
    // Collapses it at that edge of `manager`, open as a tab: also one that isn't docked yet.
    // A floating one stays.
    void collapseAt(Qt::DockWidgetArea area, ads::CDockManager* manager);
    // Out of there, along that edge, for docking it elsewhere; nothing when it isn't there.
    void leaveEdge();

    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const;

    // The options: the menu, or the panel's own options panel when it has one
    // (createOptionsPanel()). Unpinned, the options button opens them in a popup. Pinned, they
    // stay in a column on the panel's left, a pinned menu as widgets (OptionsMenuView), and the
    // button unpins them. Pinning is remembered for the panel across a restart.
    bool hasOptionsPanel();
    bool isOptionsPinned() const { return m_pinned; }
    // An options panel's popup, while unpinned: null until first opened.
    QWidget* optionsPopup() const { return m_popup; }
    void showOptionsPopup();
    // The pinned column's menu, for tests: null unless a menu is pinned.
    OptionsMenuView* pinnedMenu() const { return m_menuView; }

public Q_SLOTS:
    void setOptionsPinned(bool pinned);
    // What the options show may have changed, outside them: a renderer switched by a script,
    // say. A pinned menu is built again; a popup menu is anyway, each time it opens. Python
    // panels call it as options_changed().
    void notifyOptionsChanged();

public:

    // What a session keeps of the panel besides its type, stage and place
    // (docs/SESSIONS.md §2.4): the camera, the expanded rows, ... Empty by default,
    // and then the panel comes back as it starts. restoreSessionState() gets what
    // saveSessionState() returned, after the panel has its stage and the layout is
    // restored; it skips what no longer fits (a prim that's gone).
    virtual QJsonObject saveSessionState() const { return {}; }
    virtual void restoreSessionState(const QJsonObject& state) { Q_UNUSED(state); }

    // Asked before a workspace switch or the main window closes the panel: false
    // keeps everything as it is (a panel holding something not yet applied asks the
    // user first).
    virtual bool canClose() { return true; }

    // The submenu a panel's tab menu has right after ADS's Close (and Close Others):
    // main.cpp's View > Workspace (WorkspaceController::createMenu()), made with the
    // tab menu as its parent. Empty or returning null, there is none.
    using TabWorkspaceMenu = std::function<QMenu*(QWidget* parent)>;
    static void setTabWorkspaceMenu(TabWorkspaceMenu factory);

Q_SIGNALS:
    // The panel started or stopped being busy (beginBusy()).
    void busyChanged(bool busy);

protected:
    // Extends the context menu of this panel's tab; the base adds Maximize / Restore
    // Layout (see PanelMaximizer; double-clicking the tab does the same).
    virtual void populateContextMenu(QMenu* menu);
    // What goes after everything the panel itself adds to that menu.
    virtual void extendTabMenu(QMenu* menu) { Q_UNUSED(menu); }

    // The options button, made when first asked for; a panel that has options puts it in
    // its toolbar. populateOptionsMenu() fills its menu, built afresh each time it opens.
    PanelMenuButton* optionsButton();
    virtual void populateOptionsMenu(QMenu* menu) { Q_UNUSED(menu); }
    // A widget for the options instead of the menu, or null (the default) for the menu.
    // Asked once, when first needed; the panel owns what it returns.
    virtual QWidget* createOptionsPanel() { return nullptr; }

    // ADS's tab bar scrolls to a tab a moment after it becomes current, by its bare pointer,
    // and an area left empty outlives the panel until the event loop runs. Called before the
    // panel leaves an area, then with `left`, which drops that.
    void leavingTabBar(bool left = false);

    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // The main window's, when this panel can be maximized in it right now.
    PanelMaximizer* maximizer() const;

    QList<QPointer<QObject>> m_tabBars;
    PanelMenuButton* m_optionsButton = nullptr;
    QSplitter* m_split = nullptr;  // the options panel, then the content
    QWidget* m_content = nullptr;
    QWidget* m_optionsHost = nullptr;    // the pinned column
    QWidget* m_optionsBody = nullptr;    // in it, below its header
    QWidget* m_popup = nullptr;          // an unpinned options panel's
    QWidget* m_optionsWidget = nullptr;  // from createOptionsPanel()
    OptionsMenuView* m_menuView = nullptr;  // a pinned menu
    enum class OptionsPanel { NotAsked, None, Made } m_optionsPanel = OptionsPanel::NotAsked;
    bool m_pinned = false;
    bool m_restoredPin = false;
    BusyOverlay* m_busyOverlay = nullptr;  // made by the first beginBusy()
};

}  // namespace usdcc::ui
