// What plugins add to the app (docs/PLUGINS.md phases 2 to 4): menu actions, prim
// menu entries, events, tools, panels that come back, the Plugin Manager, and the
// example plugin. Needs PySide6 for the panels; without it this file isn't built.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/plugin_manager_side_panel.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/tool_rail.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_tool_bars.h"

#include <pxr/base/gf/ray.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <DockManager.h>

#include <QAction>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QUndoStack>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using usdcc::scripting::AppContextScope;
using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginManagerSidePanel;
using usdcc::scripting::PluginState;
using usdcc::test::pythonTestApp;
using usdcc::ui::MainWindow;
using usdcc::ui::ViewPanelRegistry;
using usdcc::usd::StageRefPtr;

namespace {
struct TestSettings {
    TestSettings() {
        QStandardPaths::setTestModeEnabled(true);
        remove();
    }
    ~TestSettings() {
        remove();
        QStandardPaths::setTestModeEnabled(false);
    }
    static void remove() {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/usdcc.ini");
    }
};

struct Collect : usdcc::core::LogSink {
    std::mutex mutex;
    std::vector<usdcc::core::LogRecord> records;
    void onRecord(const usdcc::core::LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    int count(const std::string& text) {
        std::lock_guard lock(mutex);
        int n = 0;
        for (const auto& record : records) n += record.message.find(text) != std::string::npos ? 1 : 0;
        return n;
    }
};

// A stage open in the app, from a scratch file.
struct OpenStage {
    std::filesystem::path file;
    StageRefPtr stage;
    explicit OpenStage(const std::string& name)
        : file(std::filesystem::temp_directory_path() /
               ("usdcc_plugin_" + name + "_" + std::to_string(QCoreApplication::applicationPid()) + ".usda")) {
        std::ofstream(file) << "#usda 1.0\ndef Xform \"A\"\n{\n}\ndef Xform \"B\"\n{\n}\n";
        stage = pythonTestApp().stages.openStage(file.string());
    }
    ~OpenStage() {
        pythonTestApp().stages.closeStage(stage);
        std::error_code error;
        std::filesystem::remove(file, error);
    }
};

bool run(const std::string& code) { return pythonTestApp().python.runString(code); }

std::string evaluated(const std::string& expression) {
    const auto value = pythonTestApp().python.evaluate(expression);
    return value ? *value : "?";
}

QAction* findAction(const QList<QAction*>& actions, const QString& text) {
    for (QAction* action : actions) {
        if (QString(action->text()).remove(QLatin1Char('&')) == text) return action;
    }
    return nullptr;
}

QMenu* findMenu(const QList<QAction*>& actions, const QString& text) {
    QAction* action = findAction(actions, text);
    return action ? action->menu() : nullptr;
}

void flushDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
}  // namespace

USDCC_TEST(plugin_menu_action_is_one_undo_step) {
    TestSettings settings;
    auto& app = pythonTestApp();
    OpenStage open("menu");
    MainWindow window;
    window.mainMenuBar()->addMenu(QStringLiteral("&File"));
    window.mainMenuBar()->addMenu(QStringLiteral("&Debug"));
    const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window});

    CHECK(run(R"(
import usdcc.app, usdcc.usd
def two_edits():
    stage = usdcc.usd.current_stage()
    stage.DefinePrim("/FromMenu1", "Xform")
    stage.DefinePrim("/FromMenu2", "Xform")
usdcc.app.add_menu_action("Tools/Nested/Two Edits", two_edits, shortcut="Ctrl+Shift+F9", tip="Makes two prims")
usdcc.app.add_menu_action("&File/Plugin Save", lambda: None)
def fails():
    usdcc.usd.current_stage().DefinePrim("/BeforeFailing", "Xform")
    raise RuntimeError("the action failed")
usdcc.app.add_menu_action("Tools/Fails", fails)
)"), "the actions are added");

    const QList<QAction*> bar = window.mainMenuBar()->actions();
    QMenu* tools = findMenu(bar, "Tools");
    CHECK(tools, "a new menu is made");
    CHECK(bar.indexOf(tools->menuAction()) == bar.indexOf(findAction(bar, "Debug")) - 1, "just before Debug");
    CHECK(findAction(findMenu(bar, "File")->actions(), "Plugin Save"), "an existing menu is added to, ignoring its mnemonic");
    QMenu* nested = findMenu(tools->actions(), "Nested");
    QAction* twoEdits = nested ? findAction(nested->actions(), "Two Edits") : nullptr;
    CHECK(twoEdits, "with a submenu");
    CHECK(twoEdits->shortcut().toString() == "Ctrl+Shift+F9", "and its shortcut");

    QUndoStack* stack = app.undo.stack(open.stage);
    const int before = stack->count();
    twoEdits->trigger();
    const PXR_NS::UsdStageRefPtr usd = open.stage->usdStage();
    CHECK(usd->GetPrimAtPath(PXR_NS::SdfPath("/FromMenu2")), "it ran");
    CHECK(stack->count() == before + 1, "as one undo step: %d", stack->count() - before);
    CHECK(stack->undoText() == "Two Edits", "named after the action");
    stack->undo();
    CHECK(!usd->GetPrimAtPath(PXR_NS::SdfPath("/FromMenu1")) && !usd->GetPrimAtPath(PXR_NS::SdfPath("/FromMenu2")), "undone together");

    auto sink = std::make_shared<Collect>();
    usdcc::core::LogHub::instance().addSink(sink, false);
    findAction(tools->actions(), "Fails")->trigger();
    usdcc::core::LogHub::instance().removeSink(sink.get());
    CHECK(sink->count("the action failed") == 1, "a failing action is logged");
    CHECK(stack->undoText() == "Fails (failed)", "and its step is marked: %s", qPrintable(stack->undoText()));
    stack->undo();

    CHECK(run("usdcc_replaced = []\nimport usdcc.app\nusdcc.app.add_menu_action('Tools/Fails', lambda: usdcc_replaced.append(1))\n"), "adding again");
    CHECK(tools->actions().size() == 2, "replaces rather than adds");
    CHECK(evaluated("__import__('usdcc.app').app.remove_menu_action('Tools/Fails')") == "True", "removing");
    flushDeletes();
    CHECK(tools->actions().size() == 1, "removes it");
    CHECK(!run("import usdcc.app\nusdcc.app.add_menu_action('NoMenu', print)\n"), "a path with no menu raises");
}

