// The current stage (docs/CURRENT_STAGE.md): it follows the focused view
// panel (CurrentStageTracker), Edit > Undo acts on it and names the step
// (CurrentStageUndoGroup), and the window title says which it is. Runs in
// usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/current_stage_tracker.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/current_stage_ui.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/save_stage_as.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"
#include "usdcc/usd/stages_menu.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>
#include <DockWidgetTab.h>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QStandardPaths>
#include <QContextMenuEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <memory>

using usdcc::ui::CurrentStageTracker;
using usdcc::ui::MainWindow;
using usdcc::usd::OutlinerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// Settings in Qt's test-mode folder, never the user's.
struct TestMode {
    TestMode() { QStandardPaths::setTestModeEnabled(true); }
    ~TestMode() { QStandardPaths::setTestModeEnabled(false); }
};
}  // namespace

USDCC_TEST(current_stage_follows_focus) {
    TestMode testMode;
    // Its own UI scale, gone with the test (MainWindow would otherwise make
    // an app-wide one that outlives it).
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr one = stages.createStage();
    const StageRefPtr two = stages.createStage();
    const StageRefPtr three = stages.createStage();

    MainWindow window;
    ads::CDockManager* docks = window.dockManager();
    auto* a = new OutlinerViewPanel(&stages, one, undo, QStringLiteral("A"), &window);
    auto* b = new OutlinerViewPanel(&stages, three, undo, QStringLiteral("B"), &window);
    auto* empty = new OutlinerViewPanel(&stages, nullptr, undo, QStringLiteral("Empty"), &window);
    // Side by side: ADS reports focus only on a panel that is visible.
    docks->addDockWidget(ads::LeftDockWidgetArea, a);
    docks->addDockWidget(ads::CenterDockWidgetArea, b);
    docks->addDockWidget(ads::RightDockWidgetArea, empty);
    window.resize(900, 500);
    window.show();
    QApplication::processEvents();
    CurrentStageTracker tracker(docks, &stages);
    CHECK(stages.currentStage() == one, "the first stage opened is current to begin with");

    docks->setDockWidgetFocused(b);
    CHECK(stages.currentStage() == three && tracker.focusedPanel() == b, "focusing B makes its stage current");
    docks->setDockWidgetFocused(empty);
    CHECK(stages.currentStage() == three, "a panel with no stage keeps the current one");
    docks->setDockWidgetFocused(a);
    CHECK(stages.currentStage() == one, "focusing A makes its stage current");
    a->setStage(two);
    CHECK(stages.currentStage() == two, "the focused panel switching stages switches it too");
    b->setStage(one);
    CHECK(stages.currentStage() == two, "another panel switching doesn't");

    // A script made another stage current; closing it goes back to the
    // focused panel's, not to the first stage open.
    docks->setDockWidgetFocused(b);
    b->setStage(three);
    CHECK(stages.currentStage() == three, "setup: B, focused, shows three");
    stages.setCurrentStage(two);
    stages.closeStage(two);
    CHECK(stages.currentStage() == three, "closed: the focused panel's stage takes over, not the first one");
}

USDCC_TEST(current_stage_undo_and_title) {
    StageManager stages;
    UndoManager undo;
    CHECK(usdcc::usd::windowTitle(stages) == "usdcc", "no stage: just the app's name");
    const StageRefPtr one = stages.createStage();
    const StageRefPtr two = stages.createStage();
    const QString titleOne = usdcc::usd::windowTitle(stages);
    CHECK(titleOne.startsWith("usdcc - "), "the title names the stage: [%s]", qPrintable(titleOne));

    usdcc::usd::CurrentStageUndoGroup group(undo, stages);
    QAction* undoAction = group.createUndoAction(nullptr, QStringLiteral("&Undo"));
    undo.apply(one, "Add Apple", [&]() { return bool(one->usdStage()->DefinePrim(SdfPath("/Apple"))); });
    undo.apply(two, "Add Pear", [&]() { return bool(two->usdStage()->DefinePrim(SdfPath("/Pear"))); });

    CHECK(undoAction->isEnabled() && undoAction->text() == "&Undo Add Apple", "current is one: [%s]",
          qPrintable(undoAction->text()));
    stages.setCurrentStage(two);
    CHECK(undoAction->text() == "&Undo Add Pear", "current is two: [%s]", qPrintable(undoAction->text()));
    CHECK(usdcc::usd::windowTitle(stages) ==
              "usdcc - " + QString::fromStdString(usdcc::usd::shortStageNames(stages.stages())[1]),
          "the title follows: [%s]", qPrintable(usdcc::usd::windowTitle(stages)));
    undoAction->trigger();
    CHECK(!two->usdStage()->GetPrimAtPath(SdfPath("/Pear")) && one->usdStage()->GetPrimAtPath(SdfPath("/Apple")),
          "Undo undoes two's edit, not one's");
    CHECK(!undoAction->isEnabled(), "nothing left to undo on two: disabled");

    // Closing a stage drops its history (main.cpp: UndoManager::removeStage).
    stages.closeStage(two);
    undo.removeStage(two);
    CHECK(stages.currentStage() == one && undoAction->text() == "&Undo Add Apple", "back on one: [%s]",
          qPrintable(undoAction->text()));
    delete undoAction;
}

