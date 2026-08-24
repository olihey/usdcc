#include "usdcc/ui/main_window.h"

#include <DockManager.h>
#include <QCloseEvent>
#include <QSettings>
#include <QTextEdit>

#include "usdcc/ui/side_panel.h"
#include "usdcc/ui/view_panel.h"

namespace usdcc::ui {

namespace {
constexpr auto kOrganization = "usdcc";
constexpr auto kApplication = "usdcc";
constexpr auto kGeometryKey = "MainWindow/geometry";
constexpr auto kDockLayoutKey = "MainWindow/dockLayout";
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("usdcc");
    resize(1280, 800);

    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    m_dockManager = new ads::CDockManager(this);
    setCentralWidget(m_dockManager);

    // Stand-ins proving SidePanel/ViewPanel + layout persistence work end to
    // end; replaced by real subclasses (OutlinerViewPanel, LogSidePanel,
    // ...) in later milestones.
    auto* viewPanelStandIn = new ViewPanel("Outliner", this);
    viewPanelStandIn->setContentWidget(new QTextEdit(viewPanelStandIn));
    m_dockManager->addDockWidget(ads::CenterDockWidgetArea, viewPanelStandIn);

    auto* sidePanelStandIn = new SidePanel("Log", this);
    sidePanelStandIn->setWidget(new QTextEdit(sidePanelStandIn));
    addDockWidget(Qt::BottomDockWidgetArea, sidePanelStandIn);

    restoreLayout();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::restoreLayout() {
    QSettings settings(kOrganization, kApplication);
    if (settings.contains(kGeometryKey)) {
        restoreGeometry(settings.value(kGeometryKey).toByteArray());
    }
    if (settings.contains(kDockLayoutKey)) {
        m_dockManager->restoreState(settings.value(kDockLayoutKey).toByteArray());
    }
}

void MainWindow::saveLayout() const {
    QSettings settings(kOrganization, kApplication);
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kDockLayoutKey, m_dockManager->saveState());
}

}  // namespace usdcc::ui