USDCC_TEST(plugin_prim_menu_entries) {
    auto& app = pythonTestApp();
    OpenStage open("prim_menu");
    {
        const AppContextScope scope({&app.stages, &app.undo, &app.tools});
        CHECK(run(R"(
import usdcc.app
def mark(stage, paths, clicked):
    for path in paths:
        stage.GetPrimAtPath(path).SetCustomDataByKey("marked", str(clicked))
usdcc.app.add_prim_menu_action("Marks/Mark", mark)
usdcc.app.add_prim_menu_action("Only On A", mark, applies=lambda stage, paths, clicked: clicked.name == "A")
def broken_applies(stage, paths, clicked):
    raise RuntimeError("applies failed")
usdcc.app.add_prim_menu_action("Never", mark, applies=broken_applies)
)"), "entries are added");

        usdcc::usd::PrimMenuContext context;
        context.undo = &app.undo;
        context.stage = open.stage;
        context.paths = {PXR_NS::SdfPath("/A"), PXR_NS::SdfPath("/B")};
        context.clicked = PXR_NS::SdfPath("/A");

        QMenu menu;
        usdcc::usd::populatePrimMenu(&menu, context);
        QMenu* marks = findMenu(menu.actions(), "Marks");
        QAction* mark = marks ? findAction(marks->actions(), "Mark") : nullptr;
        CHECK(mark, "at the end of the menu, in a submenu");
        CHECK(findAction(menu.actions(), "Only On A"), "applies() true shows it");
        CHECK(!findAction(menu.actions(), "Never"), "a failing applies() hides it");

        QUndoStack* stack = app.undo.stack(open.stage);
        const int before = stack->count();
        mark->trigger();
        const PXR_NS::UsdStageRefPtr usd = open.stage->usdStage();
        CHECK(usd->GetPrimAtPath(PXR_NS::SdfPath("/B")).GetCustomDataByKey(PXR_NS::TfToken("marked")).Get<std::string>() == "/A",
              "it gets the selection and the clicked prim");
        CHECK(stack->count() == before + 1 && stack->undoText() == "Mark", "as one undo step");
        stack->undo();

        context.clicked = PXR_NS::SdfPath("/B");
        QMenu other;
        usdcc::usd::populatePrimMenu(&other, context);
        CHECK(!findAction(other.actions(), "Only On A"), "applies() false leaves it out");
    }
    CHECK(usdcc::usd::PrimMenuEntries::instance().entries().empty(), "entries end with the app context, while Python is there");
}

USDCC_TEST(plugin_events) {
    auto& app = pythonTestApp();
    const AppContextScope scope({&app.stages, &app.undo, &app.tools});
    CHECK(run(R"(
import builtins, usdcc.app
builtins.usdcc_test_events = []
seen = builtins.usdcc_test_events
builtins.usdcc_test_connections = [
    usdcc.app.connect("selection_changed", lambda stage, paths: seen.append(("selection", [str(p) for p in paths]))),
    usdcc.app.connect("stage_opened", lambda stage: seen.append(("opened", stage.GetDefaultPrim().IsValid() or stage is not None))),
    usdcc.app.connect("stage_closed", lambda identifier: seen.append(("closed", identifier.endswith(".usda")))),
    usdcc.app.connect("current_stage_changed", lambda stage: seen.append(("current", stage is not None))),
]
def fails(stage, paths):
    raise RuntimeError("the event callback failed")
builtins.usdcc_test_failing = usdcc.app.connect("selection_changed", fails)
)"), "connections are made");
    CHECK(!run("import usdcc.app\nusdcc.app.connect('nothing_like_this', print)\n"), "an unknown event raises");

    auto sink = std::make_shared<Collect>();
    usdcc::core::LogHub::instance().addSink(sink, false);
    {
        OpenStage open("events");
        CHECK(evaluated("('opened', True) in __import__('builtins').usdcc_test_events") == "True", "stage_opened");
        app.stages.setSelectedPaths(open.stage, {PXR_NS::SdfPath("/A")});
        CHECK(evaluated("('selection', ['/A']) in __import__('builtins').usdcc_test_events") == "True", "selection_changed gets Sdf.Paths");
    }
    usdcc::core::LogHub::instance().removeSink(sink.get());
    CHECK(sink->count("the event callback failed") >= 1, "a failing callback is logged");
    CHECK(evaluated("('closed', True) in __import__('builtins').usdcc_test_events") == "True", "stage_closed gets the identifier");

    CHECK(run("import builtins\nfor c in builtins.usdcc_test_connections: c.disconnect()\nbuiltins.usdcc_test_failing.disconnect()\nbuiltins.usdcc_test_events.clear()\n"), "disconnecting");
    OpenStage again("events2");
    CHECK(evaluated("len(__import__('builtins').usdcc_test_events)") == "0", "ends the calls");
}

USDCC_TEST(plugin_python_tool) {
    auto& app = pythonTestApp();
    OpenStage open("tool");
    MainWindow window;
    auto tools = std::make_shared<usdcc::tools::ToolSet>();
    new usdcc::usd::ViewportToolBars(&window, tools, &app.stages);
    const AppContextScope scope({&app.stages, &app.undo, tools.get(), &window});

    CHECK(run(R"(
import builtins, usdcc.tools
from pxr import Gf
class Lift(usdcc.tools.Tool):
    def __init__(self):
        self.height = 2.0
        self.smooth = True
        self.mode = "up"
        self.pressed = None
    def options(self):
        return [usdcc.tools.number("height", "Height", self.height, 0, 10, 0.5, 1, " m"),
                usdcc.tools.toggle("smooth", "Smooth", self.smooth),
                usdcc.tools.choice("mode", "Mode", self.mode, [("up", "Up"), ("down", "Down")])]
    def set_option(self, id, value):
        setattr(self, id, value)
    def build_gizmo(self, ctx):
        gizmo = usdcc.tools.Gizmo()
        gizmo.add_line((0, 0, 0), Gf.Vec3d(0, self.height, 0), (1, 0, 0))
        gizmo.add_triangle((0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 1, 0, 0.5))
        return gizmo
    def mouse_press(self, ctx, ndc, ray):
        builtins.usdcc_test_press = (str(ctx.selection[0]), ctx.stage.GetPrimAtPath(ctx.selection[0]).IsValid(), ndc[0],
                                     ray.GetPoint(0.0)[2], ray.direction[2], ctx.camera_position[1], ctx.target_op,
                                     str(ctx.pick(ndc)), ctx.view_matrix[3][3])
        return usdcc.tools.ToolResult(starts_edit=True)
    def mouse_move(self, ctx, ndc, ray):
        ctx.stage.GetPrimAtPath(ctx.selection[0]).SetCustomDataByKey("lift", ndc[1])
        return usdcc.tools.ToolResult(new_selection=["/B"])
class Broken(usdcc.tools.Tool):
    def build_gizmo(self, ctx):
        raise RuntimeError("the gizmo failed")
class WrongResult(usdcc.tools.Tool):
    def mouse_press(self, ctx, ndc, ray):
        return 42
usdcc.tools.register_tool("test.lift", "Lift", Lift)
usdcc.tools.register_tool("test.broken", "Broken", Broken)
usdcc.tools.register_tool("test.wrong", "Wrong", WrongResult)
)"), "the tools are registered");

    CHECK(evaluated("__import__('usdcc.tools').tools.tool_names()") ==
              "['select', 'move', 'rotate', 'scale', 'test.lift', 'test.broken', 'test.wrong']",
          "the built-ins first, then these");
    auto* rail = window.findChild<usdcc::usd::ToolRail*>();
    CHECK(rail && findAction(rail->actions(), "Lift"), "the rail has a button for it");
    CHECK(rail->actions().size() == 7, "one button each: %d", int(rail->actions().size()));
    findAction(rail->actions(), "Lift")->trigger();
    CHECK(tools->activeId() == "test.lift", "clicking it activates it");
    CHECK(run("import usdcc.tools\nusdcc.tools.set_active_tool('select')\nusdcc.tools.set_active_tool('test.lift')\n"), "from Python as well");
    CHECK(findAction(rail->actions(), "Lift")->isChecked(), "and the rail follows");

    usdcc::tools::Tool& tool = tools->activeTool();
    CHECK(tool.name() == "Lift", "named by its label");
    auto options = tool.options();
    CHECK(options.size() == 3 && options[0].type == usdcc::tools::ToolOption::Type::Number && options[0].suffix == " m" &&
              options[1].type == usdcc::tools::ToolOption::Type::Toggle &&
              options[2].type == usdcc::tools::ToolOption::Type::Choice && options[2].choices.size() == 2,
          "its options reach the options bar");
    tools->setActiveOption("height", 5.0);
    tools->setActiveOption("smooth", false);
    tools->setActiveOption("mode", std::string("down"));
    options = tool.options();
    CHECK(std::get<double>(options[0].value) == 5.0 && !std::get<bool>(options[1].value) && std::get<std::string>(options[2].value) == "down",
          "and come back set");

    usdcc::tools::ToolContext ctx;
    ctx.stage = open.stage->usdStage();
    ctx.selection = {PXR_NS::SdfPath("/A")};
    ctx.viewMatrix.SetIdentity();
    ctx.projMatrix.SetIdentity();
    ctx.cameraPos = PXR_NS::GfVec3d(0, 9, 0);
    ctx.targetOpName = PXR_NS::TfToken("xformOp:translate");
    ctx.pickPrim = [](const PXR_NS::GfVec2d&) { return PXR_NS::SdfPath("/Picked"); };
    const PXR_NS::GfRay ray(PXR_NS::GfVec3d(0, 0, 7), PXR_NS::GfVec3d(0, 0, -1));

    const usdcc::tools::GizmoGeometry gizmo = tool.buildGizmo(ctx);
    CHECK(gizmo.lines.size() == 1 && gizmo.lines[0].b[1] == 5.0 && gizmo.lines[0].color[3] == 1.0f, "a gizmo line, its colour filled out");
    CHECK(gizmo.triangles.size() == 1 && gizmo.triangles[0].color[3] == 0.5f, "and a triangle");

    const usdcc::tools::ToolResult press = tool.mousePress(ctx, PXR_NS::GfVec2d(0.25, 0.5), ray);
    CHECK(press.handled && press.startsEdit, "a press can start an edit");
    CHECK(evaluated("__import__('builtins').usdcc_test_press") == "('/A', True, 0.25, 7.0, -1.0, 9.0, 'xformOp:translate', '/Picked', 1.0)",
          "with the context as Usd and Gf values: %s", evaluated("__import__('builtins').usdcc_test_press").c_str());

    // As the viewport does: one undo session for the drag.
    app.undo.beginEdit(open.stage, QStringLiteral("Lift"));
    const usdcc::tools::ToolResult move = tool.mouseMove(ctx, PXR_NS::GfVec2d(0.25, 0.75), ray);
    tool.mouseMove(ctx, PXR_NS::GfVec2d(0.25, 0.8), ray);
    tool.mouseRelease();
    CHECK(app.undo.endEdit(open.stage), "the drag's edits are one step");
    CHECK(move.newSelection && move.newSelection->front() == PXR_NS::SdfPath("/B"), "a move can select");
    app.undo.stack(open.stage)->undo();
    CHECK(!ctx.stage->GetPrimAtPath(PXR_NS::SdfPath("/A")).HasCustomDataKey(PXR_NS::TfToken("lift")), "undo takes it back");

    // What a tool made in Python costs per mouse move (docs/PLUGINS.md §3.8).
    QElapsedTimer timer;
    timer.start();
    constexpr int kMoves = 2000;
    std::size_t drawn = 0;
    for (int i = 0; i < kMoves; ++i) drawn += tool.buildGizmo(ctx).lines.size();
    const double gizmoMs = static_cast<double>(timer.nsecsElapsed()) / 1e6 / kMoves;
    std::printf("Python tool: %.3f ms per gizmo, with a line and a triangle\n", gizmoMs);
    CHECK(drawn == kMoves, "it really was asked each time");
    CHECK(gizmoMs < 16.0, "a gizmo fits in a frame at 60 Hz: %.3f ms", gizmoMs);

    auto sink = std::make_shared<Collect>();
    usdcc::core::LogHub::instance().addSink(sink, false);
    CHECK(tools->setActive("test.broken"), "activating the failing tool");
    tools->activeTool().buildGizmo(ctx);
    tools->activeTool().buildGizmo(ctx);
    CHECK(tools->setActive("test.wrong"), "and one returning nonsense");
    const usdcc::tools::ToolResult wrong = tools->activeTool().mousePress(ctx, PXR_NS::GfVec2d(0, 0), ray);
    usdcc::core::LogHub::instance().removeSink(sink.get());
    CHECK(sink->count("the gizmo failed") == 1, "its failure is logged once, not on every frame");
    CHECK(!wrong.handled, "nonsense is handled as nothing");

    CHECK(tools->setActive("test.lift"), "back to it");
    CHECK(!run("import usdcc.tools\nusdcc.tools.register_tool('move', 'Mine', usdcc.tools.Tool)\n"), "a built-in can't be replaced");
    CHECK(!run("import usdcc.tools\nusdcc.tools.register_tool('test.x', 'X', object)\n"), "it has to be a Tool");
    CHECK(evaluated("__import__('usdcc.tools').tools.unregister_tool('test.lift')") == "True", "removing the active tool");
    flushDeletes();
    CHECK(tools->activeId() == usdcc::tools::kSelectTool, "goes back to Select");
    CHECK(!findAction(rail->actions(), "Lift"), "and takes its button");
    CHECK(!tools->removeTool(usdcc::tools::kMoveTool), "a built-in can't be removed");
}

USDCC_TEST(plugin_panels_come_back) {
    TestSettings settings;
    auto& app = pythonTestApp();
    OpenStage open("panels");

    CHECK(run(R"(
import usdcc.ui
from PySide6 import QtWidgets
class BackPanel(usdcc.ui.ViewPanel):
    def create_widget(self):
        return QtWidgets.QLabel("back")
import builtins
builtins.usdcc_test_back_panel = BackPanel
)"), "a panel class");
    const std::string registerIt = "import usdcc.app, builtins\nusdcc.app.register_view_panel('test.back', 'Back', builtins.usdcc_test_back_panel)\n";
    const auto findStage = [&open](const QString&) { return open.stage; };

    {   // A session with the plugin: opens a panel of its type, and quits.
        MainWindow window;
        ViewPanelRegistry registry;
        const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry});
        CHECK(run(registerIt), "registered");
        CHECK(registry.open("test.back", open.stage, window.dockManager(), &window), "and opened");
        registry.savePanels(window.dockManager());
    }
    flushDeletes();
    {   // One without it: nothing to restore, and the entry has to survive quitting.
        MainWindow window;
        ViewPanelRegistry registry;
        const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry});
        registry.restorePanels(window.dockManager(), &window, findStage);
        CHECK(window.findChildren<usdcc::ui::ViewPanel*>().isEmpty(), "with the plugin off its panel isn't made");
        registry.savePanels(window.dockManager());
    }
    flushDeletes();
    {   // The plugin back: activated before restoring, as main() does.
        MainWindow window;
        ViewPanelRegistry registry;
        const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry});
        CHECK(run(registerIt), "registered again");
        registry.restorePanels(window.dockManager(), &window, findStage);
        const auto panels = window.findChildren<usdcc::ui::ViewPanel*>();
        CHECK(panels.size() == 1, "its panel comes back: %d", int(panels.size()));
        CHECK(panels.size() == 1 && panels.front()->currentStage() == open.stage, "showing its stage");

        // Closed: it isn't saved, and isn't listed twice.
        registry.savePanels(window.dockManager());
        const auto saved = usdcc::ui::appSettings().value(QStringLiteral("ViewPanels/opened")).toList();
        CHECK(saved.size() <= 1, "one entry for it: %d", int(saved.size()));
    }
    flushDeletes();
}

