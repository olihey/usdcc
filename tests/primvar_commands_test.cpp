// The Primvars panel's editing commands (docs/PRIMVAR_VIEWER.md §19): the row menu and the options
// menu, each command as one undo step on a stage whose primvars are in a weaker sublayer, the
// report line, a refusal that writes nothing, several prims at once, and a plugin's command.
// Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/primvar_commands.h"
#include "usdcc/usd/primvar_values_view.h"
#include "usdcc/usd/primvars_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QFile>
#include <QMenu>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>

#include <memory>
#include <string>

using namespace usdcc::usd;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

const char* const kBase = R"(#usda 1.0
def Mesh "Grid"
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
    texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (
        interpolation = "faceVarying"
    )
    int[] primvars:st:indices = [0, 1, 4, 3, 1, 2, 5, 4]
    texCoord2f[] primvars:seam = [(0, 0), (0.5, 0), (0.5, 1), (0, 1), (0.6, 0), (1, 0), (1, 1), (0.6, 1)] (
        interpolation = "faceVarying"
    )
    float[] primvars:dirt = [0, 1, 2, 3, 4, 5] (
        interpolation = "vertex"
    )
    color3f[] primvars:displayColor = [(1, 0, 0)] (
        interpolation = "constant"
    )
    string[] primvars:names = ["a", "b", "c"] (
        interpolation = "constant"
    )
}

def Mesh "Other"
{
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 1), (1, 0, 1), (2, 0, 1), (0, 1, 1), (1, 1, 1), (2, 1, 1)]
}
)";

struct Fixture {
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()), dir.filePath(QStringLiteral("ui.ini"))};
    StageManager stages;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    StageRefPtr stage;
    Fixture() {
        QStandardPaths::setTestModeEnabled(true);
        QFile base(dir.filePath(QStringLiteral("base.usda"))), root(dir.filePath(QStringLiteral("root.usda")));
        if (!base.open(QIODevice::WriteOnly) || !root.open(QIODevice::WriteOnly)) return;
        base.write(kBase);
        base.close();
        root.write("#usda 1.0\n(\n    subLayers = [@./base.usda@]\n)\n");
        root.close();
        stage = stages.openStage(root.fileName().toStdString());
        undo->trackSelection(stages);
    }
    ~Fixture() {
        if (stage) stages.closeStage(stage);
    }
    void select(std::vector<SdfPath> paths) {
        stages.setSelectedPaths(stage, std::move(paths));
        settle();
    }
    UsdGeomPrimvar primvar(const std::string& name, const char* prim = "/Grid") const {
        return UsdGeomPrimvarsAPI(stage->usdStage()->GetPrimAtPath(SdfPath(prim))).GetPrimvar(TfToken(name));
    }
    QUndoStack* history() const { return undo->stack(stage); }
};

QTreeWidgetItem* rowNamed(QTreeWidget* list, const QString& name) {
    for (QTreeWidgetItemIterator it(list); *it; ++it) {
        if ((*it)->text(0) == name) return *it;
    }
    return nullptr;
}

QAction* actionNamed(QMenu& menu, const QString& id) {
    for (QAction* action : menu.findChildren<QAction*>()) {
        if (action->objectName() == id) return action;
    }
    return nullptr;
}

// Runs command `id` on `context` (as its menu entry would).
void run(const std::string& id, const PrimvarCommandContext& context) {
    if (const PrimvarCommand* command = PrimvarCommands::instance().find(id)) command->run(context);
}
}  // namespace

