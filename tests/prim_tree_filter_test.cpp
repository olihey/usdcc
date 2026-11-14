// The search and type filter above the Outliner and the dialogs' prim trees: a match shows
// with all its ancestors, and clearing it puts back what was open.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/prim_tree_filter.h"
#include "usdcc/usd/prim_tree_widget.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>

#include <map>
#include <memory>
#include <set>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

using usdcc::usd::PrimTreeFilterBar;
using usdcc::usd::PrimTreeFilterProxy;
using usdcc::usd::StageManager;
using usdcc::usd::UndoManager;
using usdcc::usd::UsdPrimTreeModel;

namespace {
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 80) QApplication::processEvents();
}

constexpr auto kScene = R"(#usda 1.0
def Xform "World"
{
    def Mesh "Chair"
    {
        def Mesh "Leg"
        {
        }
    }
    def Sphere "Lamp"
    {
    }
}
def Scope "Looks"
{
    def Material "Wood"
    {
    }
}
def "Untyped"
{
}
)";

// The names shown, depth first, as "World/Chair/Leg".
std::set<std::string> shown(QTreeView* tree) {
    std::set<std::string> names;
    std::vector<std::pair<QModelIndex, std::string>> pending{{QModelIndex(), std::string()}};
    QAbstractItemModel* model = tree->model();
    while (!pending.empty()) {
        const auto [parent, prefix] = pending.back();
        pending.pop_back();
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const QModelIndex index = model->index(row, 0, parent);
            const std::string name = prefix + index.data().toString().toStdString();
            names.insert(name);
            pending.push_back({index, name + "/"});
        }
    }
    return names;
}

struct Fixture {
    TestSettings settings;
    QTemporaryDir temp;
    std::unique_ptr<usdcc::ui::UiScaleController> scale;
    StageManager stages;
    usdcc::usd::StageRefPtr stage;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();

    Fixture() {
        scale = std::make_unique<usdcc::ui::UiScaleController>(*static_cast<QApplication*>(QCoreApplication::instance()), temp.filePath("ui.ini"));
        const QString path = temp.filePath("scene.usda");
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "test scene opens for writing");
        file.write(kScene);
        file.close();
        stage = stages.openStage(path.toStdString());
    }
};
}  // namespace

USDCC_TEST(prim_tree_filter_outliner_search) {
    Fixture f;
    usdcc::usd::OutlinerViewPanel panel(&f.stages, f.stage, f.undo, "Outliner");
    panel.resize(400, 500);
    panel.show();
    settle();
    auto* tree = panel.findChild<QTreeView*>();
    auto* bar = panel.findChild<PrimTreeFilterBar*>();
    CHECK(tree && bar, "the Outliner has a filter bar");
    if (!tree || !bar) return;
    auto* proxy = static_cast<PrimTreeFilterProxy*>(tree->model());
    auto* model = proxy->primModel();

    // Only World open, as someone left it.
    tree->collapseAll();
    tree->expand(proxy->mapFromSource(model->fetchPath(SdfPath("/World"))));
    settle();

    bar->setNameFilter("leg");
    settle();
    CHECK(shown(tree) == (std::set<std::string>{"World", "World/Chair", "World/Chair/Leg"}), "a match shows with its parents, and nothing else");
    CHECK(tree->isExpanded(proxy->mapFromSource(model->fetchPath(SdfPath("/World/Chair")))), "opened down to it");
    CHECK(proxy->matchCount() == 1, "one match, not counting its parents: %d", proxy->matchCount());

    bar->setNameFilter("LAMP");
    settle();
    CHECK(shown(tree).count("World/Lamp") == 1, "case doesn't matter");

    // Typing waits for a pause, then filters.
    bar->searchEdit()->setText("wood");
    settle();
    settle();
    settle();
    settle();
    CHECK(shown(tree) == (std::set<std::string>{"Looks", "Looks/Wood"}), "typing filters too");

    bar->clear();
    settle();
    CHECK(shown(tree).count("World/Lamp") == 1 && shown(tree).count("Untyped") == 1, "clearing shows everything");
    CHECK(tree->isExpanded(proxy->mapFromSource(model->fetchPath(SdfPath("/World")))), "and what was open is open");
    CHECK(!tree->isExpanded(proxy->mapFromSource(model->fetchPath(SdfPath("/World/Chair")))), "and what wasn't isn't");

    // Selecting what's filtered out doesn't trouble it, and selecting in the tree still works.
    bar->setNameFilter("leg");
    settle();
    f.stages.setSelectedPaths(f.stage, {SdfPath("/World/Lamp")});
    settle();
    CHECK(tree->selectionModel()->selectedRows().isEmpty(), "a hidden prim isn't selected in the tree");
    tree->selectionModel()->setCurrentIndex(proxy->mapFromSource(model->fetchPath(SdfPath("/World/Chair/Leg"))), QItemSelectionModel::ClearAndSelect);
    settle();
    CHECK(f.stages.selectedPaths(f.stage) == std::vector<SdfPath>{SdfPath("/World/Chair/Leg")}, "selecting a shown one selects that prim");

    // An edit rebuilds the tree, and the filter stays.
    f.stage->usdStage()->DefinePrim(SdfPath("/World/LegRest"), TfToken("Cube"));
    settle();
    settle();
    CHECK(shown(tree).count("World/LegRest") == 1 && shown(tree).count("World/Lamp") == 0, "a new prim that matches shows, filtered: %d", int(shown(tree).size()));
}

