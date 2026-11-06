// Maximizing a ViewPanel's dock area and getting the layout back. Runs in
// usdcc_ui_tests (Qt's offscreen platform); the layout is saved in Qt's
// test-mode config folder, never in the user's usdcc.ini.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/log_side_panel.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/panel_maximizer.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QSplitter>
#include <QTest>

#include <memory>

using usdcc::ui::MainWindow;
using usdcc::ui::PanelMaximizer;
using usdcc::usd::OutlinerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::UndoManager;

namespace {
class TestPanel : public OutlinerViewPanel {
public:
    using OutlinerViewPanel::OutlinerViewPanel;
    using OutlinerViewPanel::populateContextMenu;
};

// Left over right, beside a third:  | left  | side |
//                                   | right |      |
// with the Log side panel below.
struct Fixture {
    TestSettings settings;
    StageManager stages;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    MainWindow window;
    ads::CDockManager* manager = window.dockManager();
    PanelMaximizer* maximizer = window.panelMaximizer();
    TestPanel* left = make("Left");
    TestPanel* right = make("Right");
    TestPanel* side = make("Side");

    Fixture() {
        ads::CDockAreaWidget* leftArea = manager->addDockWidget(ads::CenterDockWidgetArea, left);
        manager->addDockWidget(ads::RightDockWidgetArea, side, leftArea);
        manager->addDockWidget(ads::BottomDockWidgetArea, right, leftArea);
        window.resize(1000, 700);
        window.show();
        QElapsedTimer timer;
        timer.start();
        while (!window.isLayoutSettled() && timer.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 50);
        settle();
    }

    TestPanel* make(const QString& name) { return new TestPanel(&stages, nullptr, undo, name, &window); }

    static void settle() {
        for (int i = 0; i < 3; ++i) QApplication::processEvents();
    }

    // Every splitter's sizes, the side panel's height: what must come back.
    QList<QList<int>> layoutSizes() const {
        QList<QList<int>> sizes;
        for (const auto* splitter : manager->findChildren<QSplitter*>()) sizes.append(splitter->sizes());
        sizes.append({window.logPanel()->height(), manager->height()});
        return sizes;
    }

    // Give or take the frame ADS keeps around its root splitter.
    bool fillsDockManager(const ads::CDockWidget* panel) const {
        const QSize area = panel->dockAreaWidget()->size();
        return manager->width() - area.width() <= 4 && manager->height() - area.height() <= 4;
    }
};

QString maximizeEntry(TestPanel* panel, bool trigger = false) {
    QMenu menu;
    panel->populateContextMenu(&menu);
    const QList<QAction*> actions = menu.actions();
    for (QAction* action : actions) {
        if (action->text() == "Maximize" || action->text() == "Restore Layout") {
            const QString text = action->text();
            if (trigger) action->trigger();
            return text;
        }
    }
    return {};
}
}  // namespace

USDCC_TEST(panel_maximize_gives_the_area_all_the_room_and_restore_puts_it_back) {
    Fixture f;
    const QList<QList<int>> before = f.layoutSizes();
    CHECK(!f.fillsDockManager(f.left), "the panels share the room to begin with");
    CHECK(f.window.logPanel()->isVisible(), "and the side panel is showing");

    f.maximizer->maximize(f.left);
    Fixture::settle();
    CHECK(f.fillsDockManager(f.left), "the area fills the dock manager");
    CHECK(!f.right->isVisible() && !f.side->isVisible(), "the other panels are out of sight");
    CHECK(!f.right->isClosed() && !f.side->isClosed(), "but not closed");
    CHECK(!f.window.logPanel()->isVisible(), "the docked side panel is hidden too");
    CHECK(f.manager->height() > before.last().last(), "which gives the dock manager its room");

    f.maximizer->restore();
    Fixture::settle();
    CHECK(f.right->isVisible() && f.side->isVisible() && f.window.logPanel()->isVisible(), "everything is back");
    CHECK(f.layoutSizes() == before, "at the sizes it had");
}

USDCC_TEST(panel_maximize_nested_and_outer_areas) {
    Fixture f;
    const QList<QList<int>> before = f.layoutSizes();
    f.maximizer->maximize(f.side);  // hides the whole left splitter
    Fixture::settle();
    CHECK(f.fillsDockManager(f.side), "an outer area fills it");
    f.maximizer->maximize(f.right);  // restores first, then maximizes this one
    Fixture::settle();
    CHECK(f.fillsDockManager(f.right), "going straight to a nested one fills it too");
    CHECK(f.maximizer->isMaximized(f.right) && !f.maximizer->isMaximized(f.side), "only that one counts as maximized");
    f.maximizer->restore();
    Fixture::settle();
    CHECK(f.layoutSizes() == before, "the sizes are back");
}

