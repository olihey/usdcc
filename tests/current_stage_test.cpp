// The current stage (docs/CURRENT_STAGE.md): it follows the focused view
// panel (CurrentStageTracker), Edit > Undo acts on it and names the step
// (CurrentStageUndoGroup), and the window title says which it is. Runs in
// usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/current_stage_tracker.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/current_stage_ui.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/stage_names.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QAction>
#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUndoStack>

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
