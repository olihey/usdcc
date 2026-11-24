#include "usdcc/ui/main_window.h"

#include <DockAreaWidget.h>
#include <AutoHideDockContainer.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <QAbstractButton>
#include <QChildEvent>
#include <QCloseEvent>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QWindow>
#include <QStandardPaths>
#include <QToolBar>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <QGuiApplication>
#include <QJsonArray>
#include <QScreen>

#include <FloatingDockContainer.h>

#include <algorithm>
#include <cstdlib>

#include "usdcc/core/log.h"
#include "usdcc/ui/log_panel.h"
#include "usdcc/ui/panel_maximizer.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/ui/theme.h"
#include "usdcc/ui/title_bar.h"
#include "usdcc/ui/toast_log_sink.h"
#include "usdcc/ui/toast_manager.h"

#include <QWKWidgets/widgetwindowagent.h>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

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
// Where the side panels are (SidePanel::place()), by objectName: for those the dock layout
// can't place, as they're added after it was restored.
constexpr auto kSidePanelPlacesKey = "MainWindow/sidePanelPlaces";
// Whether the bottom bar (the timeline) shows.
constexpr auto kBottomBarKey = "MainWindow/bottomBarVisible";

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
#ifdef Q_OS_MACOS
    // Our title bar lies where the system's would be, which Qt counts as outside the
    // safe area: the layout would leave an empty strip there, above our bar.
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif
    m_titleBar = new TitleBar(this);
#ifdef Q_OS_MACOS
    // QWindowKit takes a window's id for an NSView there, which it only is on Cocoa: the
    // offscreen platform (the tests') numbers its windows.
    const bool frameless = QGuiApplication::platformName() == QLatin1String("cocoa");
#else
    const bool frameless = true;
#endif
    if (frameless) setUpWindowAgent();
    setMenuWidget(m_titleBar);

    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    // Background tabs show their close button too, so a panel can be closed
    // without bringing it to the front first.
    ads::CDockManager::setConfigFlag(ads::CDockManager::AllTabsHaveCloseButton, true);
    // Panels collapse to the window's edge (DockPanel::collapseAt()), but not viewports and
    // render views.
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHideDisabledButtons, true);
    ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::DefaultAutoHideConfig);
    // The dock manager shares the centre with the bottom bar (setBottomBar()).
    m_center = new QWidget(this);
    auto* centerLayout = new QVBoxLayout(m_center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);
    m_dockManager = new ads::CDockManager(m_center);
    theme::applyToDockManager(m_dockManager);
    centerLayout->addWidget(m_dockManager, 1);
    setCentralWidget(m_center);
    // A collapsed panel slides out over the others, viewports too, which are native windows
    // and draw over any widget that isn't one: so it's one, and on top (eventFilter()).
    connect(m_dockManager, &ads::CDockContainerWidget::autoHideWidgetCreated, this,
            [this](ads::CAutoHideDockContainer* container) {
                container->installEventFilter(this);
                // Qt's offscreen platform (the tests') crashes taking native children down.
                if (QGuiApplication::platformName() == QLatin1String("offscreen")) return;
                container->setAttribute(Qt::WA_DontCreateNativeAncestors);
                container->setAttribute(Qt::WA_NativeWindow);
            });
    m_panelMaximizer = new PanelMaximizer(this, m_dockManager);

    // The application log (docs/LOGGING.md). Its objectName stays "Log", the
    // name the stand-in it replaced was saved under, so existing layouts
    // still place it. The composition root (main.cpp) establishes the center
    // dock area itself once USD is available.
    m_logPanel = new LogPanel(this);
    addSidePanel(m_logPanel, Qt::BottomDockWidgetArea);

    // Toasts, and the log records marked to show as one. Not the records
    // logged before the window existed (no backlog replay): they'd pop up
    // long after the fact.
    m_toasts = new ToastManager(this);
    m_toastSink = std::make_shared<ToastLogSink>(m_toasts, [this]() { m_logPanel->showLatest(); });
    usdcc::core::LogHub::instance().addSink(m_toastSink, false);

    // Deliberately not calling restoreLayout() here — see its declaration in
    // main_window.h for why. The composition root calls it once every panel
    // exists.
}

