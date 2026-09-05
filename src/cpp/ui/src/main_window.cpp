#include "usdcc/ui/main_window.h"

#include <DockManager.h>
#include <QCloseEvent>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTextEdit>

#include "usdcc/ui/side_panel.h"

namespace usdcc::ui {

namespace {
constexpr auto kGeometryKey = "MainWindow/geometry";
constexpr auto kDockLayoutKey = "MainWindow/dockLayout";

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

    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    m_dockManager = new ads::CDockManager(this);
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

    // Deliberately not calling restoreLayout() here — see its declaration in
    // main_window.h for why. The composition root calls it once every panel
    // exists.
}

ads::CDockManager* MainWindow::dockManager() const { return m_dockManager; }

void MainWindow::closeEvent(QCloseEvent* event) {
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::restoreLayout() {
    QSettings settings = layoutSettings();
    if (settings.contains(kGeometryKey)) {
        restoreGeometry(settings.value(kGeometryKey).toByteArray());
    }
    if (settings.contains(kDockLayoutKey)) {
        m_dockManager->restoreState(settings.value(kDockLayoutKey).toByteArray());
    }
}

void MainWindow::saveLayout() const {
    QSettings settings = layoutSettings();
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kDockLayoutKey, m_dockManager->saveState());
}

}  // namespace usdcc::ui