USDCC_TEST(prim_tree_filter_types) {
    Fixture f;
    usdcc::usd::OutlinerViewPanel panel(&f.stages, f.stage, f.undo, "Outliner");
    panel.show();
    settle();
    auto* tree = panel.findChild<QTreeView*>();
    auto* bar = panel.findChild<PrimTreeFilterBar*>();
    if (!tree || !bar) return;

    const auto types = bar->stageTypes();
    std::map<std::string, int> counts(types.begin(), types.end());
    CHECK(counts["Mesh"] == 2 && counts["Material"] == 1 && counts["Xform"] == 1, "the types the stage has, with how many");
    CHECK(counts.count("") == 1, "and prims with none");
    CHECK(!counts.count("Camera"), "and only those");

    bar->setTypeFilter(std::set<std::string>{"Material"});
    settle();
    CHECK(shown(tree) == (std::set<std::string>{"Looks", "Looks/Wood"}), "a type shows with its parents");
    CHECK(bar->typesButton()->text() == "Material", "the button says which: %s", qPrintable(bar->typesButton()->text()));

    bar->setTypeFilter(std::set<std::string>{"Material", "Mesh"});
    settle();
    CHECK(shown(tree).count("World/Chair/Leg") == 1 && shown(tree).count("Looks/Wood") == 1 && shown(tree).count("World/Lamp") == 0, "several at once");
    CHECK(bar->typesButton()->text() == "Material, Mesh", "the button names them: %s", qPrintable(bar->typesButton()->text()));
    CHECK(bar->typesButton()->toolTip().contains("Material, Mesh"), "and so does its tooltip");
    bar->setTypeFilter(std::set<std::string>{"Material", "Mesh", "Sphere"});
    CHECK(bar->typesButton()->text() == "Material, Mesh +1", "and says how many more: %s", qPrintable(bar->typesButton()->text()));
    CHECK(bar->typesButton()->toolTip().contains("Sphere"), "which the tooltip names");
    bar->setTypeFilter(std::set<std::string>{"Material", "Mesh"});
    settle();
    Q_EMIT bar->typesButton()->menu()->aboutToShow();
    QStringList bold;
    for (QAction* action : bar->typesButton()->menu()->actions()) {
        if (action->font().bold()) bold << action->text();
    }
    CHECK(bold == (QStringList{"Material  (1)", "Mesh  (2)"}), "the menu shows which are ticked in bold: %s", qPrintable(bold.join(",")));

    // With a name too: both must match.
    bar->setNameFilter("leg");
    settle();
    CHECK(shown(tree) == (std::set<std::string>{"World", "World/Chair", "World/Chair/Leg"}), "name and type together");
    bar->setNameFilter("lamp");
    settle();
    CHECK(shown(tree).empty(), "a name of the wrong type shows nothing");

    // The menu: ticking a type adds it.
    bar->clear();
    settle();
    QMenu* menu = bar->typesButton()->menu();
    Q_EMIT menu->aboutToShow();
    QAction* sphere = nullptr;
    for (QAction* action : menu->actions()) {
        if (action->text().startsWith("Sphere")) sphere = action;
    }
    CHECK(sphere && sphere->isCheckable() && !sphere->isChecked(), "the menu lists the stage's types");
    if (!sphere) return;
    sphere->trigger();
    settle();
    CHECK(sphere->font().bold(), "ticking it makes it bold at once, with the menu still open");
    CHECK(shown(tree) == (std::set<std::string>{"World", "World/Lamp"}), "ticking one shows only it");
    QAction* all = menu->actions().front();
    all->trigger();
    settle();
    CHECK(shown(tree).count("Untyped") == 1 && !bar->isFiltering(), "All Types shows everything again");
    CHECK(!sphere->isChecked() && !sphere->font().bold(), "and unticks the types in the open menu");
    CHECK(all->isChecked() && all->font().bold(), "and is ticked itself");
    all->trigger();
    CHECK(all->isChecked() && !bar->isFiltering(), "clicking it again leaves it ticked");
    sphere->trigger();
    settle();
    CHECK(sphere->isChecked() && !all->isChecked() && shown(tree) == (std::set<std::string>{"World", "World/Lamp"}), "and a type can be ticked again after it");
}