ads::CDockManager* MainWindow::dockManager() const { return m_dockManager; }

PanelMaximizer* MainWindow::panelMaximizer() const { return m_panelMaximizer; }

MainWindow::~MainWindow() {
    // Before m_toasts goes: the sink posts to it from any thread.
    usdcc::core::LogHub::instance().removeSink(m_toastSink.get());
    // ~QWidget deletes the side panels after m_sidePanels is gone.
    for (SidePanel* panel : std::as_const(m_sidePanels)) panel->m_window = nullptr;
}

LogPanel* MainWindow::logPanel() const { return m_logPanel; }

ToastManager* MainWindow::toasts() const { return m_toasts; }

void MainWindow::addSidePanel(SidePanel* panel, Qt::DockWidgetArea area) {
    if (!panel || m_sidePanels.contains(panel)) return;
    m_panelMaximizer->restore();
    // ADS knows its panels by name, and a second of one would replace the first.
    if (const QString name = panel->objectName(); m_dockManager->findDockWidget(name)) {
        USDCC_LOG_WARNING("usdcc.layout", "Two panels are named \"{}\"; the saved layout can only place one of them",
                          name.toStdString());
        panel->setObjectName(
            uniqueName(name, [this](const QString& taken) { return m_dockManager->findDockWidget(taken) != nullptr; }));
    }
    m_sidePanels.append(panel);
    panel->m_window = this;
    panel->m_lastArea = area;
    const ads::SideBarLocation bar = area == Qt::LeftDockWidgetArea    ? ads::SideBarLeft
                                     : area == Qt::RightDockWidgetArea ? ads::SideBarRight
                                     : area == Qt::TopDockWidgetArea   ? ads::SideBarTop
                                                                       : ads::SideBarBottom;
    m_dockManager->addAutoHideDockWidget(bar, panel)->setSize(
        (bar == ads::SideBarLeft || bar == ads::SideBarRight ? width() : height()) / 4);
    const auto known = m_places.constFind(panel->objectName());
    if (m_layoutRestored && known != m_places.cend()) {
        panel->setPlace(known->toMap());
    } else if (isVisible()) {
        panel->dockAt(area);
    }
    if (m_viewMenu) m_viewMenu->insertAction(m_viewMenuPanelsEnd, panel->toggleViewAction());
}

void MainWindow::removeSidePanel(SidePanel* panel) {
    if (!panel || !m_sidePanels.contains(panel)) return;
    forgetSidePanel(panel);
    panel->deleteLater();
}

void MainWindow::forgetSidePanel(SidePanel* panel) {
    if (!m_sidePanels.removeOne(panel)) return;
    panel->m_window = nullptr;
    m_panelMaximizer->restore();
    m_places.insert(panel->objectName(), panel->place());
    if (m_viewMenu) m_viewMenu->removeAction(panel->toggleViewAction());
    panel->leavingTabBar();
    m_dockManager->removeDockWidget(panel);
    panel->leavingTabBar(true);
}

QVariantMap MainWindow::sidePanelPlaces() const {
    QVariantMap places = m_places;
    for (const SidePanel* panel : m_sidePanels) places.insert(panel->objectName(), panel->place());
    return places;
}

void MainWindow::setBottomBar(QWidget* bar, const QString& name) {
    if (!bar || m_bottomBar) return;
    m_bottomBar = bar;
    bar->setParent(m_center);
    m_center->layout()->addWidget(bar);
    m_bottomBarAction = new QAction(name, this);
    m_bottomBarAction->setObjectName(QStringLiteral("bottomBarAction"));
    m_bottomBarAction->setCheckable(true);
    m_bottomBarAction->setChecked(true);
    connect(m_bottomBarAction, &QAction::toggled, bar, &QWidget::setVisible);
    if (m_viewMenu) m_viewMenu->insertAction(m_viewMenuPanelsEnd, m_bottomBarAction);
}