USDCC_TEST(primvar_commands_menu) {
    Fixture f;
    CHECK(f.stage, "the stage opens");
    if (!f.stage) return;
    PrimvarsViewPanel panel(&f.stages, f.stage, f.undo);
    f.select({SdfPath("/Grid")});
    QTreeWidgetItem* dirt = rowNamed(panel.list(), "dirt");
    CHECK(dirt, "dirt is listed");
    if (!dirt) return;
    QMenu menu;
    panel.fillRowMenu(&menu, dirt);
    for (const char* id : {"usdcc.rename", "usdcc.duplicate", "usdcc.remove", "usdcc.block", "usdcc.type", "usdcc.interpolation.faceVarying",
                           "usdcc.index", "usdcc.elementSize", "usdcc.create"}) {
        CHECK(actionNamed(menu, id), "the row menu has %s", id);
    }
    QAction* vertex = actionNamed(menu, "usdcc.interpolation.vertex");
    CHECK(vertex && !vertex->isEnabled() && vertex->toolTip().contains("vertex already"), "greyed out where it is already");
    CHECK(!actionNamed(menu, "usdcc.flatten") && !actionNamed(menu, "usdcc.copyToSelected"), "nothing to flatten, no other prim");
    QMenu st;
    panel.fillRowMenu(&st, rowNamed(panel.list(), "st"));
    CHECK(actionNamed(st, "usdcc.flatten") && !actionNamed(st, "usdcc.index"), "an indexed one flattens");
}

USDCC_TEST(primvar_commands_undo_and_report) {
    Fixture f;
    if (!f.stage) return;
    PrimvarsViewPanel panel(&f.stages, f.stage, f.undo);
    f.select({SdfPath("/Grid")});
    PrimvarCommandContext context = panel.commandContext("dirt");
    context.askText = [](const QString&, const QString&, const QString&) { return std::optional<QString>(QStringLiteral("grime")); };
    const int before = f.history()->count();
    run("usdcc.rename", context);
    VtValue value;
    CHECK(f.primvar("grime") && !f.primvar("dirt").Get(&value), "renamed");
    CHECK(f.history()->count() == before + 1 && f.history()->undoText() == "Rename dirt", "one undo step: %s",
          qPrintable(f.history()->undoText()));
    CHECK(panel.lastReport().contains("Rename dirt: done.") && panel.lastReport().contains("dirt is blocked here"), "reported: %s",
          qPrintable(panel.lastReport()));
    f.history()->undo();
    CHECK(f.primvar("dirt").Get(&value) && !f.primvar("grime"), "and undone");

    // Refused: nothing written, no step, the reason said.
    context = panel.commandContext("seam");
    run("usdcc.interpolation.vertex", context);
    CHECK(f.primvar("seam").GetInterpolation() == UsdGeomTokens->faceVarying && f.history()->count() == before + 1,
          "a refusal writes nothing and pushes no step");
    CHECK(panel.lastReport().contains("wasn't done") && panel.lastReport().contains("2 of the 6 points"), "and says why: %s",
          qPrintable(panel.lastReport()));

    context = panel.commandContext("st");
    context.askItem = [](const QString&, const QString&, const QStringList& items) {
        return items.contains("float2[]") ? std::optional<QString>(QStringLiteral("float2[]")) : std::nullopt;
    };
    run("usdcc.type", context);
    CHECK(f.primvar("st").GetTypeName() == SdfValueTypeNames->Float2Array && panel.lastReport().contains("Overrides st from base.usda"),
          "another type, over the base's: %s", qPrintable(panel.lastReport()));
}