// Stages > a stage: Make Current, Save Stage and Save Stage As first, then the panel
// types under Views, then Close Stage. Make Current switches the panels on screen only.
USDCC_TEST(stages_menu_make_current) {
    TestMode testMode;
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr one = stages.createStage();
    const StageRefPtr two = stages.createStage();

    MainWindow window;
    ads::CDockManager* docks = window.dockManager();
    auto* left = new OutlinerViewPanel(&stages, one, undo, QStringLiteral("Left"), &window);
    auto* front = new OutlinerViewPanel(&stages, one, undo, QStringLiteral("Front"), &window);
    auto* behind = new OutlinerViewPanel(&stages, one, undo, QStringLiteral("Behind"), &window);
    auto* closed = new OutlinerViewPanel(&stages, one, undo, QStringLiteral("Closed"), &window);
    docks->addDockWidget(ads::LeftDockWidgetArea, left);
    ads::CDockAreaWidget* area = docks->addDockWidget(ads::CenterDockWidgetArea, behind);
    docks->addDockWidgetTabToArea(front, area);
    docks->addDockWidget(ads::RightDockWidgetArea, closed);
    window.resize(900, 500);
    window.show();
    front->setAsCurrentTab();
    closed->toggleView(false);
    QApplication::processEvents();

    usdcc::ui::ViewPanelRegistry types;
    types.add(QStringLiteral("outliner"), QStringLiteral("Outliner"), {});
    usdcc::usd::StagesMenu menu(&stages, &types, docks, &window);
    Q_EMIT menu.aboutToShow();
    const QList<QMenu*> stageMenus = menu.findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
    CHECK(stageMenus.size() == 2, "a submenu per stage: [%d]", int(stageMenus.size()));
    if (stageMenus.size() != 2) return;
    QMenu* second = stageMenus[1];
    const QList<QAction*> actions = second->actions();
    QMenu* views = second->findChild<QMenu*>(QStringLiteral("views"));
    CHECK(actions.size() == 7 && actions[0]->objectName() == QStringLiteral("makeCurrent") &&
              actions[0]->text() == QStringLiteral("Make &Current") && actions[1]->objectName() == QStringLiteral("saveStage") &&
              actions[2]->objectName() == QStringLiteral("saveStageAs") && actions[3]->isSeparator() && views &&
              actions[4] == views->menuAction() && actions[4]->text() == QStringLiteral("&Views") &&
              actions[6]->text() == QStringLiteral("&Close Stage..."),
          "Make Current, Save Stage, Save Stage As..., a separator, Views, a separator, Close Stage...: [%d]",
          int(actions.size()));
    CHECK(!actions[1]->isEnabled() && actions[2]->isEnabled(), "nothing to save in a new stage; Save As can");
    CHECK(views && views->actions().size() == 1 && views->actions()[0]->text() == QStringLiteral("Outliner"),
          "Views lists the panel types");

    actions[0]->trigger();
    CHECK(left->currentStage() == two && front->currentStage() == two, "the panels on screen show it");
    CHECK(behind->currentStage() == one && closed->currentStage() == one, "a tab behind and a closed panel don't");
    CHECK(stages.currentStage() == two, "and it is the current stage");
}

// Stages > a stage > Save Stage saves that stage's edited file layers, whichever stage is
// current. And a panel tab's menu ends with what the app adds (ViewPanel::setTabMenuExtension(),
// main.cpp's Save Stage).
USDCC_TEST(stages_menu_save_stage) {
    TestMode testMode;
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("saved.usda"));
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "a stage file");
        file.write("#usda 1.0\n");
    }
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr edited = stages.openStage(path.toStdString());
    const StageRefPtr other = stages.createStage();
    stages.setCurrentStage(other);
    CHECK(edited && !usdcc::usd::stageHasUnsavedLayers(edited), "opened, nothing to save yet");
    if (!edited) return;
    edited->usdStage()->DefinePrim(PXR_NS::SdfPath("/Saved"));
    CHECK(usdcc::usd::stageHasUnsavedLayers(edited), "edited: something to save");

    MainWindow window;
    usdcc::ui::ViewPanelRegistry types;
    usdcc::usd::StagesMenu menu(&stages, &types, window.dockManager(), &window);
    Q_EMIT menu.aboutToShow();
    const QList<QMenu*> stageMenus = menu.findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
    CHECK(stageMenus.size() == 2, "a submenu per stage");
    if (stageMenus.size() != 2) return;
    QAction* save = stageMenus[0]->findChild<QAction*>(QStringLiteral("saveStage"));
    CHECK(save && save->isEnabled(), "Save Stage is on for the edited stage");
    if (!save) return;
    save->trigger();
    QApplication::processEvents();  // queued, out of the menu
    CHECK(!usdcc::usd::stageHasUnsavedLayers(edited), "and saves it, though another stage is current");
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly) && file.readAll().contains("def \"Saved\""), "into its file");

    // The tab's menu: the panel's own entries, then the extension's.
    auto* panel = new OutlinerViewPanel(&stages, edited, undo, QStringLiteral("Tab"), &window);
    window.dockManager()->addDockWidget(ads::CenterDockWidgetArea, panel);
    window.show();
    QApplication::processEvents();
    usdcc::ui::ViewPanel* extendedFor = nullptr;
    usdcc::ui::ViewPanel::setTabMenuExtension([&extendedFor](usdcc::ui::ViewPanel* shown, QMenu* tabMenu) {
        extendedFor = shown;
        tabMenu->addAction(QStringLiteral("Extended"))->setObjectName(QStringLiteral("extended"));
    });
    QStringList shownActions;
    QTimer::singleShot(0, [&shownActions]() {
        if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (QAction* action : popup->actions()) shownActions << action->objectName();
            popup->close();
        }
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(4, 4), panel->tabWidget()->mapToGlobal(QPoint(4, 4)));
    QApplication::sendEvent(panel->tabWidget(), &event);
    usdcc::ui::ViewPanel::setTabMenuExtension({});
    CHECK(extendedFor == panel && !shownActions.isEmpty() && shownActions.last() == QStringLiteral("extended"),
          "the tab menu ends with the app's entries: %s", qPrintable(shownActions.join(QLatin1Char(','))));
}