QMenu* MainWindow::createPopupMenu() {
    if (!m_viewMenu) return QMainWindow::createPopupMenu();
    // The View menu's panel section (side panels and the bottom bar, above
    // m_viewMenuPanelsEnd), then its toolbar entries right below it.
    QList<QAction*> toolBarActions;
    for (QToolBar* toolBar : findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly))
        toolBarActions.append(toolBar->toggleViewAction());
    auto* menu = new QMenu(this);
    bool panels = true;
    for (QAction* action : m_viewMenu->actions()) {
        if (action == m_viewMenuPanelsEnd) {
            panels = false;
            menu->addSeparator();
        } else if (panels || toolBarActions.contains(action)) {
            menu->addAction(action);
        }
    }
    return menu;
}

void MainWindow::setUpWindowAgent() {
    m_windowAgent = new QWK::WidgetWindowAgent(this);
    m_windowAgent->setup(this);
    m_windowAgent->setTitleBar(m_titleBar);  // the bar's empty space drags / double-click-maximizes
    // Interactive widgets inside the bar must opt out of that dragging. The
    // caption buttons are registered as the window's system buttons instead,
    // which also gives the maximize button Windows 11's snap-layout flyout.
    m_windowAgent->setHitTestVisible(m_titleBar->menuBar(), true);
#ifdef Q_OS_MACOS
    // macOS keeps its own close, minimize and zoom buttons, which QWindowKit places in
    // the room the bar leaves for them; the bar's own are hidden (TitleBar).
    m_windowAgent->setSystemButtonArea(m_titleBar->systemButtonArea());
#else
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::WindowIcon, m_titleBar->logoWidget());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Minimize, m_titleBar->minimizeButton());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Maximize, m_titleBar->maximizeButton());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Close, m_titleBar->closeButton());
#endif
}

QMenuBar* MainWindow::mainMenuBar() const { return m_titleBar->menuBar(); }

