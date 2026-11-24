// Side panels (SidePanel): collapsing to a tab at an edge and docking again, several at one
// edge, the edges they're found at however they nest, closing, the places kept for panels
// added later, and deleting one. Runs in usdcc_ui_tests (Qt's offscreen platform); the layout
// is saved in Qt's test-mode config folder, never in the user's usdcc.ini.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/log_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/side_panel.h"

#include <AutoHideTab.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QTextEdit>

#include <cstdlib>

using usdcc::ui::MainWindow;
using usdcc::ui::SidePanel;

namespace {

void settle(MainWindow& window) {
    QElapsedTimer timer;
    timer.start();
    while (!window.isLayoutSettled() && timer.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    for (int i = 0; i < 3; ++i) QApplication::processEvents();
}

bool inViewMenu(MainWindow& window, const SidePanel* panel) {
    return window.viewMenu()->actions().contains(panel->toggleViewAction());
}

// Two view panels' worth of layout, side by side, and panels added like the app's.
struct Panels {
    ads::CDockWidget* left = new ads::CDockWidget(QStringLiteral("Left"));
    ads::CDockWidget* right = new ads::CDockWidget(QStringLiteral("Right"));
    SidePanel* notes = new SidePanel(QStringLiteral("Notes"));
    SidePanel* scripts = new SidePanel(QStringLiteral("Scripts"));
    explicit Panels(MainWindow& window) {
        left->setWidget(new QTextEdit);
        right->setWidget(new QTextEdit);
        auto* area = window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, left);
        window.dockManager()->addDockWidget(ads::RightDockWidgetArea, right, area);
        notes->setWidget(new QLabel(QStringLiteral("notes")));
        scripts->setWidget(new QTextEdit);
        window.addSidePanel(notes, Qt::BottomDockWidgetArea);
        window.addSidePanel(scripts, Qt::RightDockWidgetArea);
        window.buildViewMenu();
        window.resize(1200, 800);
    }
};

}  // namespace

USDCC_TEST(side_panel_collapse_and_dock) {
    TestSettings settings;
    {
        MainWindow window;
        Panels panels(window);
        window.restoreLayout();
        window.show();
        settle(window);
        SidePanel* log = window.logPanel();
        CHECK(panels.notes->isCollapsed() && !panels.notes->isVisible() && panels.notes->area() == Qt::BottomDockWidgetArea,
              "added before the window is up, it starts as a tab at its edge");
        CHECK(panels.notes->sideTabWidget()->isVisible(), "which shows");
        CHECK(inViewMenu(window, panels.notes) && inViewMenu(window, log), "listed in the View menu");

        panels.scripts->dockAt(Qt::RightDockWidgetArea);
        log->dockAt(Qt::BottomDockWidgetArea);
        settle(window);
        CHECK(panels.scripts->isVisible() && panels.scripts->area() == Qt::RightDockWidgetArea, "docked on the right");
        CHECK(log->isVisible() && log->area() == Qt::BottomDockWidgetArea, "and along the bottom, under it");
        CHECK(panels.scripts->area() == Qt::RightDockWidgetArea, "the right one is at the right still");
        panels.scripts->resizeToShare(0.3);
        settle(window);
        CHECK(std::abs(panels.scripts->place().value("size").toDouble() - 0.3) < 0.02, "with the share asked for: %f",
              panels.scripts->place().value("size").toDouble());

        panels.notes->dockAt(Qt::BottomDockWidgetArea);
        settle(window);
        CHECK(panels.notes->dockAreaWidget() == log->dockAreaWidget(), "a second one at an edge is a tab of the first");
        CHECK(panels.notes->isVisible() && !log->isVisible(), "in front");
        log->raise();
        settle(window);
        CHECK(log->isVisible(), "raise() brings the other's tab in front");

        log->setCollapsed(true);
        settle(window);
        CHECK(log->isCollapsed() && !log->isVisible() && panels.notes->isVisible(), "collapsing one leaves the other");
        log->reveal();
        settle(window);
        CHECK(log->isVisible() && log->isCollapsed(), "revealed, it slides out");
        panels.scripts->setCollapsed(true);
        panels.scripts->reveal();
        settle(window);
        CHECK(panels.scripts->isVisible() && !log->isVisible(), "one at a time");
        panels.scripts->dockAt(Qt::RightDockWidgetArea);
        panels.scripts->resizeToShare(0.3);
        log->setCollapsed(false);
        settle(window);
        CHECK(!log->isCollapsed() && log->area() == Qt::BottomDockWidgetArea, "and docks where it was");

        panels.notes->hide();
        settle(window);
        CHECK(panels.notes->isHidden() && panels.notes->area() == Qt::NoDockWidgetArea, "hidden");
        panels.notes->show();
        settle(window);
        CHECK(!panels.notes->isHidden() && panels.notes->isVisible() && !panels.notes->isCollapsed(), "and back where it was");

        window.dockManager()->addDockWidget(ads::LeftDockWidgetArea, panels.notes, panels.right->dockAreaWidget());
        settle(window);
        CHECK(panels.notes->area() == Qt::NoDockWidgetArea, "between the view panels it's at no edge");
        panels.notes->setCollapsed(true);
        CHECK(panels.notes->area() == Qt::BottomDockWidgetArea, "and collapses to the last one it was at");
        window.close();  // saves the layout
    }
    {
        MainWindow window;
        Panels panels(window);
        window.restoreLayout();
        window.show();
        settle(window);
        CHECK(panels.scripts->isVisible() && panels.scripts->area() == Qt::RightDockWidgetArea, "docked in the next session too");
        CHECK(window.logPanel()->area() == Qt::BottomDockWidgetArea && !window.logPanel()->isCollapsed(), "the Log as well");
        CHECK(panels.notes->isCollapsed() && panels.notes->area() == Qt::BottomDockWidgetArea, "and the collapsed one collapsed");
    }
}

