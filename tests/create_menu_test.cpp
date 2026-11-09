// The Create menu (docs/CREATE_MENU.md): its entries and their icons, where a prim
// goes, selecting it, and the Outliner showing it. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/create_menu.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"
#include "usdcc/usd/variant_actions.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QStringList>
#include <QStandardPaths>
#include <QTimer>
#include <QTemporaryDir>
#include <QTreeView>
#include <QUndoStack>

#include <memory>

using usdcc::usd::PrimCreateMenu;
using usdcc::usd::OutlinerViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
namespace prim_icons = usdcc::usd::prim_icons;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60) QApplication::processEvents();
}

QAction* entry(QMenu* menu, const QString& text) {
    for (QAction* action : menu->actions()) {
        if (action->text() == text) return action;
    }
    return nullptr;
}

QAction* entry(QMenu* menu, const QString& submenu, const QString& text) {
    QAction* holder = entry(menu, submenu);
    return holder && holder->menu() ? entry(holder->menu(), text) : nullptr;
}

QString firstLine(QMenu* menu) { return menu->actions().isEmpty() ? QString() : menu->actions().front()->text(); }

bool sameIcon(const QAction* action, const char* type) {
    return action && !action->icon().isNull() &&
           action->icon().cacheKey() == prim_icons::icon(prim_icons::iconNameForType(TfToken(type))).cacheKey();
}

struct Scene {
    // Its own, as UI tests have: one left behind would stand in for a later test's.
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini"))};
    StageManager manager;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    QWidget window;
    Scene() { QStandardPaths::setTestModeEnabled(true); }
    ~Scene() { QStandardPaths::setTestModeEnabled(false); }
};
}  // namespace

USDCC_TEST(create_menu_lists_everything_disabled_with_no_stage) {
    Scene s;
    PrimCreateMenu menu(&s.manager, s.undo, &s.window);
    CHECK(menu.menuAction()->isEnabled(), "it opens with no stage");
    menu.rebuild();
    CHECK(firstLine(&menu) == QStringLiteral("Prim (no type)"), "starting with the types, with no line above: [%s]", qPrintable(firstLine(&menu)));

    QAction* typeless = entry(&menu, QStringLiteral("Prim (no type)"));
    QAction* xform = entry(&menu, QStringLiteral("Xform"));
    QAction* cube = entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Cube"));
    CHECK(typeless && xform && cube, "and listing what can be made");
    if (!typeless || !xform || !cube) return;
    CHECK(!typeless->isEnabled() && !xform->isEnabled() && !cube->isEnabled(), "all disabled");
    CHECK(sameIcon(cube, "Cube") && !cube->toolTip().isEmpty(), "with their icons and tooltips");
    QAction* geometry = entry(&menu, QStringLiteral("Geometry"));
    CHECK(geometry && geometry->isEnabled(), "a submenu can still be opened, to see what's in it");
    cube->trigger();
    CHECK(s.manager.stages().empty(), "choosing one does nothing");

    // Opening a stage and closing it, with the menu built each time it opens.
    const StageRefPtr stage = s.manager.createStage();
    s.manager.setCurrentStage(stage);
    menu.rebuild();
    CHECK(firstLine(&menu) == QStringLiteral("At the root of the stage"), "with a stage it says where");
    CHECK(entry(&menu, QStringLiteral("Xform"))->isEnabled(), "and its entries work");
    s.manager.closeStage(stage);
    menu.rebuild();
    CHECK(firstLine(&menu) == QStringLiteral("Prim (no type)"), "and back when it closes");
    CHECK(!entry(&menu, QStringLiteral("Xform"))->isEnabled(), "disabled again");
}