void MainWindow::addTitleBarWidget(QWidget* widget) {
    if (!widget) return;
    m_titleBar->addTrailingWidget(widget);
    if (m_windowAgent) m_windowAgent->setHitTestVisible(widget, true);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    for (SidePanel* panel : std::as_const(m_sidePanels)) {
        if (!panel->canClose()) {
            event->ignore();
            return;
        }
    }
    for (const std::function<bool()>& check : m_closeChecks) {
        if (!check()) {
            event->ignore();
            return;
        }
    }
    m_panelMaximizer->restore();  // the user's layout is what gets saved
    Q_EMIT closing();
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // The native window's own changes too, which the widget's state can miss
    // (isFullScreenNow()).
    if (QWindow* window = windowHandle(); window && !m_windowStatesWatched) {
        m_windowStatesWatched = true;
        connect(window, &QWindow::windowStateChanged, this, [this]() { syncFullScreenAction(); });
    }
    if (!m_layoutSettled) m_settleTimer.start();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (!m_layoutSettled && isVisible()) m_settleTimer.start();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show) {
        if (auto* container = qobject_cast<ads::CAutoHideDockContainer*>(watched)) {
            container->raise();
            // One at a time: they'd cover each other.
            for (ads::CAutoHideDockContainer* other : m_dockManager->autoHideWidgets()) {
                if (other != container && other->isVisible()) other->collapseView(true);
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::restoreLayout() {
    QSettings settings = layoutSettings();
    if (settings.contains(kGeometryKey)) {
        restoreGeometry(settings.value(kGeometryKey).toByteArray());
    }
    applyLayout(settings.value(kWindowStateKey).toByteArray(), settings.value(kSidePanelPlacesKey).toMap(),
                settings.value(kDockLayoutKey).toByteArray());
    if (m_bottomBarAction) m_bottomBarAction->setChecked(settings.value(kBottomBarKey, true).toBool());
    keepFloatingPanelsOnScreen();
    USDCC_LOG_DEBUG("usdcc.layout", "Restored the window layout from {}", settings.fileName().toStdString());
}

void MainWindow::applyLayout(const QByteArray& windowState, const QVariantMap& places, const QByteArray& dockLayout) {
    // Toolbars, which are all created by now (see restoreLayout()'s declaration), keyed
    // by their objectNames.
    if (!windowState.isEmpty() && !restoreState(windowState, kWindowStateVersion)) {
        USDCC_LOG_WARNING("usdcc.layout", "Ignored the saved toolbar layout (older version)");
    }
    m_places = places;
    QSet<SidePanel*> closed;
    for (SidePanel* panel : std::as_const(m_sidePanels)) {
        if (panel->isClosed()) closed.insert(panel);
    }
    if (!dockLayout.isEmpty()) {
        if (m_dockManager->restoreState(dockLayout)) {
            showPanelsMissingFrom(dockLayout, closed);
        } else {
            USDCC_LOG_WARNING("usdcc.layout", "Could not restore the saved panel layout");
        }
    }
    m_layoutRestored = true;
}

QJsonObject MainWindow::layoutState() const {
    QJsonObject state;
    state.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    state.insert(QStringLiteral("maximized"), isMaximized());
    state.insert(QStringLiteral("windowState"), QString::fromLatin1(saveState(kWindowStateVersion).toBase64()));
    state.insert(QStringLiteral("sidePanelPlaces"), QJsonObject::fromVariantMap(sidePanelPlaces()));
    if (m_bottomBarAction) state.insert(QStringLiteral("bottomBarVisible"), m_bottomBarAction->isChecked());
    // As XML, readable in the session file; ADS compresses it by default.
    const QByteArray dock = m_dockManager->saveState();
    state.insert(QStringLiteral("dockLayout"), QString::fromUtf8(dock.startsWith("<?xml") ? dock : qUncompress(dock)));
    return state;
}

void MainWindow::restoreLayoutState(const QJsonObject& state, bool withGeometry) {
    m_panelMaximizer->restore();
    if (withGeometry) {
        restoreGeometry(QByteArray::fromBase64(state.value(QStringLiteral("geometry")).toString().toLatin1()));
    } else if (state.value(QStringLiteral("maximized")).toBool() && !isMaximized() && isVisible()) {
        showMaximized();
    }
    applyLayout(QByteArray::fromBase64(state.value(QStringLiteral("windowState")).toString().toLatin1()),
                state.value(QStringLiteral("sidePanelPlaces")).toObject().toVariantMap(),
                state.value(QStringLiteral("dockLayout")).toString().toUtf8());
    // A session from before there was one says nothing: the bar stays as it is.
    if (m_bottomBarAction && state.contains(QStringLiteral("bottomBarVisible"))) {
        m_bottomBarAction->setChecked(state.value(QStringLiteral("bottomBarVisible")).toBool());
    }
    keepFloatingPanelsOnScreen();
}

void MainWindow::keepFloatingPanelsOnScreen() {
    QList<QRect> screens;
    for (const QScreen* screen : QGuiApplication::screens()) screens << screen->availableGeometry();
    if (screens.isEmpty()) return;
    QList<QWidget*> floating;
    for (ads::CFloatingDockContainer* container : m_dockManager->floatingWidgets()) floating << container;
    for (QWidget* widget : floating) {
        const QRect frame = widget->frameGeometry();
        const QRect fitted = fitOnScreens(frame, screens);
        if (fitted == frame) continue;
        // The frame is what has to fit; geometry() is the inside of it.
        const QSize border = frame.size() - widget->geometry().size();
        widget->resize(fitted.size() - border);
        widget->move(fitted.topLeft());
        USDCC_LOG_DEBUG("usdcc.layout", "Moved the floating {} onto a screen", widget->objectName().toStdString());
    }
}

QRect fitOnScreens(const QRect& rect, const QList<QRect>& screens) {
    if (screens.isEmpty()) return rect;
    const QRect* best = nullptr;
    qint64 bestArea = 0;
    for (const QRect& screen : screens) {
        if (screen.contains(rect)) return rect;
        const QRect overlap = screen.intersected(rect);
        const qint64 area = qint64(overlap.width()) * overlap.height();
        if (area > bestArea) {
            bestArea = area;
            best = &screen;
        }
    }
    if (!best) {
        // Off every screen: the nearest, by the distance between centres.
        qint64 nearest = -1;
        for (const QRect& screen : screens) {
            const QPoint d = screen.center() - rect.center();
            const qint64 distance = qint64(d.x()) * d.x() + qint64(d.y()) * d.y();
            if (nearest < 0 || distance < nearest) {
                nearest = distance;
                best = &screen;
            }
        }
    }
    QRect fitted(rect.topLeft(), rect.size().boundedTo(best->size()));
    fitted.moveLeft(std::clamp(fitted.left(), best->left(), best->right() - fitted.width() + 1));
    fitted.moveTop(std::clamp(fitted.top(), best->top(), best->bottom() - fitted.height() + 1));
    return fitted;
}

void MainWindow::showPanelsMissingFrom(const QByteArray& dockLayout, const QSet<SidePanel*>& closed) {
    // ADS saves its layout as XML, compressed by default: one <Widget Name=..>
    // per panel, open or closed.
    const QByteArray xml = dockLayout.startsWith("<?xml") ? dockLayout : qUncompress(dockLayout);
    QSet<QString> saved;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement && reader.name() == QLatin1String("Widget")) {
            saved.insert(reader.attributes().value(QLatin1String("Name")).toString());
        }
    }
    if (reader.hasError() || saved.isEmpty()) return;  // can't tell: leave the layout as restored

    // A side panel goes to its edge; collapsed, it takes no room. Closed before, it stays so.
    for (SidePanel* panel : std::as_const(m_sidePanels)) {
        if (saved.contains(panel->objectName())) continue;
        panel->setCollapsed(true);
        if (closed.contains(panel)) panel->hide();
    }

    // ADS hides the panels its saved layout doesn't name. One the user closed
    // is named (as closed); one that isn't was gone when the layout was saved
    // (deleted by Close Stage, or new), so it opens beside the others.
    ads::CDockAreaWidget* nextTo = nullptr;
    for (ads::CDockWidget* dockWidget : m_dockManager->dockWidgetsMap()) {
        if (saved.contains(dockWidget->objectName()) && !dockWidget->isClosed() && dockWidget->dockAreaWidget() &&
            !dockWidget->isAutoHide()) {
            nextTo = dockWidget->dockAreaWidget();
            break;
        }
    }
    for (ads::CDockWidget* dockWidget : m_dockManager->dockWidgetsMap()) {
        if (saved.contains(dockWidget->objectName()) || !dockWidget->isClosed()) continue;
        if (qobject_cast<SidePanel*>(dockWidget)) continue;
        if (nextTo) {
            m_dockManager->addDockWidgetTabToArea(dockWidget, nextTo);
        } else {
            nextTo = m_dockManager->addDockWidget(ads::CenterDockWidgetArea, dockWidget);
        }
        dockWidget->toggleView(true);
        USDCC_LOG_DEBUG("usdcc.layout", "Showing {}, which the saved layout doesn't have",
                        dockWidget->objectName().toStdString());
    }
}

void MainWindow::saveLayout() const {
    QSettings settings = layoutSettings();
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kWindowStateKey, saveState(kWindowStateVersion));
    settings.setValue(kDockLayoutKey, m_dockManager->saveState());
    QStringList panels;
    for (const SidePanel* panel : m_sidePanels) panels << panel->objectName();
    settings.setValue(kSidePanelPlacesKey, sidePanelPlaces());
    if (m_bottomBarAction) settings.setValue(kBottomBarKey, m_bottomBarAction->isChecked());
    USDCC_LOG_DEBUG("usdcc.layout", "Saved the window layout to {} (side panels: {})", settings.fileName().toStdString(),
                    panels.join(QStringLiteral(", ")).toStdString());
}