// Save Stage As goes on in the new file: it takes the old stage's place in its
// panels, as the current stage, with its selection and unsaved sublayer edits.
USDCC_TEST(save_stage_as_switches_to_the_file) {
    TestMode testMode;
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    QDir(dir.path()).mkpath(QStringLiteral("out"));
    {
        QFile anim(dir.filePath(QStringLiteral("anim.usda")));
        if (anim.open(QIODevice::WriteOnly)) anim.write("#usda 1.0\n");
        QFile shot(dir.filePath(QStringLiteral("shot.usda")));
        if (shot.open(QIODevice::WriteOnly)) shot.write("#usda 1.0\n(\n    subLayers = [@./anim.usda@]\n)\ndef \"Chair\" {}\n");
    }
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr other = stages.createStage();
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;
    const SdfLayerHandle anim = SdfLayer::Find(dir.filePath(QStringLiteral("anim.usda")).toStdString());
    stage->setEditTargetLayer(anim);
    stage->usdStage()->DefinePrim(SdfPath("/Walk"));
    stage->setEditTargetLayer(stage->usdStage()->GetRootLayer());
    stage->usdStage()->DefinePrim(SdfPath("/Unsaved"));
    stages.setSelectedPaths(stage, {SdfPath("/Chair")});
    stages.setCurrentStage(stage);

    MainWindow window;
    ads::CDockManager* docks = window.dockManager();
    auto* shown = new OutlinerViewPanel(&stages, stage, undo, QStringLiteral("Shown"), &window);
    auto* hidden = new OutlinerViewPanel(&stages, stage, undo, QStringLiteral("Hidden"), &window);
    auto* elsewhere = new OutlinerViewPanel(&stages, other, undo, QStringLiteral("Elsewhere"), &window);
    docks->addDockWidget(ads::LeftDockWidgetArea, shown);
    docks->addDockWidget(ads::CenterDockWidgetArea, hidden);
    docks->addDockWidget(ads::RightDockWidgetArea, elsewhere);
    window.show();
    hidden->toggleView(false);
    QApplication::processEvents();

    QString error;
    const QString path = dir.filePath(QStringLiteral("out/shot_v2.usda"));
    const StageRefPtr saved = usdcc::usd::saveStageAsAndSwitch(stages, stage, path.toStdString(), &error);
    CHECK(saved && saved != stage, "a new stage: %s", qPrintable(error));
    if (!saved) return;
    CHECK(QFileInfo(QString::fromStdString(saved->usdStage()->GetRootLayer()->GetRealPath())) == QFileInfo(path) &&
              !saved->usdStage()->GetRootLayer()->IsDirty(),
          "on the new file, saved");
    CHECK(std::find(stages.stages().begin(), stages.stages().end(), stage) == stages.stages().end() &&
              stages.stages().size() == 2,
          "in the old stage's place");
    CHECK(shown->currentStage() == saved && hidden->currentStage() == saved && elsewhere->currentStage() == other,
          "its panels, closed ones too, show the new one; others don't change");
    CHECK(stages.currentStage() == saved, "it is current");
    CHECK(stages.selectedPaths(saved) == std::vector<SdfPath>({SdfPath("/Chair")}), "with the selection");
    CHECK(saved->usdStage()->GetPrimAtPath(SdfPath("/Unsaved")) && saved->usdStage()->GetPrimAtPath(SdfPath("/Walk")),
          "the root's unsaved edit, and the sublayer's, still unsaved");
    CHECK(anim && anim->IsDirty() && saved->usdStage()->HasLocalLayer(anim), "in the same sublayer");

    // Its own file: a plain save, the same stage.
    saved->usdStage()->DefinePrim(SdfPath("/More"));
    CHECK(usdcc::usd::saveStageAsAndSwitch(stages, saved, path.toStdString(), &error) == saved &&
              !saved->usdStage()->GetRootLayer()->IsDirty(),
          "its own file: saved in place");
}
