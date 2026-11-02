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

#include "usdcc/core/log.h"
#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/title_bar.h"

#include <QWKWidgets/widgetwindowagent.h>

#include <utility>

namespace usdcc::ui {

namespace {
constexpr auto kGeometryKey = "MainWindow/geometry";
constexpr auto kDockLayoutKey = "MainWindow/dockLayout";
// QMainWindow::saveState(): where its toolbars and side-panel dock widgets are
// docked, floating or hidden. Bump the version if their set changes in a way an
// old saved state would mishandle (restoreState() then ignores it).
constexpr auto kWindowStateKey = "MainWindow/windowState";
constexpr int kWindowStateVersion = 1;

// See appSettings() for why this is a plain INI file.
QSettings layoutSettings() { return appSettings(); }
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("usdcc");
    resize(1280, 800);

    // Qt drops a layout it restored into a window that doesn't fit it yet
    // 150 ms after the last resize (QMainWindowLayout's restoredState); a
    // little longer than that, the layout is the window's own.
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(300);
    connect(&m_settleTimer, &QTimer::timeout, this, [this]() {
        m_layoutSettled = true;
        Q_EMIT layoutSettled();
    });

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

    // The application log (docs/LOGGING.md). Its objectName stays "Log", the
    // name the stand-in it replaced was saved under, so existing layouts
    // still place it. The composition root (main.cpp) establishes the center
    // dock area itself once USD is available.
    m_logPanel = new LogSidePanel(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_logPanel);
    m_sidePanels.append(m_logPanel);

    // Deliberately not calling restoreLayout() here — see its declaration in
    // main_window.h for why. The composition root calls it once every panel
    // exists.
}

ads::CDockManager* MainWindow::dockManager() const { return m_dockManager; }

LogSidePanel* MainWindow::logPanel() const { return m_logPanel; }

void MainWindow::addSidePanel(SidePanel* panel, Qt::DockWidgetArea area) {
    if (!panel || m_sidePanels.contains(panel)) return;
    for (const SidePanel* existing : std::as_const(m_sidePanels)) {
        if (existing->objectName() == panel->objectName()) {
            USDCC_LOG_WARNING("usdcc.layout",
                              "Two side panels are named \"{}\"; the saved layout can only place one of them",
                              panel->objectName().toStdString());
        }
    }
    panel->setParent(this);
    // After restoreLayout(), restoreDockWidget() is what puts a late panel
    // where the saved state had it; it returns false for one it never saw.
    if (!m_layoutRestored || !restoreDockWidget(panel)) {
        addDockWidget(area, panel);
        // setParent() hid it; a window that is already up doesn't show it
        // again (one added before the window is shown appears with it).
        if (isVisible()) panel->show();
    }
    m_sidePanels.append(panel);
    connect(panel, &QObject::destroyed, this, [this, panel]() { m_sidePanels.removeOne(panel); });
    if (m_viewMenu) {
        m_viewMenu->insertAction(m_viewMenuPanelsEnd, panel->toggleViewAction());
    }
}

QMenuBar* MainWindow::mainMenuBar() const { return m_titleBar->menuBar(); }

void MainWindow::closeEvent(QCloseEvent* event) {
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!m_layoutSettled) m_settleTimer.start();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (!m_layoutSettled && isVisible()) m_settleTimer.start();
}

void MainWindow::restoreLayout() {
    QSettings settings = layoutSettings();
    if (settings.contains(kGeometryKey)) {
        restoreGeometry(settings.value(kGeometryKey).toByteArray());
    }
    // Toolbars and side panels, which are all created by now (see this
    // method's declaration), keyed by their objectNames.
    if (settings.contains(kWindowStateKey) &&
        !restoreState(settings.value(kWindowStateKey).toByteArray(), kWindowStateVersion)) {
        USDCC_LOG_WARNING("usdcc.layout", "Ignored the saved toolbar and side panel layout (older version)");
    }
    if (settings.contains(kDockLayoutKey) &&
        !m_dockManager->restoreState(settings.value(kDockLayoutKey).toByteArray())) {
        USDCC_LOG_WARNING("usdcc.layout", "Could not restore the saved panel layout");
    }
    m_layoutRestored = true;
    USDCC_LOG_DEBUG("usdcc.layout", "Restored the window layout from {}", settings.fileName().toStdString());
}

void MainWindow::saveLayout() const {
    QSettings settings = layoutSettings();
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kWindowStateKey, saveState(kWindowStateVersion));
    settings.setValue(kDockLayoutKey, m_dockManager->saveState());
    QStringList panels;
    for (const SidePanel* panel : m_sidePanels) panels << panel->objectName();
    USDCC_LOG_DEBUG("usdcc.layout", "Saved the window layout to {} (side panels: {})", settings.fileName().toStdString(),
                    panels.join(QStringLiteral(", ")).toStdString());
}

void MainWindow::buildViewMenu() {
    QMenu* viewMenu = mainMenuBar()->addMenu(tr("&View"));
    m_viewMenu = viewMenu;
    // QDockWidget already provides a checkable show/hide QAction, kept in
    // sync with the dock widget's actual visibility — no custom toggle slot
    // needed.
    for (SidePanel* panel : m_sidePanels) {
        viewMenu->addAction(panel->toggleViewAction());
    }
    // Side panels added later (addSidePanel()) go in above this; QMenu hides
    // it if nothing follows.
    m_viewMenuPanelsEnd = viewMenu->addSeparator();

    // Toolbars can be hidden (and dragged anywhere), so list them too: this is
    // the way back to one that was hidden. Whoever added them (main.cpp) must
    // have done so before this runs.
    const QList<QToolBar*> toolBars = findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolBar* toolBar : toolBars) {
        viewMenu->addAction(toolBar->toggleViewAction());
    }
    UiScaleController::instance().addMenu(viewMenu);
}

}  // namespace usdcc::ui