USDCC_TEST(create_menu_entries_and_icons) {
    Scene s;
    const StageRefPtr stage = s.manager.createStage();
    s.manager.setCurrentStage(stage);
    PrimCreateMenu menu(&s.manager, s.undo, &s.window);
    menu.rebuild();

    CHECK(firstLine(&menu) == QStringLiteral("At the root of the stage") && !menu.actions().front()->isEnabled(), "says where");
    QAction* typeless = entry(&menu, QStringLiteral("Prim (no type)"));
    CHECK(typeless && menu.actions().indexOf(typeless) == 2, "Prim (no type) first, after the line");
    CHECK(typeless && !typeless->toolTip().isEmpty(), "saying what it's for");
    CHECK(sameIcon(typeless, ""), "with the Outliner's dashed circle");
    CHECK(sameIcon(entry(&menu, QStringLiteral("Xform")), "Xform"), "Xform at the top");
    CHECK(sameIcon(entry(&menu, QStringLiteral("Camera")), "Camera"), "Camera too");

    QAction* cube = entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Cube"));
    CHECK(sameIcon(cube, "Cube"), "a submenu's entry, with its own icon");
    CHECK(cube && !cube->toolTip().isEmpty(), "and its schema's documentation");
    CHECK(sameIcon(entry(&menu, QStringLiteral("Lights"), QStringLiteral("RectLight")), "RectLight"), "each light its own");
    CHECK(sameIcon(entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Capsule")), "Capsule"), "a family's icon");
    CHECK(sameIcon(entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Capsule_1")), "Capsule_1"),
          "and its newer version on a line of its own");
    CHECK(entry(&menu, QStringLiteral("Lights"), QStringLiteral("DomeLight")) &&
              entry(&menu, QStringLiteral("Lights"), QStringLiteral("DomeLight_1")),
          "both dome lights");
    CHECK(sameIcon(entry(&menu, QStringLiteral("Geometry")), "Cube"), "a submenu has its first entry's icon");
    CHECK(!entry(&menu, QStringLiteral("Plugins")), "no Plugins with nothing in it");

    // Opened twice: submenus don't pile up.
    menu.rebuild();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // no event loop runs them here
    CHECK(menu.findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly).size() ==
              usdcc::usd::PrimCreationCatalog::instance().categories().size() - 1,
          "one set of submenus");
}

USDCC_TEST(create_menu_creates_where_it_says) {
    Scene s;
    const StageRefPtr stage = s.manager.createStage();
    s.manager.setCurrentStage(stage);
    PrimCreateMenu menu(&s.manager, s.undo, &s.window);
    menu.rebuild();

    entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Cube"))->trigger();
    const SdfPath cube("/Cube");
    CHECK(stage->usdStage()->GetPrimAtPath(cube).GetTypeName() == TfToken("Cube"), "made at the root");
    CHECK(s.manager.selectedPaths(stage) == std::vector<SdfPath>{cube}, "and selected");
    CHECK(s.undo->stack(stage)->count() == 1, "one undo step");

    menu.rebuild();
    CHECK(firstLine(&menu) == QStringLiteral("In /Cube"), "the line follows the selection");
    entry(&menu, QStringLiteral("Prim (no type)"))->trigger();
    const SdfPath typeless("/Cube/Prim");
    const UsdPrim prim = stage->usdStage()->GetPrimAtPath(typeless);
    CHECK(prim && prim.GetTypeName().IsEmpty(), "with no type, under it");

    // Where it can't go: every entry says no, and the first line why.
    stage->usdStage()->GetPrimAtPath(cube).SetActive(false);
    s.manager.setSelectedPaths(stage, {cube});
    menu.rebuild();
    CHECK(firstLine(&menu).contains(QStringLiteral("inactive")), "says why not");
    QAction* sphere = entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Sphere"));
    CHECK(sphere && !sphere->isEnabled(), "entries disabled");
    CHECK(!entry(&menu, QStringLiteral("Prim (no type)"))->isEnabled(), "all of them");
    stage->usdStage()->GetPrimAtPath(cube).SetActive(true);

    // In a variant.
    const SdfPath xform("/Xform");
    s.manager.setSelectedPaths(stage, {});
    menu.rebuild();
    entry(&menu, QStringLiteral("Xform"))->trigger();
    usdcc::usd::variant_actions::addSet(*s.undo, stage, xform, "look", "red");
    stage->enterVariantEdit(xform, "look", "red");
    s.manager.setSelectedPaths(stage, {cube});
    menu.rebuild();
    CHECK(firstLine(&menu) == QStringLiteral("In /Xform, in look=red"), "says the variant");
    entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Sphere"))->trigger();
    CHECK(stage->usdStage()->GetRootLayer()->GetPrimAtPath(SdfPath("/Xform{look=red}Sphere")), "and goes in it");
    stage->exitVariantEdit();
}

USDCC_TEST(create_in_the_prim_menu) {
    Scene s;
    const StageRefPtr stage = s.manager.createStage();
    const UsdStageRefPtr usd = stage->usdStage();
    const SdfPath a("/A");
    const SdfPath b("/B");
    usd->DefinePrim(a, TfToken("Xform"));
    usd->DefinePrim(b, TfToken("Xform"));
    usdcc::usd::PrimMenuContext context;
    context.undo = s.undo.get();
    context.stageManager = &s.manager;
    context.stage = stage;
    context.dialogParent = &s.window;

    // Several selected: it goes under the one clicked.
    s.manager.setSelectedPaths(stage, {a, b});
    context.paths = {a, b};
    context.clicked = b;
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, context);
        QAction* create = entry(&menu, QStringLiteral("Create"));
        CHECK(create && create->menu() && create->isEnabled(), "a Create submenu");
        if (!create) return;
        QMenu* sub = create->menu();
        CHECK(firstLine(sub) == QStringLiteral("In /B"), "saying where");
        CHECK(sameIcon(entry(sub, QStringLiteral("Prim (no type)")), ""), "Prim (no type), with its icon");
        CHECK(sameIcon(entry(sub, QStringLiteral("Xform")), "Xform"), "Xform");
        QAction* cube = entry(sub, QStringLiteral("Geometry"), QStringLiteral("Cube"));
        CHECK(sameIcon(cube, "Cube"), "the same categories as the menu bar's");
        cube->trigger();
    }
    const SdfPath made("/B/Cube");
    CHECK(usd->GetPrimAtPath(made).GetTypeName() == TfToken("Cube"), "under the clicked prim");
    CHECK(!usd->GetPrimAtPath(SdfPath("/A/Cube")), "not the other selected one");
    CHECK(s.manager.selectedPaths(stage) == std::vector<SdfPath>{made}, "and selected");
    CHECK(s.undo->stack(stage)->count() == 1, "one undo step");
    s.undo->stack(stage)->undo();
    CHECK(!usd->GetPrimAtPath(made), "undone");

    // With no type.
    context.paths = {a};
    context.clicked = a;
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, context);
        entry(entry(&menu, QStringLiteral("Create"))->menu(), QStringLiteral("Prim (no type)"))->trigger();
    }
    const UsdPrim typeless = usd->GetPrimAtPath(SdfPath("/A/Prim"));
    CHECK(typeless && typeless.GetTypeName().IsEmpty(), "a prim with no type");

    // Where it can't go: disabled, saying why.
    usd->GetPrimAtPath(b).SetActive(false);
    context.paths = {b};
    context.clicked = b;
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, context);
        QAction* create = entry(&menu, QStringLiteral("Create"));
        CHECK(create && !create->isEnabled() && create->toolTip().contains(QStringLiteral("inactive")), "an inactive prim");
    }
    usd->GetPrimAtPath(b).SetActive(true);

    // In a variant: under the clicked prim when it reaches it, not otherwise.
    usdcc::usd::variant_actions::addSet(*s.undo, stage, a, "look", "red");
    stage->enterVariantEdit(a, "look", "red");
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, context);
        QAction* create = entry(&menu, QStringLiteral("Create"));
        CHECK(create && !create->isEnabled() && create->toolTip().contains(QStringLiteral("doesn't reach")), "outside the variant");
    }
    context.paths = {a};
    context.clicked = a;
    {
        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, context);
        QMenu* sub = entry(&menu, QStringLiteral("Create"))->menu();
        CHECK(firstLine(sub) == QStringLiteral("In /A, in look=red"), "inside it, saying so");
        entry(sub, QStringLiteral("Geometry"), QStringLiteral("Sphere"))->trigger();
    }
    CHECK(usd->GetRootLayer()->GetPrimAtPath(SdfPath("/A{look=red}Sphere")), "and it goes in the variant");
    stage->exitVariantEdit();
}

