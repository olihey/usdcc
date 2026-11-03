// Collapsing side panels (SidePanel::setCollapsed()): double-clicking a tab
// collapses every panel sharing that spot to its title bar, double-clicking
// a title bar expands them again to the height they had, a panel alone at
// the right edge collapses to a vertical title bar's width instead, the
// collapsed state is saved with the layout, and floating a panel expands it.
// Runs in usdcc_ui_tests (Qt's offscreen platform); the layout is saved in
// Qt's test-mode config folder, never in the user's usdcc.ini.
#include "test_harness.h"

#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/side_panel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTabBar>
#include <QTextEdit>

#include <cstdlib>

using usdcc::ui::MainWindow;
using usdcc::ui::SidePanel;

namespace {

struct TestSettings {
    TestSettings() {
        QStandardPaths::setTestModeEnabled(true);
        remove();
    }
    ~TestSettings() {
        remove();
        QStandardPaths::setTestModeEnabled(false);
    }
    static void remove() {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/usdcc.ini");
    }
};

void settle(MainWindow& window) {
    QElapsedTimer timer;
    timer.start();
    while (!window.isLayoutSettled() && timer.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::processEvents();
}

void send(QWidget* widget, QEvent::Type type, QPoint globalPos, Qt::MouseButtons held) {
    const QPoint pos = widget->mapFromGlobal(globalPos);
    QMouseEvent event(type, pos, globalPos, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

// As a real double-click: press, release, press, double-click, release, with
// the mouse move Windows sends once the panel has moved under the cursor.
void doubleClick(QWidget* widget, QPoint pos) {
    const QPoint at = widget->mapToGlobal(pos);
    send(widget, QEvent::MouseButtonPress, at, Qt::LeftButton);
    send(widget, QEvent::MouseButtonRelease, at, Qt::NoButton);
    send(widget, QEvent::MouseButtonPress, at, Qt::LeftButton);
    send(widget, QEvent::MouseButtonDblClick, at, Qt::LeftButton);
    QApplication::processEvents();  // expanding resizes the dock area a moment later
    QApplication::processEvents();
    send(widget, QEvent::MouseMove, at, Qt::LeftButton);
    send(widget, QEvent::MouseButtonRelease, at, Qt::NoButton);
    QApplication::processEvents();
}

QTabBar* shownTabBar(MainWindow& window) {
    for (QTabBar* tabBar : window.findChildren<QTabBar*>(Qt::FindDirectChildrenOnly)) {
        if (tabBar->isVisible()) return tabBar;
    }
    return nullptr;
}

// The Log panel is the window's own; these are added like the app's.
struct Panels {
    SidePanel* notes = new SidePanel(QStringLiteral("Notes"));
    SidePanel* scripts = new SidePanel(QStringLiteral("Scripts"));
    explicit Panels(MainWindow& window) {
        notes->setWidget(new QLabel(QStringLiteral("notes")));
        scripts->setWidget(new QTextEdit);
        window.addSidePanel(notes, Qt::BottomDockWidgetArea);
        window.addSidePanel(scripts, Qt::RightDockWidgetArea);
    }
};

}  // namespace

USDCC_TEST(side_panel_collapse) {
    TestSettings settings;
    int scriptsWidth = 0;
    {
        MainWindow window;
        Panels panels(window);
        SidePanel* log = window.logPanel();
        window.tabifyDockWidget(log, panels.notes);
        window.buildViewMenu();
        window.restoreLayout();
        window.resize(1200, 800);
        window.show();
        settle(window);
        window.resizeDocks({log}, {300}, Qt::Vertical);
        log->raise();
        QApplication::processEvents();
        const int expandedHeight = log->height();
        CHECK(expandedHeight > 200, "the bottom panels start out tall: %d", expandedHeight);

        QTabBar* tabs = shownTabBar(window);
        CHECK(tabs && tabs->count() == 2, "Log and Notes share a spot, with tabs");
        if (!tabs) return;
        doubleClick(tabs, tabs->tabRect(1).center());
        CHECK(log->isCollapsed() && panels.notes->isCollapsed(), "a tab double-click collapses both panels");
        CHECK(log->height() < 50, "to the title bar: %d", log->height());
        panels.notes->raise();
        QApplication::processEvents();
        CHECK(panels.notes->height() < 50, "the other tab is collapsed too: %d", panels.notes->height());
        CHECK(!panels.scripts->isCollapsed(), "panels elsewhere aren't");

        doubleClick(panels.notes, QPoint(20, 5));
        CHECK(!log->isCollapsed() && !panels.notes->isCollapsed(), "a title bar double-click expands both");
        CHECK(std::abs(panels.notes->height() - expandedHeight) <= 2, "to the height they had: %d, was %d",
              panels.notes->height(), expandedHeight);
        CHECK(window.dockWidgetArea(panels.notes) == Qt::BottomDockWidgetArea && !panels.notes->isFloating() &&
                  window.tabifiedDockWidgets(log).contains(panels.notes),
              "and stay where they were: the double-clicks' last mouse move isn't a drag of the title bar");

        scriptsWidth = panels.scripts->width();
        doubleClick(panels.scripts, QPoint(20, 5));
        CHECK(panels.scripts->isCollapsed() && panels.scripts->width() < 50,
              "alone at the right edge, a panel collapses to a narrow strip: %d", panels.scripts->width());
        CHECK(panels.scripts->features().testFlag(QDockWidget::DockWidgetVerticalTitleBar),
              "with its title bar turned on its side");
        CHECK(panels.scripts->height() > 400, "and keeps its height: %d", panels.scripts->height());
        window.close();  // saves the layout
    }
    {
        // The next session restores the collapsed panel, which expands to the
        // width it had before collapsing.
        MainWindow window;
        Panels panels(window);
        window.buildViewMenu();
        window.restoreLayout();
        window.show();
        settle(window);
        CHECK(panels.scripts->isCollapsed() && panels.scripts->width() < 50,
              "a collapsed panel is collapsed in the next session: %d", panels.scripts->width());
        CHECK(!window.logPanel()->isCollapsed(), "an expanded one isn't");
        doubleClick(panels.scripts, QPoint(5, 20));  // the vertical title bar
        CHECK(!panels.scripts->isCollapsed() && std::abs(panels.scripts->width() - scriptsWidth) <= 2,
              "and expands to its old width: %d, was %d", panels.scripts->width(), scriptsWidth);
        CHECK(!panels.scripts->features().testFlag(QDockWidget::DockWidgetVerticalTitleBar),
              "with its title bar upright again");
        CHECK(window.dockWidgetArea(panels.scripts) == Qt::RightDockWidgetArea && !panels.scripts->isFloating(),
              "still docked at the right edge");

        SidePanel* log = window.logPanel();
        log->setCollapsed(true);
        CHECK(log->isCollapsed(), "collapsed in code");
        log->setFloating(true);
        CHECK(!log->isCollapsed(), "floating a collapsed panel expands it");
        log->setCollapsed(true);
        CHECK(!log->isCollapsed(), "a floating panel doesn't collapse");
    }
}