void MainWindow::buildViewMenu() {
    QMenu* viewMenu = mainMenuBar()->addMenu(tr("&View"));
    m_viewMenu = viewMenu;
    // ADS already provides a checkable show/hide QAction, kept in
    // sync with the dock widget's actual visibility — no custom toggle slot
    // needed.
    for (SidePanel* panel : m_sidePanels) {
        viewMenu->addAction(panel->toggleViewAction());
    }
    if (m_bottomBarAction) viewMenu->addAction(m_bottomBarAction);
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

    // Under UI Scale. On the window too, so its shortcut works with the menu bar
    // out of reach.
    m_fullScreenAction = viewMenu->addAction(tr("&Full Screen"));
    m_fullScreenAction->setObjectName(QStringLiteral("fullScreen"));
    m_fullScreenAction->setCheckable(true);
    m_fullScreenAction->setChecked(isFullScreenNow());
    m_fullScreenAction->setShortcuts(QKeySequence::FullScreen);
    m_fullScreenAction->setStatusTip(tr("Fill the screen with the window"));
    addAction(m_fullScreenAction);
    // From what the window is, not the check mark, which may be behind it.
    connect(m_fullScreenAction, &QAction::triggered, this, [this]() {
        setFullScreen(!isFullScreenNow());
        syncFullScreenAction();
    });
}