USDCC_TEST(create_on_the_outliners_empty_space) {
    Scene s;
    const StageRefPtr stage = s.manager.createStage();
    const UsdStageRefPtr usd = stage->usdStage();
    const SdfPath world("/World");
    usd->DefinePrim(world, TfToken("Xform"));
    usd->DefinePrim(SdfPath("/World/Child"), TfToken("Xform"));
    s.manager.setCurrentStage(stage);
    s.manager.setSelectedPaths(stage, {world});

    {   // At the root, though something is selected, which the menu bar's would go under.
        QMenu menu;
        usdcc::usd::populateEmptySpaceMenu(&menu, &s.manager, stage, s.undo.get());
        CHECK(menu.actions().size() == 1, "only Create: %d", int(menu.actions().size()));
        QAction* create = entry(&menu, QStringLiteral("Create"));
        CHECK(create && create->menu() && create->isEnabled(), "a Create submenu");
        if (!create) return;
        QMenu* sub = create->menu();
        CHECK(firstLine(sub) == QStringLiteral("At the root of the stage"), "saying where: [%s]", qPrintable(firstLine(sub)));
        CHECK(sameIcon(entry(sub, QStringLiteral("Geometry"), QStringLiteral("Cube")), "Cube"), "with the usual entries");
        entry(sub, QStringLiteral("Geometry"), QStringLiteral("Cube"))->trigger();
    }
    const SdfPath cube("/Cube");
    CHECK(usd->GetPrimAtPath(cube).GetTypeName() == TfToken("Cube"), "made at the root");
    CHECK(!usd->GetPrimAtPath(SdfPath("/World/Cube")), "not under the selection");
    CHECK(s.manager.selectedPaths(stage) == std::vector<SdfPath>{cube}, "and selected");
    CHECK(s.undo->stack(stage)->count() == 1 && s.undo->stack(stage)->canUndo(), "one undo step");
    s.undo->stack(stage)->undo();
    CHECK(!usd->GetPrimAtPath(cube), "undone");

    {   // With no type.
        QMenu menu;
        usdcc::usd::populateEmptySpaceMenu(&menu, &s.manager, stage, s.undo.get());
        entry(entry(&menu, QStringLiteral("Create"))->menu(), QStringLiteral("Prim (no type)"))->trigger();
    }
    const UsdPrim typeless = usd->GetPrimAtPath(SdfPath("/Prim"));
    CHECK(typeless && typeless.GetTypeName().IsEmpty(), "a prim with no type at the root");

    // While editing a variant the root is out of reach: not into the variant instead.
    usdcc::usd::variant_actions::addSet(*s.undo, stage, world, "look", "red");
    stage->enterVariantEdit(world, "look", "red");
    {
        QMenu menu;
        usdcc::usd::populateEmptySpaceMenu(&menu, &s.manager, stage, s.undo.get());
        QAction* create = entry(&menu, QStringLiteral("Create"));
        CHECK(create && !create->isEnabled(), "disabled in a variant");
        CHECK(create && create->toolTip().contains(QStringLiteral("doesn't reach")), "saying why: [%s]", create ? qPrintable(create->toolTip()) : "");
    }
    stage->exitVariantEdit();

    {   // No stage.
        QMenu menu;
        usdcc::usd::populateEmptySpaceMenu(&menu, &s.manager, nullptr, s.undo.get());
        QAction* create = entry(&menu, QStringLiteral("Create"));
        CHECK(create && !create->isEnabled() && create->toolTip().contains(QStringLiteral("No stage")), "disabled with no stage");
    }

    // Through the real right-click, on the tree's empty space and on a row.
    OutlinerViewPanel outliner(&s.manager, stage, s.undo, QStringLiteral("Outliner"));
    outliner.resize(300, 400);
    outliner.show();
    settle();
    auto* tree = outliner.findChild<QTreeView*>();
    CHECK(tree != nullptr, "the tree");
    if (!tree) return;
    QStringList entries;
    const auto rightClickAt = [&](const QPoint& point) {
        entries.clear();
        QTimer closer;
        closer.setInterval(20);
        QObject::connect(&closer, &QTimer::timeout, [&]() {
            if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* action : popup->actions()) entries << action->text();
                popup->close();
            }
        });
        closer.start();
        Q_EMIT tree->customContextMenuRequested(point);
    };
    const QPoint emptySpace(tree->viewport()->width() / 2, tree->viewport()->height() - 5);
    CHECK(!tree->indexAt(emptySpace).isValid(), "a point on empty space");
    s.manager.setSelectedPaths(stage, {world});
    settle();
    rightClickAt(emptySpace);
    CHECK(entries == QStringList{QStringLiteral("Create")}, "empty space: only Create: [%s]", qPrintable(entries.join(u',')));
    CHECK(s.manager.selectedPaths(stage) == std::vector<SdfPath>{world}, "and the selection stays");
    const QPoint onRow = tree->visualRect(tree->model()->index(0, 0)).center();
    rightClickAt(onRow);
    CHECK(entries.size() > 3 && entries.contains(QStringLiteral("Create")), "a row keeps its full menu: %d entries", int(entries.size()));
}