USDCC_TEST(plugin_manager_panel) {
    TestSettings settings;
    auto& app = pythonTestApp();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("usdcc_plugin_panel_" + std::to_string(QCoreApplication::applicationPid()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto write = [&root](const std::filesystem::path& name, const std::string& text) {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary) << text;
    };
    write("good/usdcc-plugin.json", R"({"id": "panel.good", "name": "Good One", "version": "2.5", "api": 1,
        "description": "It does good.", "python": {"module": "usdcc_test_panel_good"}})");
    write("good/python/usdcc_test_panel_good/__init__.py", R"(
import usdcc.app, usdcc.ui
class P(usdcc.ui.ViewPanel):
    def create_widget(self):
        from PySide6 import QtWidgets
        return QtWidgets.QLabel()
def activate(plugin):
    usdcc.app.register_view_panel("panel.good.p", "Good Panel", P)
    usdcc.app.add_menu_action("Tools/Good Action", print)
)");
    write("bad/usdcc-plugin.json", R"({"id": "panel.bad", "name": "Bad One", "version": "1", "api": 7})");

    PluginManager::Options options;
    options.folders = {root};
    options.crashMarker = root / "marker.txt";
    PluginManager manager(options);
    manager.scan();

    MainWindow window;
    ViewPanelRegistry registry;
    const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &registry, &manager});
    manager.activate(app.python);

    PluginManagerSidePanel panel(manager);
    auto* tree = panel.findChild<QTreeWidget*>();
    CHECK(tree && tree->topLevelItemCount() == 2, "a row each");
    QTreeWidgetItem* good = nullptr;
    QTreeWidgetItem* bad = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        (item->text(0) == "Good One" ? good : bad) = item;
    }
    CHECK(good && good->text(1) == "2.5" && good->text(2) == "Running", "version and state");
    CHECK(bad && bad->text(2) == "Not loaded", "and one that didn't load");

    const auto details = [&panel]() {
        for (QLabel* label : panel.findChildren<QLabel*>()) {
            if (label->objectName() != "propertyNote") return label;
        }
        return static_cast<QLabel*>(nullptr);
    };
    tree->setCurrentItem(good);
    CHECK(details()->text().contains("It does good.") && details()->text().contains("View panel: Good Panel") &&
              details()->text().contains("Menu action: Tools/Good Action"),
          "details list what it added: %s", qPrintable(details()->text()));
    tree->setCurrentItem(bad);
    CHECK(details()->text().contains("API 7") || details()->text().contains("API"), "and why one isn't running");

    QLabel* pending = panel.findChild<QLabel*>("propertyNote");
    CHECK(pending && pending->isHidden(), "nothing pending at first");
    good->setCheckState(0, Qt::Unchecked);
    CHECK(!manager.isEnabled("panel.good"), "unticking turns it off");
    CHECK(!pending->isHidden(), "for the next start, which it says");
    CHECK(manager.find("panel.good")->state == PluginState::Active, "this session it still runs");
    good->setCheckState(0, Qt::Checked);
    CHECK(manager.isEnabled("panel.good") && pending->isHidden(), "ticking it again cancels that");

    std::filesystem::remove_all(root, error);
}

