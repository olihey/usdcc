// The Primvars panel's editing commands (docs/PRIMVAR_VIEWER.md §19): the row menu and the options
// menu, each command as one undo step on a stage whose primvars are in a weaker sublayer, the
// report line, a refusal that writes nothing, several prims at once, and a plugin's command.
// Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/primvar_commands.h"
#include "usdcc/usd/primvars_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QStandardPaths>
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