USDCC_TEST(panel_maximize_from_tab_double_click_and_context_menu) {
    Fixture f;
    const QList<QList<int>> before = f.layoutSizes();
    QTest::mouseDClick(f.left->tabWidget(), Qt::LeftButton);
    Fixture::settle();
    CHECK(f.maximizer->isMaximized(f.left), "a double-click maximizes");
    CHECK(!f.left->isFloating(), "instead of undocking");
    QTest::mouseDClick(f.left->tabWidget(), Qt::LeftButton);
    Fixture::settle();
    CHECK(!f.maximizer->isActive() && f.layoutSizes() == before, "and another restores");

    CHECK(maximizeEntry(f.left) == "Maximize", "the menu offers to maximize");
    maximizeEntry(f.left, true);
    Fixture::settle();
    CHECK(f.maximizer->isMaximized(f.left), "and does");
    CHECK(maximizeEntry(f.left) == "Restore Layout", "then offers to restore");
    maximizeEntry(f.left, true);
    Fixture::settle();
    CHECK(!f.maximizer->isActive() && f.layoutSizes() == before, "and does");

    f.left->setFloating();
    Fixture::settle();
    CHECK(maximizeEntry(f.left).isEmpty(), "a floating panel has no such entry");
    CHECK(!f.maximizer->canMaximize(f.left), "and can't be maximized");
}

USDCC_TEST(panel_maximize_keeps_tabs_and_shows_the_chosen_one) {
    Fixture f;
    TestPanel* tabbed = f.make("Tabbed");
    f.manager->addDockWidgetTabToArea(tabbed, f.left->dockAreaWidget());
    Fixture::settle();
    CHECK(tabbed->isVisible() && !f.left->isVisible(), "the newly added tab is showing");

    f.maximizer->maximize(f.left);
    Fixture::settle();
    CHECK(f.left->isVisible(), "the panel asked for is shown");
    CHECK(f.maximizer->isMaximized(tabbed), "its tab neighbour is in the maximized area too");
}

USDCC_TEST(panel_maximize_restores_when_the_layout_changes) {
    {
        Fixture f;
        f.maximizer->maximize(f.left);
        f.left->toggleView(false);  // the maximized area's only panel
        Fixture::settle();
        CHECK(!f.maximizer->isActive(), "closing the maximized panel restores");
        CHECK(f.right->isVisible() && f.side->isVisible(), "so the others aren't left hidden");
    }
    {
        Fixture f;
        f.maximizer->maximize(f.left);
        f.side->toggleView(false);
        f.side->toggleView(true);  // opening a hidden panel
        Fixture::settle();
        CHECK(!f.maximizer->isActive(), "a hidden panel opened restores");
        CHECK(f.side->isVisible() && f.left->isVisible(), "and it can be seen");
    }
    {
        Fixture f;
        f.maximizer->maximize(f.left);
        f.manager->addDockWidget(ads::RightDockWidgetArea, f.make("Added"));
        Fixture::settle();
        CHECK(!f.maximizer->isActive(), "a panel added restores");
        CHECK(f.manager->findDockWidget("Added")->isVisible() && f.right->isVisible(), "all of it showing");
    }
    {
        Fixture f;
        f.maximizer->maximize(f.left);
        f.side->toggleView(false);  // closed out of sight stays closed
        Fixture::settle();
        f.maximizer->restore();
        Fixture::settle();
        CHECK(f.side->isClosed() && !f.side->dockAreaWidget()->isVisible(), "a panel closed meanwhile stays closed");
        CHECK(f.fillsDockManager(f.left) == false && f.right->isVisible(), "the rest is back");
        CHECK(f.left->dockAreaWidget()->width() == f.manager->width(), "and takes the room it left");
    }
}

USDCC_TEST(panel_maximize_is_not_saved_as_the_layout) {
    Fixture f;
    const QList<QList<int>> before = f.layoutSizes();
    f.maximizer->maximize(f.left);
    Fixture::settle();
    f.window.close();
    CHECK(!f.maximizer->isActive(), "closing the window restores first");
    CHECK(f.right->dockAreaWidget() && !f.right->dockAreaWidget()->isHidden(), "so the saved layout has the others");
    CHECK(f.layoutSizes().first() == before.first(), "at their sizes");
}
