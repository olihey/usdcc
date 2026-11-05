// Reloading a Python plugin (docs/PLUGIN_DEV.md): files are rewritten between
// reloads, and everything it registered has to follow. Needs PySide6 for the panels.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/scripted_view_panel.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <DockManager.h>

#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>

#include <filesystem>
#include <fstream>
#include <string>

using usdcc::scripting::AppContextScope;
using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginState;
using usdcc::test::pythonTestApp;
using usdcc::ui::MainWindow;
using usdcc::ui::ViewPanelRegistry;

namespace {
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void flushDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

std::string evaluated(const std::string& expression) {
    const auto value = pythonTestApp().python.evaluate(expression);
    return value ? *value : "?";
}

bool hasAction(QWidget* root, const QString& text) {
    for (QMenu* menu : root->findChildren<QMenu*>()) {
        for (QAction* action : menu->actions()) {
            if (QString(action->text()).remove(QLatin1Char('&')) == text) return true;
        }
    }
    return false;
}

// A window, a stage, and a folder of plugins, with usdcc.ini out of the way.
struct Bench {
    std::filesystem::path root;
    std::filesystem::path stageFile;
    usdcc::usd::StageRefPtr stage;
    ViewPanelRegistry panels;
    explicit Bench(const std::string& name)
        : root(std::filesystem::temp_directory_path() / ("usdcc_reload_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "plugins");
        QStandardPaths::setTestModeEnabled(true);
        removeSettings();
        stageFile = root / "stage.usda";
        std::ofstream(stageFile) << "#usda 1.0\ndef Xform \"A\"\n{\n}\ndef Xform \"B\"\n{\n}\n";
        stage = pythonTestApp().stages.openStage(stageFile.string());
    }
    ~Bench() {
        pythonTestApp().stages.closeStage(stage);
        stage = nullptr;
        removeSettings();
        QStandardPaths::setTestModeEnabled(false);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    static void removeSettings() { QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/usdcc.ini"); }
    void write(const std::string& name, const std::string& text) const {
        const std::filesystem::path file = root / "plugins" / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    void manifest(const std::string& id, const std::string& module, const std::string& extra = {}, const std::string& python = {}) const {
        write(id + "/usdcc-plugin.json", "{\"id\": \"" + id + "\", \"name\": \"" + id + "\", \"version\": \"1.0\", \"api\": 1, \"python\": {\"module\": \"" + module + "\"" + python + "}" + extra + "}");
    }
    PluginManager::Options options() const {
        PluginManager::Options options;
        options.folders = {root / "plugins"};
        options.crashMarker = root / "marker.txt";
        return options;
    }
};

// One of everything, with `version` showing in each.
std::string everything(int version) {
    const std::string v = std::to_string(version);
    return "import usdcc.app, usdcc.tools, usdcc.ui, usdcc.usd\n"
           "from PySide6 import QtWidgets\n"
           "from . import helper\n"
           "VERSION = " + v + "\n"
           "EVENTS = []\n"
           "DEACTIVATED = []\n"
           "class T(usdcc.tools.Tool):\n    pass\n"
           "class P(usdcc.ui.ViewPanel):\n"
           "    def create_widget(self):\n"
           "        usdcc.app.connect('selection_changed', on_panel_selection)\n"
           "        return QtWidgets.QLabel('panel " + v + " ' + helper.VALUE)\n"
           "class S(usdcc.ui.SidePanel):\n    pass\n"
           "def on_selection(stage, paths):\n    EVENTS.append('activate')\n"
           "def on_panel_selection(stage, paths):\n    EVENTS.append('panel')\n"
           "def action():\n    pass\n"
           "def prim(stage, paths, clicked):\n    pass\n"
           "def activate(plugin):\n"
           "    usdcc.tools.register_tool('reload.tool', 'Reload Tool', T)\n"
           "    usdcc.app.register_view_panel('reload.panel', 'Reload Panel', P)\n"
           "    usdcc.app.add_menu_action('Tools/Reload Action " + v + "', action)\n"
           "    usdcc.app.add_menu_action('Reload Menu/Inner/Entry " + v + "', action)\n"
           "    usdcc.app.add_prim_menu_action('Reload Prim', prim, id='reload.prim')\n"
           "    usdcc.app.connect('selection_changed', on_selection)\n"
           "    usdcc.app.add_side_panel(S('Reload Side'), 'right', id='reload.side')\n"
           "def deactivate(plugin):\n"
           "    import sys\n"
           "    sys.modules['builtins'].usdcc_reload_deactivated = VERSION\n";
}
}  // namespace

USDCC_TEST(plugin_reload_takes_everything_with_it) {
    Bench bench("all");
    auto& app = pythonTestApp();
    MainWindow window;
    window.mainMenuBar()->addMenu(QStringLiteral("&Tools"));
    window.mainMenuBar()->addMenu(QStringLiteral("&Debug"));
    bench.manifest("r.all", "usdcc_reload_all");
    bench.write("r.all/python/usdcc_reload_all/__init__.py", everything(1));
    bench.write("r.all/python/usdcc_reload_all/helper.py", "VALUE = 'helper 1'\n");

    PluginManager::Options options = bench.options();
    PluginManager manager(options);
    const AppContextScope scope({&app.stages, &app.undo, &app.tools, &window, &bench.panels, &manager});
    manager.scan();
    manager.activate(app.python);
    CHECK(manager.find("r.all")->state == PluginState::Active, "it starts: %s", manager.find("r.all")->reason.c_str());
    CHECK(hasAction(&window, "Reload Action 1"), "with its menu action");

    auto* panel = bench.panels.open(QStringLiteral("reload.panel"), bench.stage, window.dockManager(), &window);
    CHECK(panel != nullptr, "and a panel opens");
    if (!panel) return;
    const auto panelText = [panel]() {
        QLabel* label = panel->findChild<QLabel*>();
        return label ? label->text().toStdString() : std::string();
    };
    CHECK(panelText() == "panel 1 helper 1", "showing version 1: %s", panelText().c_str());
    panel->setAsCurrentTab();
    app.tools.setActive("reload.tool");

    // A same-length edit straight after the last: the .pyc can't be told apart by its
    // time and size.
    bench.write("r.all/python/usdcc_reload_all/__init__.py", everything(2));
    bench.write("r.all/python/usdcc_reload_all/helper.py", "VALUE = 'helper 2'\n");
    const auto result = manager.reload("r.all", app.python);
    flushDeletes();

    CHECK(result.ok, "it reloads: %s", manager.find("r.all")->reason.c_str());
    CHECK(evaluated("__import__('usdcc_reload_all').VERSION") == "2", "the new code runs");
    CHECK(evaluated("__import__('usdcc_reload_all').helper.VALUE") == "helper 2", "submodules too");
    CHECK(!hasAction(&window, "Reload Action 1") && hasAction(&window, "Reload Action 2"), "the old menu action is gone, the new one there");
    CHECK(!hasAction(&window, "Entry 1") && hasAction(&window, "Entry 2"), "and in menus it made");
    CHECK(app.tools.activeId() == "reload.tool", "the tool stays active: %s", app.tools.activeId().c_str());
    CHECK(panelText() == "panel 2 helper 2", "the open panel was rebuilt: %s", panelText().c_str());
    CHECK(!panel->isClosed(), "and is still there");
    CHECK(evaluated("__import__('builtins').usdcc_reload_deactivated") == "1", "the old code's deactivate() ran");
    int sides = 0;
    for (QObject* child : window.children()) {
        if (child->objectName() == "reload.side") ++sides;
    }
    CHECK(sides == 1, "one side panel, not two: %d", sides);

    // Both connections, from activate() and from the panel, moved to the new code.
    app.python.runString("import usdcc_reload_all as m; m.EVENTS.clear()");
    app.python.runString("import usdcc.usd; usdcc.usd.select(['/A'], usdcc.usd.stages()[-1])");
    CHECK(evaluated("sorted(__import__('usdcc_reload_all').EVENTS)") == "['activate', 'panel']", "the new code hears each event once: %s",
          evaluated("sorted(__import__('usdcc_reload_all').EVENTS)").c_str());

    // Broken: it fails, saying why, with nothing left behind. Then fixed.
    bench.write("r.all/python/usdcc_reload_all/__init__.py", "def broken(:\n");
    const auto broken = manager.reload("r.all", app.python);
    flushDeletes();
    CHECK(!broken.ok && manager.find("r.all")->state == PluginState::Failed, "a syntax error fails");
    CHECK(contains(manager.find("r.all")->reason, "SyntaxError"), "and says so: %s", manager.find("r.all")->reason.c_str());
    CHECK(!hasAction(&window, "Reload Action 2"), "its menu action is gone");
    CHECK(app.tools.activeId() != "reload.tool", "so is its tool");
    CHECK(contains(panelText(), "Nothing provides"), "the panel says so: %s", panelText().c_str());

    bench.write("r.all/python/usdcc_reload_all/__init__.py",
                "import usdcc.app\ndef activate(plugin):\n    usdcc.app.add_menu_action('Tools/Half Way', print)\n    raise RuntimeError('half way')\n");
    const auto half = manager.reload("r.all", app.python);
    CHECK(!half.ok && contains(manager.find("r.all")->reason, "half way"), "activate() raising fails");
    CHECK(hasAction(&window, "Half Way"), "keeping what it registered so far");

    bench.write("r.all/python/usdcc_reload_all/__init__.py", everything(3));
    const auto fixed = manager.reload("r.all", app.python);
    flushDeletes();
    CHECK(fixed.ok, "fixing it brings it back: %s", manager.find("r.all")->reason.c_str());
    CHECK(!hasAction(&window, "Half Way"), "without what the failed one left");
    CHECK(panelText() == "panel 3 helper 2", "and the panel: %s", panelText().c_str());

    // Stops registering something.
    bench.write("r.all/python/usdcc_reload_all/__init__.py", "def activate(plugin):\n    pass\n");
    manager.reload("r.all", app.python);
    flushDeletes();
    CHECK(!bench.panels.find(QStringLiteral("reload.panel")), "the panel type is gone");
    CHECK(!app.tools.tool("reload.tool"), "the tool");
    CHECK(!hasAction(&window, "Entry 3"), "and the menu it made");
    bool menuLeft = false;
    for (QAction* action : window.mainMenuBar()->actions()) menuLeft = menuLeft || action->text() == "Reload Menu";
    CHECK(!menuLeft, "which leaves the menu bar");
    panel->closeDockWidget();
    manager.deactivate(app.python);
}

USDCC_TEST(plugin_reload_manifest_and_dependents) {
    Bench bench("deps");
    auto& app = pythonTestApp();
    bench.manifest("r.base", "usdcc_reload_base");
    bench.write("r.base/python/usdcc_reload_base/__init__.py", "NAME = 'base 1'\n");
    bench.manifest("r.user", "usdcc_reload_user", R"(, "requires": ["r.base"])");
    bench.write("r.user/python/usdcc_reload_user/__init__.py", "import usdcc_reload_base as b\nSEEN = b.NAME\n");
    bench.manifest("r.other", "usdcc_reload_other");
    bench.write("r.other/python/usdcc_reload_other/__init__.py", "COUNT = 0\ndef activate(plugin):\n    global COUNT\n    COUNT += 1\n");

    PluginManager manager(bench.options());
    manager.scan();
    manager.activate(app.python);
    CHECK(evaluated("__import__('usdcc_reload_user').SEEN") == "base 1", "first");

    bench.write("r.base/python/usdcc_reload_base/__init__.py", "NAME = 'base 2'\n");
    const auto result = manager.reload("r.base", app.python);
    CHECK(result.ok && result.names.size() == 2, "it goes with what requires it: %d", int(result.names.size()));
    CHECK(evaluated("__import__('usdcc_reload_user').SEEN") == "base 2", "which sees the new one");
    CHECK(evaluated("__import__('usdcc_reload_other').COUNT") == "1", "others aren't touched");

    // A plain plugin has no deactivate(), and that's worth a word.
    bool noted = false;
    for (const std::string& note : result.notes) noted = noted || contains(note, "deactivate");
    CHECK(noted, "it says it has no deactivate()");

    // The manifest is read again.
    bench.manifest("r.base", "usdcc_reload_base", {}, R"(, "requires": ["usdcc-reload-nowhere"])");
    const auto needs = manager.reload("r.base", app.python);
    CHECK(!needs.ok && manager.find("r.base")->state == PluginState::NeedsPackages, "a new requirement is checked");
    CHECK(manager.find("r.user")->state == PluginState::Refused, "and what requires it waits");
    bench.manifest("r.base", "usdcc_reload_base");
    CHECK(manager.reload("r.base", app.python).ok, "and back");
    CHECK(manager.find("r.user")->state == PluginState::Active, "with what requires it");

    bench.write("r.base/usdcc-plugin.json", "{ not json");
    CHECK(!manager.reload("r.base", app.python).ok && contains(manager.find("r.base")->reason, "JSON"), "a broken manifest says so");
    bench.manifest("r.base", "usdcc_reload_base");
    CHECK(manager.reload("r.base", app.python).ok, "and can be fixed");

    bench.manifest("r.base", "usdcc_reload_base", R"(, "usd": ["usd"])");
    const auto usd = manager.reload("r.base", app.python);
    noted = false;
    for (const std::string& note : usd.notes) noted = noted || contains(note, "next start");
    CHECK(noted, "a change to its USD part waits for the next start");

    // What can't be reloaded.
    manager.setEnabled("r.other", false);
    bench.write("native/usdcc-plugin.json", "{}");
    CHECK(manager.whyNotReloadable("r.base").empty(), "a running Python plugin can");
    CHECK(!manager.whyNotReloadable("no.such").empty(), "an unknown one can't");
    manager.setEnabled("r.other", true);
    manager.deactivate(app.python);
}