bool MainWindow::isFullScreenNow() const {
    // The native window's state. From maximized, Windows reports the window
    // restored on the way to full screen, and the widget's state can stay
    // there (isFullScreen() false) while the window is full screen.
    if (const QWindow* window = windowHandle()) return window->windowStates().testFlag(Qt::WindowFullScreen);
    return isFullScreen();
}

void MainWindow::syncFullScreenAction() {
#ifdef Q_OS_WIN
    // Out of full screen, however: the normal place setFullScreen() stood in for
    // goes back. Queued, as this can run halfway through Qt's way out.
    if (m_normalBeforeFullScreen.isValid() && !isFullScreenNow()) {
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_normalBeforeFullScreen.isValid() || isFullScreenNow()) return;
            const HWND hwnd = reinterpret_cast<HWND>(winId());
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (GetWindowPlacement(hwnd, &placement)) {
                int left, top, right, bottom;
                m_normalBeforeFullScreen.getCoords(&left, &top, &right, &bottom);
                placement.rcNormalPosition = {left, top, right, bottom};
                SetWindowPlacement(hwnd, &placement);
            }
            m_normalBeforeFullScreen = QRect();
        }, Qt::QueuedConnection);
    }
#endif
    if (!m_fullScreenAction) return;
    const QSignalBlocker blocker(m_fullScreenAction);
    m_fullScreenAction->setChecked(isFullScreenNow());
}

void MainWindow::setFullScreen(bool fullScreen) {
    if (fullScreen == isFullScreenNow()) return;
    if (fullScreen) {
        m_maximizedBeforeFullScreen = isMaximized();
#ifdef Q_OS_WIN
        // Qt leaves full screen by putting the window back at its normal place, then
        // maximizing it, which shows the window small and Windows growing it. That
        // normal place is read here, from the window's placement: make it the
        // maximized window's own rect for the time being, so the way back is
        // maximized throughout. The real one is put back once out of full screen.
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        RECT maximized{};
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (m_maximizedBeforeFullScreen && GetWindowPlacement(hwnd, &placement) && GetWindowRect(hwnd, &maximized) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const RECT normal = placement.rcNormalPosition;
            m_normalBeforeFullScreen.setCoords(normal.left, normal.top, normal.right, normal.bottom);
            // The placement is in workspace coordinates: offset by a taskbar at the top or left.
            const LONG dx = monitor.rcWork.left - monitor.rcMonitor.left;
            const LONG dy = monitor.rcWork.top - monitor.rcMonitor.top;
            placement.rcNormalPosition = {maximized.left - dx, maximized.top - dy, maximized.right - dx,
                                          maximized.bottom - dy};
            SetWindowPlacement(hwnd, &placement);
        }
#endif
        showFullScreen();
    } else if (m_maximizedBeforeFullScreen) {
        showMaximized();
    } else {
        showNormal();
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    // However the window got there (the action, a script, the system).
    if (event->type() == QEvent::WindowStateChange) syncFullScreenAction();
}

}  // namespace usdcc::ui
