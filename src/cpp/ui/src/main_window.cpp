#include "usdcc/ui/main_window.h"

#include <DockManager.h>
#include <QAbstractButton>
#include <QCloseEvent>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QToolBar>
#include <QTextEdit>

#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/title_bar.h"

#include <QWKWidgets/widgetwindowagent.h>

namespace usdcc::ui {

namespace {
constexpr auto kGeometryKey = "MainWindow/geometry";
constexpr auto kDockLayoutKey = "MainWindow/dockLayout";
// QMainWindow::saveState(): where its toolbars and side-panel dock widgets are
// docked, floating or hidden. Bump the version if their set changes in a way an
// old saved state would mishandle (restoreState() then ignores it).
constexpr auto kWindowStateKey = "MainWindow/windowState";
constexpr int kWindowStateVersion = 1;

// A plain INI file (rather than QSettings' native format, which is the
// Windows registry under HKCU\Software\usdcc\usdcc) so it's a real file a
// developer can find, inspect, or delete by hand instead of hunting through
// regedit — useful during active development, when the on-disk panel set
// changes often enough that a stale saved layout is a routine annoyance
// rather than a rare edge case.
QSettings layoutSettings() {
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    return QSettings(configDir + "/usdcc.ini", QSettings::IniFormat);
}
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("usdcc");
    resize(1280, 800);

    // Replace the OS title bar with our own (see TitleBar). QWindowKit makes
    // the window frameless and adds back native behaviour; its agent must be
    // set up before the window is first shown, and it requires this attribute
    // so the window's native handle isn't forced onto its ancestors.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_windowAgent = new QWK::WidgetWindowAgent(this);
    m_windowAgent->setup(this);

    m_titleBar = new TitleBar(this);
    m_windowAgent->setTitleBar(m_titleBar);  // the bar's empty space drags / double-click-maximizes
    // Interactive widgets inside the bar must opt out of that dragging. The
    // caption buttons are registered as the window's system buttons instead,
    // which also gives the maximize button Windows 11's snap-layout flyout.
    m_windowAgent->setHitTestVisible(m_titleBar->menuBar(), true);
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::WindowIcon, m_titleBar->logoWidget());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Minimize, m_titleBar->minimizeButton());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Maximize, m_titleBar->maximizeButton());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Close, m_titleBar->closeButton());
#ifdef Q_OS_MAC
    // macOS keeps its native traffic-light buttons (untested here).
    m_titleBar->minimizeButton()->hide();
    m_titleBar->maximizeButton()->hide();
    m_titleBar->closeButton()->hide();
#endif
    setMenuWidget(m_titleBar);

    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    m_dockManager = new ads::CDockManager(this);
    theme::applyToDockManager(m_dockManager);
    setCentralWidget(m_dockManager);

    // Stand-in proving SidePanel + layout persistence work end to end;
    // replaced by a real LogSidePanel in a later milestone (M8). The
    // equivalent ViewPanel stand-in was retired once M3/M4 gave the app real
    // ViewPanel subclasses (Viewport/Outliner/Attributes) that prove the
    // same thing — the composition root (main.cpp) establishes the center
    // dock area itself once USD is available, since a placeholder here would
    // just be a second, unused "empty center area" when it isn't.
    auto* sidePanelStandIn = new SidePanel("Log", this);
    sidePanelStandIn->setWidget(new QTextEdit(sidePanelStandIn));
    addDockWidget(Qt::BottomDockWidgetArea, sidePanelStandIn);
    m_sidePanels.append(sidePanelStandIn);

    // Deliberately not calling restoreLayout() here — see its declaration in
    // main_window.h for why. The composition root calls it once every panel
    // exists.
}

ads::CDockManager* MainWindow::dockManager() const { return m_dockManager; }

QMenuBar* MainWindow::mainMenuBar() const { return m_titleBar->menuBar(); }

void MainWindow::closeEvent(QCloseEvent* event) {
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::restoreLayout() {
    QSettings settings = layoutSettings();
    if (settings.contains(kGeometryKey)) {
        restoreGeometry(settings.value(kGeometryKey).toByteArray());
    }
    // Toolbars and side panels, which are all created by now (see this
    // method's declaration), keyed by their objectNames.
    if (settings.contains(kWindowStateKey)) {
        restoreState(settings.value(kWindowStateKey).toByteArray(), kWindowStateVersion);
    }
    if (settings.contains(kDockLayoutKey)) {
        m_dockManager->restoreState(settings.value(kDockLayoutKey).toByteArray());
    }
}

void MainWindow::saveLayout() const {
    QSettings settings = layoutSettings();
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kWindowStateKey, saveState(kWindowStateVersion));
    settings.setValue(kDockLayoutKey, m_dockManager->saveState());
}

void MainWindow::buildViewMenu() {
    QMenu* viewMenu = mainMenuBar()->addMenu(tr("&View"));
    // QDockWidget already provides a checkable show/hide QAction, kept in
    // sync with the dock widget's actual visibility — no custom toggle slot
    // needed.
    for (SidePanel* panel : m_sidePanels) {
        viewMenu->addAction(panel->toggleViewAction());
    }

    // Toolbars can be hidden (and dragged anywhere), so list them too: this is
    // the way back to one that was hidden. Whoever added them (main.cpp) must
    // have done so before this runs.
    const QList<QToolBar*> toolBars = findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly);
    if (!toolBars.isEmpty()) {
        viewMenu->addSeparator();
        for (QToolBar* toolBar : toolBars) {
            viewMenu->addAction(toolBar->toggleViewAction());
        }
    }
}

}  // namespace usdcc::ui