USDCC_TEST(create_menu_outliner_shows_the_new_prim) {
    Scene s;
    const StageRefPtr stage = s.manager.createStage();
    s.manager.setCurrentStage(stage);
    stage->usdStage()->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    stage->usdStage()->DefinePrim(SdfPath("/World/Group"), TfToken("Xform"));
    OutlinerViewPanel outliner(&s.manager, stage, s.undo, QStringLiteral("Outliner"));
    outliner.resize(300, 400);
    outliner.show();
    settle();
    auto* tree = outliner.findChild<QTreeView*>();
    CHECK(tree != nullptr, "the tree");
    if (!tree) return;

    PrimCreateMenu menu(&s.manager, s.undo, &s.window);
    s.manager.setSelectedPaths(stage, {SdfPath("/World/Group")});
    settle();
    tree->collapseAll();
    settle();
    menu.rebuild();
    entry(&menu, QStringLiteral("Geometry"), QStringLiteral("Sphere"))->trigger();
    settle();

    const QModelIndexList selected = tree->selectionModel()->selectedIndexes();
    CHECK(selected.size() == 1 && selected.front().data().toString() == QStringLiteral("Sphere"), "selected in the tree");
    if (!selected.isEmpty()) {
        bool shown = true;
        for (QModelIndex parent = selected.front().parent(); parent.isValid(); parent = parent.parent()) {
            shown = shown && tree->isExpanded(parent);
        }
        CHECK(shown, "with its parents expanded, though they were collapsed");
        CHECK(tree->visualRect(selected.front()).isValid() && tree->viewport()->rect().intersects(tree->visualRect(selected.front())), "in view");
    }
}
