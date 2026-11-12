// A stage with a single root prim (say /World) opens in the Outliner with that prim
// expanded: on a stage load, a stage switch in the panel, and when an edit leaves one
// root. A root the user collapsed, or a session restored collapsed, stays collapsed.
// Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTreeView>

#include <memory>

using usdcc::usd::OutlinerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
using usdcc::usd::UsdPrimTreeModel;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

struct Scene {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini"))};
    StageManager manager;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();

    StageRefPtr stageWith(std::initializer_list<const char*> paths) {
        StageRefPtr stage = manager.createStage();
        for (const char* path : paths) stage->usdStage()->DefinePrim(SdfPath(path), TfToken("Xform"));
        return stage;
    }
};

// The panel's tree, and its rows by prim path.
struct Tree {
    QTreeView* view;
    QSortFilterProxyModel* proxy;
    UsdPrimTreeModel* model;
    explicit Tree(OutlinerViewPanel& panel) : view(panel.findChild<QTreeView*>()) {
        proxy = static_cast<QSortFilterProxyModel*>(view->model());
        model = static_cast<UsdPrimTreeModel*>(proxy->sourceModel());
    }
    QModelIndex row(const char* path) const { return proxy->mapFromSource(model->fetchPath(SdfPath(path))); }
    bool expanded(const char* path) const { return view->isExpanded(row(path)); }
};
}  // namespace

USDCC_TEST(outliner_single_root_expands_on_load) {
    Scene s;
    const StageRefPtr world = s.stageWith({"/World", "/World/Geo", "/World/Geo/Cube"});
    s.manager.setCurrentStage(world);
    OutlinerViewPanel panel(&s.manager, world, s.undo, QStringLiteral("Outliner"));
    panel.show();
    settle();
    Tree tree(panel);
    CHECK(tree.row("/World").isValid() && tree.expanded("/World"), "the lone root opens with the stage");
    CHECK(!tree.expanded("/World/Geo"), "and only it: its children stay closed");

    const StageRefPtr two = s.stageWith({"/A", "/A/Kid", "/B", "/B/Kid"});
    OutlinerViewPanel twoRoots(&s.manager, two, s.undo, QStringLiteral("Outliner"));
    twoRoots.show();
    settle();
    Tree other(twoRoots);
    CHECK(!other.expanded("/A") && !other.expanded("/B"), "several roots stay closed");
}

USDCC_TEST(outliner_single_root_expands_on_stage_switch) {
    Scene s;
    const StageRefPtr two = s.stageWith({"/A", "/A/Kid", "/B", "/B/Kid"});
    const StageRefPtr world = s.stageWith({"/World", "/World/Geo"});
    OutlinerViewPanel panel(&s.manager, two, s.undo, QStringLiteral("Outliner"));
    panel.show();
    settle();
    Tree tree(panel);
    CHECK(!tree.expanded("/A"), "two roots: closed");

    panel.setStage(world);
    settle();
    CHECK(tree.expanded("/World"), "switching to a stage with one root opens it");

    tree.view->collapse(tree.row("/World"));
    panel.setStage(two);
    panel.setStage(world);
    settle();
    CHECK(tree.expanded("/World"), "and again on each switch to it");
}

USDCC_TEST(outliner_single_root_expands_after_resync) {
    Scene s;
    const StageRefPtr stage = s.stageWith({"/World", "/World/Geo", "/Extra"});
    OutlinerViewPanel panel(&s.manager, stage, s.undo, QStringLiteral("Outliner"));
    panel.show();
    settle();
    Tree tree(panel);
    CHECK(!tree.expanded("/World"), "two roots: closed");

    stage->usdStage()->RemovePrim(SdfPath("/Extra"));
    settle();
    CHECK(tree.expanded("/World"), "an edit that leaves one root opens it");

    // The user's collapse holds while that root stays the lone one.
    tree.view->collapse(tree.row("/World"));
    stage->usdStage()->DefinePrim(SdfPath("/World/New"), TfToken("Xform"));
    settle();
    CHECK(tree.row("/World/New").isValid(), "the tree rebuilt");
    CHECK(!tree.expanded("/World"), "a collapsed lone root stays collapsed through other edits");

    // From an empty stage, the first root prim opens.
    const StageRefPtr empty = s.stageWith({});
    panel.setStage(empty);
    settle();
    empty->usdStage()->DefinePrim(SdfPath("/Scene"), TfToken("Xform"));
    empty->usdStage()->DefinePrim(SdfPath("/Scene/Child"), TfToken("Xform"));
    settle();
    CHECK(tree.expanded("/Scene"), "the first root prim made on an empty stage opens");
}

USDCC_TEST(outliner_single_root_keeps_session_state) {
    Scene s;
    const StageRefPtr world = s.stageWith({"/World", "/World/Geo", "/World/Geo/Cube"});
    OutlinerViewPanel panel(&s.manager, world, s.undo, QStringLiteral("Outliner"));
    panel.show();
    settle();
    Tree tree(panel);
    tree.view->collapse(tree.row("/World"));
    const QJsonObject collapsed = panel.saveSessionState();
    CHECK(collapsed.value(QStringLiteral("expanded")).toArray().isEmpty(), "saved with nothing open");

    // A session's panel is made for the stage (opening the root), then gets its state.
    OutlinerViewPanel restored(&s.manager, world, s.undo, QStringLiteral("Outliner"));
    restored.show();
    settle();
    Tree again(restored);
    CHECK(again.expanded("/World"), "before the session state: open by default");
    restored.restoreSessionState(collapsed);
    settle();
    CHECK(!again.expanded("/World"), "a session saved with the root collapsed restores it collapsed");

    tree.view->expand(tree.row("/World/Geo"));  // and its parent, which shows it
    tree.view->expand(tree.row("/World"));
    restored.restoreSessionState(panel.saveSessionState());
    settle();
    CHECK(again.expanded("/World") && again.expanded("/World/Geo"), "and one saved open restores open");
}