USDCC_TEST(side_panel_added_later_and_removed) {
    TestSettings settings;
    {
        MainWindow window;
        Panels panels(window);
        window.restoreLayout();
        window.show();
        settle(window);
        auto* late = new SidePanel(QStringLiteral("Late"));
        late->setWidget(new QLabel(QStringLiteral("late")));
        window.addSidePanel(late, Qt::LeftDockWidgetArea);
        settle(window);
        CHECK(late->isVisible() && late->area() == Qt::LeftDockWidgetArea, "added to a window that's up, it's docked");

        late->dockAt(Qt::TopDockWidgetArea);
        QPointer<SidePanel> gone(late);
        QPointer<QAction> action(late->toggleViewAction());
        window.removeSidePanel(late);
        CHECK(!window.sidePanels().contains(late) && !window.viewMenu()->actions().contains(action.data()),
              "removed, it's off the View menu");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        settle(window);
        CHECK(!gone, "and deleted");
        CHECK(!window.dockManager()->findDockWidget(QStringLiteral("Late")), "the dock manager doesn't know it");

        auto* again = new SidePanel(QStringLiteral("Late"));
        window.addSidePanel(again, Qt::LeftDockWidgetArea);
        settle(window);
        CHECK(again->area() == Qt::TopDockWidgetArea, "another of its name goes where it was");
        delete again;
        settle(window);
        CHECK(window.sidePanels().size() == 3, "one deleted outright is gone too");
        window.close();
    }
    {
        MainWindow window;
        Panels panels(window);
        window.restoreLayout();
        window.show();
        settle(window);
        auto* late = new SidePanel(QStringLiteral("Late"));
        window.addSidePanel(late, Qt::LeftDockWidgetArea);
        settle(window);
        CHECK(late->isVisible() && late->area() == Qt::TopDockWidgetArea, "and in the next session");
        auto* twin = new SidePanel(QStringLiteral("Late"));
        window.addSidePanel(twin, Qt::LeftDockWidgetArea);
        settle(window);
        CHECK(twin->objectName() != late->objectName() &&
                  window.dockManager()->findDockWidget(QStringLiteral("Late")) == late,
              "a second of the same name gets another and replaces nothing");
    }
}

USDCC_TEST(side_panel_can_close) {
    TestSettings settings;
    struct Busy : SidePanel {
        using SidePanel::SidePanel;
        bool allow = false;
        bool canClose() override { return allow; }
    };
    MainWindow window;
    auto* busy = new Busy(QStringLiteral("Busy"));
    window.addSidePanel(busy, Qt::RightDockWidgetArea);
    window.show();
    settle(window);
    CHECK(!window.close() && window.isVisible(), "a panel can keep the window open");
    busy->allow = true;
    CHECK(window.close(), "and let it close");
}