USDCC_TEST(plugin_manager_panel_shows_types) {
    TestSettings settings;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("usdcc_plugin_types_" + std::to_string(QCoreApplication::applicationPid()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto manifest = [&root](const std::string& id, const std::string& parts) {
        std::filesystem::create_directories(root / id);
        std::ofstream(root / id / "usdcc-plugin.json", std::ios::binary)
            << R"({"id": ")" << id << R"(", "name": ")" << id << R"(", "version": "1", "api": 1)" << parts << "}";
    };
    const std::string python = R"(, "python": {"module": "usdcc_test_types_mod"})";
    const std::string native = R"(, "native": {"library": "x"})";
    const std::string usd = R"(, "usd": ["usd"])";
    manifest("t.python", python);
    manifest("t.cpp", native);
    manifest("t.usd", usd);
    manifest("t.all", native + python + usd);
    manifest("t.mixed", python + usd);
    manifest("t.nothing", "");

    PluginManager::Options options;
    options.folders = {root};
    PluginManager manager(options);
    manager.scan();

    using usdcc::scripting::pluginTypes;
    const auto typesOf = [&manager](const std::string& id) { return pluginTypes(manager.find(id)->manifest); };
    using Types = std::vector<std::string>;
    CHECK(typesOf("t.python") == Types{"Python"}, "Python");
    CHECK(typesOf("t.cpp") == Types{"C++"}, "C++, whether or not it could load");
    CHECK(typesOf("t.usd") == Types{"USD"}, "USD with no code");
    CHECK((typesOf("t.all") == Types{"C++", "Python", "USD"}), "all, in a fixed order");
    CHECK(typesOf("t.nothing").empty(), "nothing");

    PluginManagerSidePanel panel(manager);
    auto* tree = panel.findChild<QTreeWidget*>();
    CHECK(tree && tree->topLevelItemCount() == 6, "a row each");
    if (!tree) return;
    std::map<QString, QTreeWidgetItem*> rows;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) rows[tree->topLevelItem(i)->text(0)] = tree->topLevelItem(i);

    const auto image = [&tree](QTreeWidgetItem* item) { return item->icon(0).pixmap(tree->iconSize()).toImage(); };
    CHECK(!rows["t.python"]->icon(0).isNull() && !rows["t.cpp"]->icon(0).isNull() && !rows["t.all"]->icon(0).isNull(), "each has an icon");
    CHECK(image(rows["t.python"]) != image(rows["t.cpp"]) && image(rows["t.python"]) != image(rows["t.usd"]) &&
              image(rows["t.cpp"]) != image(rows["t.usd"]),
          "telling them apart");
    CHECK(image(rows["t.all"]) != image(rows["t.cpp"]) && image(rows["t.mixed"]) != image(rows["t.python"]), "and a mix from either part");
    CHECK(image(rows["t.python"]).size() == image(rows["t.all"]).size(), "the same width, so the names line up");
    CHECK(image(rows["t.nothing"]).size() == image(rows["t.all"]).size(), "even with nothing to show");
    CHECK(tree->iconSize().width() > tree->iconSize().height(), "wide enough for several");

    const auto details = [&panel]() {
        for (QLabel* label : panel.findChildren<QLabel*>()) {
            if (label->objectName() != "propertyNote") return label->text();
        }
        return QString();
    };
    tree->setCurrentItem(rows["t.mixed"]);
    CHECK(details().contains("Type: Python + USD"), "the details spell it out: %s", qPrintable(details()));
    CHECK(rows["t.mixed"]->toolTip(0).contains("Python + USD") && rows["t.mixed"]->toolTip(0).contains("t.mixed"), "and the tooltip");
    tree->setCurrentItem(rows["t.nothing"]);
    CHECK(!details().contains("Type:"), "no line with nothing to say");
    CHECK(rows["t.nothing"]->toolTip(0) == "t.nothing", "nor in the tooltip");

    std::filesystem::remove_all(root, error);
}

#ifdef USDCC_EXAMPLE_PLUGINS
USDCC_TEST(plugin_example) {
    TestSettings settings;
    auto& app = pythonTestApp();
    OpenStage open("example");
    PluginManager::Options options;
    options.folders = {std::filesystem::u8path(USDCC_EXAMPLE_PLUGINS)};
    PluginManager manager(options);
    manager.scan();

    MainWindow window;
    ViewPanelRegistry registry;
    auto tools = std::make_shared<usdcc::tools::ToolSet>();
    new usdcc::usd::ViewportToolBars(&window, tools, &app.stages);
    const AppContextScope scope({&app.stages, &app.undo, tools.get(), &window, &registry, &manager});
    manager.activate(app.python);

    const std::string id = "org.usdcc.measure-example";
    const auto* plugin = manager.find(id);
    CHECK(plugin && plugin->state == PluginState::Active, "it loads: %s", plugin ? plugin->reason.c_str() : "not found");
    CHECK(plugin->contributions.size() == 4, "the Plugin Manager is told of four things: %d", int(plugin->contributions.size()));

    // Its panel, opened as the Stages menu does.
    CHECK(registry.find(QString::fromStdString(id + ".notes")), "its panel type");
    usdcc::ui::ViewPanel* panel = registry.open(QString::fromStdString(id + ".notes"), open.stage, window.dockManager(), &window);
    CHECK(panel, "opens");

    // Its menu action, which edits as one undo step and shows in the panel.
    app.stages.setCurrentStage(open.stage);
    app.stages.setSelectedPaths(open.stage, {PXR_NS::SdfPath("/A")});
    QMenu* menu = findMenu(window.mainMenuBar()->actions(), "Tools");
    QAction* add = menu ? findAction(menu->actions(), "Add Note to Selection") : nullptr;
    CHECK(add, "its menu action");
    QUndoStack* stack = app.undo.stack(open.stage);
    const int before = stack->count();
    add->trigger();
    const PXR_NS::UsdPrim prim = open.stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/A"));
    CHECK(prim.HasCustomDataKey(PXR_NS::TfToken("usdccNote")), "adds a note");
    CHECK(stack->count() == before + 1, "as one undo step");

    // Its prim menu entry, only where there's a note.
    usdcc::usd::PrimMenuContext context;
    context.undo = &app.undo;
    context.stage = open.stage;
    context.paths = {PXR_NS::SdfPath("/A")};
    context.clicked = PXR_NS::SdfPath("/A");
    QMenu primMenu;
    usdcc::usd::populatePrimMenu(&primMenu, context);
    QAction* clear = findAction(primMenu.actions(), "Clear Note");
    CHECK(clear, "its prim menu entry");
    clear->trigger();
    CHECK(!prim.HasCustomDataKey(PXR_NS::TfToken("usdccNote")), "clears it");

    // Its tool: a drag along the ground draws a line, and edits nothing.
    CHECK(tools->setActive(id + ".measure"), "its tool");
    usdcc::tools::Tool& tool = tools->activeTool();
    usdcc::tools::ToolContext ctx;
    ctx.stage = open.stage->usdStage();
    const PXR_NS::GfVec3d down(0, -1, 0);
    CHECK(tool.mousePress(ctx, PXR_NS::GfVec2d(0, 0), PXR_NS::GfRay(PXR_NS::GfVec3d(0, 5, 0), down)).handled, "starts on the ground");
    tool.mouseMove(ctx, PXR_NS::GfVec2d(0, 0), PXR_NS::GfRay(PXR_NS::GfVec3d(3, 5, 4), down));
    const auto gizmo = tool.buildGizmo(ctx);
    CHECK(gizmo.lines.size() == 1 && (gizmo.lines[0].b - gizmo.lines[0].a).GetLength() == 5.0, "a 3-4-5 line");
    tool.mouseRelease();
    CHECK(tool.buildGizmo(ctx).lines.empty(), "gone at the end of the drag");
    tools->setActive(usdcc::tools::kSelectTool);

    manager.deactivate(app.python);
}
#endif
