// The Dependencies panel (docs/COMPOSITION_ARCS.md §7): a row per file,
// missing ones red and first, the prims under each; selecting the prims
// using a file; unloading and loading a payload everywhere as undo steps;
// and following the stage's changes. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "dependency_files.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/dependencies_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>

#include <cstring>
#include <memory>

using usdcc::usd::Dependency;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int indexOf(const usdcc::usd::DependenciesViewPanel& panel, const char* name) {
    for (int i = 0; i < int(panel.dependencies().size()); ++i) {
        const std::string& path = panel.dependencies()[i].resolvedPath;
        if (path.size() >= strlen(name) && path.compare(path.size() - strlen(name), std::string::npos, name) == 0) return i;
    }
    return -1;
}

QAction* action(QMenu& menu, const QString& text) {
    for (QAction* entry : menu.actions()) {
        if (entry->text() == text) return entry;
    }
    return nullptr;
}

// Until the panel's coalescing timer has refreshed it.
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 300) QApplication::processEvents();
}
}  // namespace

USDCC_TEST(dependencies_panel) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    usdcc::test::writeDependencyFiles(dir);
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("root.usda")).toStdString());
    CHECK(stage, "the stage opens");
    if (!stage) return;

    usdcc::usd::DependenciesViewPanel panel(&stages, stage, undo);
    panel.setShowAssetAttributes(false);
    panel.resize(800, 500);
    panel.show();
    auto* tree = panel.findChild<QTreeWidget*>();
    CHECK(tree && tree->topLevelItemCount() == 6, "a row per file: [%d]", tree ? tree->topLevelItemCount() : -1);
    if (!tree) return;
    CHECK(tree->topLevelItem(0)->text(0) == "missing.usda" && tree->topLevelItem(0)->text(3) == "missing" &&
              tree->topLevelItem(0)->foreground(0).color() != tree->topLevelItem(5)->foreground(0).color(),
          "the missing file first, in its own colour");

    const int chair = indexOf(panel, "chair.usda");
    CHECK(chair >= 0 && tree->topLevelItem(chair)->childCount() == 5 && tree->topLevelItem(chair)->text(2) == "5 prims",
          "chair.usda: its five prims under it");
    panel.selectPrimsUsing(chair);
    CHECK(stages.selectedPaths(stage).size() == 5, "Select Prims Using It");

    // Unload, then load, the payload everywhere.
    const int heavy = indexOf(panel, "heavy.usda");
    QUndoStack* stack = undo->stack(stage);
    {
        QMenu menu;
        panel.populateFileMenu(&menu, heavy);
        QAction* unload = action(menu, "Unload Everywhere");
        CHECK(unload && unload->isEnabled() && action(menu, "Load Everywhere") &&
                  !action(menu, "Load Everywhere")->isEnabled(),
              "loaded: Unload Everywhere");
        if (unload) unload->trigger();
    }
    settle();
    const int heavyAfter = indexOf(panel, "heavy.usda");
    CHECK(heavyAfter >= 0 && panel.dependencies()[heavyAfter].unloaded.size() == 1 &&
              tree->topLevelItem(heavyAfter)->text(3) == "1 unloaded",
          "the panel follows: 1 unloaded");
    CHECK(stack && stack->canUndo(), "an undo step");
    if (stack) stack->undo();
    settle();
    CHECK(panel.dependencies()[indexOf(panel, "heavy.usda")].unloaded.empty(), "undo loads it again");

    // An edit adding a reference shows up.
    stage->usdStage()->GetPrimAtPath(SdfPath("/T")).GetReferences().AddReference("./leg.usda", SdfPath("/Leg"));
    settle();
    const int leg = indexOf(panel, "leg.usda");
    CHECK(leg >= 0 && panel.dependencies()[leg].prims.size() == 5, "a new reference is listed");

    panel.setShowAssetAttributes(true);
    CHECK(indexOf(panel, "wood.png") >= 0, "Show Asset Attributes adds the texture");
    panel.setShowAssetAttributes(false);
    QStandardPaths::setTestModeEnabled(false);
}