USDCC_TEST(primvar_commands_several_prims) {
    Fixture f;
    if (!f.stage) return;
    PrimvarsViewPanel panel(&f.stages, f.stage, f.undo);
    f.select({SdfPath("/Grid"), SdfPath("/Other")});
    QMenu options;
    PrimvarCommandContext context = panel.commandContext({});
    addPrimvarCommands(&options, context);
    CHECK(actionNamed(options, "usdcc.create") && !actionNamed(options, "usdcc.rename"), "the panel's own menu creates only");

    context.askCreate = [](const QStringList& prims) {
        PrimvarCreateRequest request;
        request.name = prims.size() == 2 ? "weight" : "wrong";
        request.type = SdfValueTypeNames->FloatArray;
        request.interpolation = UsdGeomTokens->vertex;
        return std::optional<PrimvarCreateRequest>(request);
    };
    const int before = f.history()->count();
    run("usdcc.create", context);
    CHECK(f.primvar("weight") && f.primvar("weight", "/Other") && f.history()->count() == before + 1, "on both, one step");
    f.history()->undo();
    CHECK(!f.primvar("weight") && !f.primvar("weight", "/Other"), "and both undone");

    context = panel.commandContext("st");
    QMenu row;
    addPrimvarCommands(&row, context);
    CHECK(actionNamed(row, "usdcc.copyToSelected"), "Copy to Other Selected Prims");
    run("usdcc.copyToSelected", context);
    VtVec2fArray grid, other;
    f.primvar("st").ComputeFlattened(&grid);
    f.primvar("st", "/Other").ComputeFlattened(&other);
    CHECK(!grid.empty() && grid == other, "copied: %s", qPrintable(panel.lastReport()));
}

USDCC_TEST(primvar_commands_plugin) {
    Fixture f;
    if (!f.stage) return;
    PrimvarsViewPanel panel(&f.stages, f.stage, f.undo);
    f.select({SdfPath("/Grid")});
    std::string ran;
    PrimvarCommand command;
    command.id = "test.double";
    command.label = QStringLiteral("Tools/Double It");
    command.applies = [](const PrimvarCommandContext& context) { return context.primvar == "dirt"; };
    command.run = [&ran](const PrimvarCommandContext& context) { ran = context.primvar; };
    PrimvarCommands::instance().add(command);
    QMenu dirt, st;
    panel.fillRowMenu(&dirt, rowNamed(panel.list(), "dirt"));
    panel.fillRowMenu(&st, rowNamed(panel.list(), "st"));
    QAction* action = actionNamed(dirt, "test.double");
    CHECK(action && !actionNamed(st, "test.double"), "where it applies, in its submenu");
    if (action) action->trigger();
    CHECK(ran == "dirt", "and it runs");
    CHECK(PrimvarCommands::instance().remove("test.double") && !PrimvarCommands::instance().find("test.double"), "removed");
}