USDCC_TEST(prim_tree_filter_in_show_prims_tree) {
    Fixture f;
    usdcc::usd::PrimTreeWidget widget;
    widget.setStage(f.stage->usdStage());
    widget.show();
    settle();
    widget.filterBar()->setTypeFilter(std::set<std::string>{"Mesh"});
    settle();
    CHECK(widget.isFiltering() && widget.matchCount() == 2, "the dialogs' tree filters by type too: %d", widget.matchCount());
    CHECK(shown(widget.treeView()).count("World/Chair/Leg") == 1, "with its parents");
    widget.clearFilter();
    CHECK(!widget.isFiltering(), "and clears both");
}

// Filtering away the selected prim used to pass the tree's loss of it on to the stage,
// from inside the proxy's update, and crash (a deep prim of a usdz, filtered for Material).
USDCC_TEST(prim_tree_filter_hides_the_selection) {
    Fixture f;
    usdcc::usd::OutlinerViewPanel panel(&f.stages, f.stage, f.undo, "Outliner");
    panel.resize(400, 500);
    panel.show();
    settle();
    auto* tree = panel.findChild<QTreeView*>();
    auto* bar = panel.findChild<PrimTreeFilterBar*>();
    if (!tree || !bar) return;
    auto* proxy = static_cast<PrimTreeFilterProxy*>(tree->model());
    auto* model = proxy->primModel();
    const std::vector<SdfPath> leg{SdfPath("/World/Chair/Leg")};

    f.stages.setSelectedPaths(f.stage, leg);
    settle();
    bar->setTypeFilter(std::set<std::string>{"Material"});
    settle();
    CHECK(shown(tree) == (std::set<std::string>{"Looks", "Looks/Wood"}), "only the material shows");
    CHECK(f.stages.selectedPaths(f.stage) == leg, "the hidden prim stays selected");
    CHECK(tree->selectionModel()->selectedRows().isEmpty(), "and isn't selected in the tree");

    bar->setNameFilter("le");  // a search over the type: still hidden
    settle();
    CHECK(f.stages.selectedPaths(f.stage) == leg, "still selected");

    bar->clear();
    settle();
    const QModelIndex legRow = proxy->mapFromSource(model->fetchPath(leg.front()));
    CHECK(tree->selectionModel()->isSelected(legRow), "shown again, it's selected in the tree again");
    CHECK(f.stages.selectedPaths(f.stage) == leg, "and on the stage");

    // Selecting in the tree while filtering still selects.
    bar->setNameFilter("wood");
    settle();
    tree->selectionModel()->setCurrentIndex(proxy->mapFromSource(model->fetchPath(SdfPath("/Looks/Wood"))),
                                            QItemSelectionModel::ClearAndSelect);
    settle();
    CHECK(f.stages.selectedPaths(f.stage) == std::vector<SdfPath>{SdfPath("/Looks/Wood")}, "a click in the filtered tree selects");
}