// Editing in the Values table (docs/PRIMVAR_VIEWER.md §20): a cell, text that isn't numbers, the
// Change submenu on picked rows, its components, and undo.
USDCC_TEST(primvar_values_editing) {
    Fixture f;
    if (!f.stage) return;
    PrimvarsViewPanel panel(&f.stages, f.stage, f.undo);
    f.select({SdfPath("/Grid")});
    f.stage->setPrimvarFocus("dirt");
    settle();
    auto* view = qobject_cast<PrimvarValuesView*>(panel.view(QStringLiteral("values")));
    CHECK(view, "the Values view");
    if (!view) return;
    PrimvarValuesModel* model = view->model();
    int points = -1;
    for (int c = 0; c < model->columnCount(); ++c) {
        if (model->columns()[std::size_t(c)].name == "points") points = c;
    }
    CHECK((model->flags(model->index(0, 0)) & Qt::ItemIsEditable) && points > 0 && !(model->flags(model->index(0, points)) & Qt::ItemIsEditable),
          "dirt's cells can be edited, the points' can't");
    CHECK(model->data(model->index(2, 0), Qt::EditRole).toString() == "2.0", "a cell's edit text: %s",
          qPrintable(model->data(model->index(2, 0), Qt::EditRole).toString()));

    const int before = f.history()->count();
    CHECK(model->setData(model->index(2, 0), QStringLiteral("7")), "a cell edited");
    VtFloatArray dirt;
    f.primvar("dirt").ComputeFlattened(&dirt);
    CHECK(dirt.size() == 6 && dirt[2] == 7 && f.history()->count() == before + 1 && f.history()->undoText() == "Edit dirt",
          "written, one step: %s", qPrintable(f.history()->undoText()));
    CHECK(view->lastReport().contains("Edit dirt: done.") && view->lastReport().contains("Overrides dirt from base.usda"), "reported: %s",
          qPrintable(view->lastReport()));
    const auto shown = [&]() {
        settle();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 2000 && model->data(model->index(2, 0), Qt::DisplayRole).toString() != "7.0") QApplication::processEvents(QEventLoop::AllEvents, 10);
        return model->data(model->index(2, 0), Qt::DisplayRole).toString() == "7.0";
    };
    CHECK(shown(), "and shown");
    CHECK(!model->setData(model->index(2, 0), QStringLiteral("seven")) && view->lastReport().contains("isn't numbers"), "words aren't numbers: %s",
          qPrintable(view->lastReport()));

    // Add 10 to rows 0 and 1, through the Change submenu.
    QItemSelectionModel* rows = view->table()->selectionModel();
    rows->select(QItemSelection(model->index(0, 0), model->index(1, 0)), QItemSelectionModel::ClearAndSelect);
    QStringList asked;
    view->setAskChange([&asked](PrimvarValueChange::Kind kind, const QString&, const QStringList& components, bool) {
        asked = components;
        PrimvarValueChange change;
        change.kind = kind;
        change.numbers = {10};
        return std::optional<PrimvarValueChange>(change);
    });
    QMenu menu;
    view->addChangeMenu(&menu, 0);
    QAction* add = actionNamed(menu, "primvarValues.add");
    CHECK(add && add->isEnabled() && actionNamed(menu, "primvarValues.remap"), "Set, Add, Multiply, Clamp, Remap");
    if (add) add->trigger();
    f.primvar("dirt").ComputeFlattened(&dirt);
    CHECK(dirt[0] == 10 && dirt[1] == 11 && dirt[3] == 3 && f.history()->undoText() == "Add to dirt", "added to the picked rows: %s",
          qPrintable(view->lastReport()));
    f.history()->undo();
    f.primvar("dirt").ComputeFlattened(&dirt);
    CHECK(dirt[0] == 0 && dirt[2] == 7, "undone");

    f.stage->setPrimvarFocus("st");
    settle();
    rows->select(model->index(1, 0), QItemSelectionModel::ClearAndSelect);
    QMenu st;
    view->addChangeMenu(&st, 0);
    if (QAction* set = actionNamed(st, "primvarValues.set")) set->trigger();
    CHECK((asked == QStringList{"u", "v"}), "a UV's numbers are u and v: %s", qPrintable(asked.join(",")));
    CHECK(view->lastReport().contains("1 shared value was copied"), "a shared value copied for the row: %s", qPrintable(view->lastReport()));

    // A constant array's cell: its edit text is its values, without the length the cell shows, and
    // goes back in as it is.
    f.stage->setPrimvarFocus("displayColor");
    settle();
    const QString color = model->data(model->index(0, 0), Qt::EditRole).toString();
    CHECK(color == "(1.0, 0.0, 0.0)", "a constant color3f[]'s edit text: %s", qPrintable(color));
    CHECK(model->setData(model->index(0, 0), color) && model->setData(model->index(0, 0), QStringLiteral("(0, 0.5, 1)")),
          "taken back, and another: %s", qPrintable(view->lastReport()));
    VtVec3fArray colors;
    f.primvar("displayColor").Get(&colors);
    CHECK(colors == VtVec3fArray{GfVec3f(0, 0.5f, 1)}, "written");
    f.stage->setPrimvarFocus("names");
    settle();
    const QString names = model->data(model->index(0, 0), Qt::EditRole).toString();
    CHECK(names == "a, b, c", "a constant string[]'s: %s", qPrintable(names));
    CHECK(model->setData(model->index(0, 0), QStringLiteral("x, \"y, z\", w")), "texts: %s", qPrintable(view->lastReport()));
    VtStringArray texts;
    f.primvar("names").Get(&texts);
    CHECK((texts == VtStringArray{"x", "y, z", "w"}), "one each, a quoted comma kept");

}
