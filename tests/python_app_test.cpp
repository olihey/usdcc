// Scripts working on the app (docs/SCRIPTING.md phase 2): usdcc.usd / usdcc.app
// / usdcc.tools on the live StageManager, UndoManager and ToolSet, and
// ScriptRunner's one undo step per stage a run changes — M7's exit criterion
// (a script opens a stage, edits a prim, and drives the undo stack like a UI
// edit) included.
#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_runner.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/relationship_targets.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/toast_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QCoreApplication>
#include <QUndoStack>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using usdcc::test::pythonTestApp;
using usdcc::usd::StageRefPtr;

namespace {
// A small .usda in the temp folder, deleted again when this goes out of scope.
struct TempStageFile {
    std::filesystem::path path;
    explicit TempStageFile(const std::string& name) {
        path = std::filesystem::temp_directory_path() /
               (name + "_" + std::to_string(QCoreApplication::applicationPid()) + ".usda");
        std::ofstream(path) << "#usda 1.0\ndef Xform \"World\" {}\n";
    }
    ~TempStageFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    // With forward slashes, ready to paste into Python source.
    std::string python() const { return path.generic_string(); }
};

StageRefPtr findStage(const std::filesystem::path& file) {
    for (const StageRefPtr& stage : pythonTestApp().stages.stages()) {
        if (std::filesystem::equivalent(stage->usdStage()->GetRootLayer()->GetRealPath(), file)) return stage;
    }
    return nullptr;
}

int undoCount(const StageRefPtr& stage) { return pythonTestApp().undo.stack(stage)->count(); }

std::string undoText(const StageRefPtr& stage) { return pythonTestApp().undo.stack(stage)->undoText().toStdString(); }

bool hasPrim(const StageRefPtr& stage, const char* path) {
    return stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath(path)).IsValid();
}

// Python source for the app's already-open stage on `file` (open_stage()
// would open the file again, as a second stage).
std::string openStageIn(const TempStageFile& file) {
    return "next(s for s in usdcc.usd.stages() if s.GetRootLayer().realPath.replace('\\\\', '/').lower() == r'" +
           file.python() + "'.lower())";
}

std::string evaluate(const std::string& expression) {
    return pythonTestApp().python.evaluate(expression).value_or("<raised>");
}
}  // namespace

USDCC_TEST(python_exit_criterion) {
    auto& app = pythonTestApp();
    TempStageFile file("usdcc_exit_criterion");

    // A script opens a stage and edits a prim.
    const bool ok = app.runner.run(
        "import usdcc.usd\n"
        "stage = usdcc.usd.open_stage(r'" + file.python() + "')\n"
        "cube = stage.DefinePrim('/World/Cube', 'Cube')\n"
        "cube.GetAttribute('size').Set(2.0)\n"
        "usdcc.usd.select([cube], stage)\n",
        "exit_criterion.py");
    CHECK(ok, "the script ran");

    const StageRefPtr stage = findStage(file.path);
    CHECK(stage != nullptr, "the stage is open in the app");
    if (!stage) return;
    CHECK(hasPrim(stage, "/World/Cube"), "the prim exists");
    CHECK(app.stages.selectedPaths(stage) == std::vector<PXR_NS::SdfPath>({PXR_NS::SdfPath("/World/Cube")}),
          "and is selected");
    CHECK(app.runner.lastUndoSteps() == 1 && undoCount(stage) == 1, "one undo step: %d", undoCount(stage));
    CHECK(undoText(stage) == "Run exit_criterion.py", "named after the run: %s", undoText(stage).c_str());

    // A UI-style edit on top, through the same UndoManager the panels use.
    auto size = [&] {
        double value = 0;
        stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Cube")).GetAttribute(PXR_NS::TfToken("size")).Get(&value);
        return value;
    };
    app.undo.apply(stage, QStringLiteral("Set Size"), [&] {
        return stage->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Cube"))
            .GetAttribute(PXR_NS::TfToken("size"))
            .Set(3.0);
    });
    CHECK(undoCount(stage) == 2 && size() == 3.0, "the UI edit is the next step");

    // Driving the stack from Python, step by step, like Edit > Undo/Redo.
    const std::string id = std::to_string(stage->cacheId());
    const std::string stageExpr = "__import__('usdcc.usd').usd._stage(" + id + ")";
    app.python.runString("import usdcc.usd\nusdcc.usd.undo(" + stageExpr + ")\n");
    CHECK(size() == 2.0 && hasPrim(stage, "/World/Cube"), "undo reverts the UI edit only");
    app.python.runString("import usdcc.usd\nusdcc.usd.undo(" + stageExpr + ")\n");
    CHECK(!hasPrim(stage, "/World/Cube"), "the next undo reverts the whole script run");
    CHECK(evaluate("__import__('usdcc.usd').usd.can_undo(" + stageExpr + ")") == "False", "nothing left to undo");
    app.python.runString("import usdcc.usd\nusdcc.usd.redo(" + stageExpr + ")\n");
    CHECK(hasPrim(stage, "/World/Cube") && size() == 2.0, "redo brings the script's edits back");
    app.python.runString("import usdcc.usd\nusdcc.usd.redo(" + stageExpr + ")\n");
    CHECK(size() == 3.0, "and then the UI edit");

    app.stages.closeStage(stage);
}

USDCC_TEST(python_run_spans_stages) {
    auto& app = pythonTestApp();
    TempStageFile fileA("usdcc_spans_a");
    TempStageFile fileB("usdcc_spans_b");
    TempStageFile fileC("usdcc_spans_c");
    const StageRefPtr a = app.stages.openStage(fileA.path.string());
    const StageRefPtr untouched = app.stages.openStage(fileC.path.string());

    const bool ok = app.runner.run(
        "import usdcc.usd\n"
        "a = " + openStageIn(fileA) + "\n"                        // already open
        "b = usdcc.usd.open_stage(r'" + fileB.python() + "')\n"   // opened during the run
        "a.DefinePrim('/World/A')\n"
        "b.DefinePrim('/World/B')\n",
        "two_stages.py");
    const StageRefPtr b = findStage(fileB.path);
    CHECK(ok && b, "the script ran and opened B");
    if (!b) return;
    CHECK(app.runner.lastUndoSteps() == 2, "a step on each changed stage: %d", app.runner.lastUndoSteps());
    CHECK(undoCount(a) == 1 && undoCount(b) == 1 && undoText(b) == "Run two_stages.py", "A and B each got one");
    CHECK(undoCount(untouched) == 0, "an untouched stage got none");
    CHECK(app.stages.stages().size() == 3, "no stage opened twice: %zu", app.stages.stages().size());

    app.undo.stack(b)->undo();
    CHECK(!hasPrim(b, "/World/B") && hasPrim(b, "/World"), "B's step reverts to B as it was opened");

    app.stages.closeStage(a);
    app.stages.closeStage(b);
    app.stages.closeStage(untouched);
}

USDCC_TEST(python_failed_run_is_one_step) {
    auto& app = pythonTestApp();
    TempStageFile file("usdcc_failed_run");
    const StageRefPtr stage = app.stages.openStage(file.path.string());

    const bool ok = app.runner.run(
        "import usdcc.usd\n"
        "stage = " + openStageIn(file) + "\n"
        "stage.DefinePrim('/World/Partial')\n"
        "raise ValueError('stop here')\n",
        "failing.py");
    CHECK(!ok, "the run reports the exception");
    CHECK(hasPrim(stage, "/World/Partial"), "the changes before it stay");
    CHECK(undoCount(stage) == 1 && undoText(stage) == "Run failing.py (failed)", "as one step: %s",
          undoText(stage).c_str());
    app.undo.stack(stage)->undo();
    CHECK(!hasPrim(stage, "/World/Partial"), "which one Undo reverts");

    app.stages.closeStage(stage);
}

USDCC_TEST(python_edit_blocks) {
    auto& app = pythonTestApp();
    TempStageFile file("usdcc_edit_blocks");
    const StageRefPtr stage = app.stages.openStage(file.path.string());
    const std::string open = "import usdcc.usd\nstage = " + openStageIn(file) + "\n";

    // Outside a run (a startup script, a plugin): edit() makes the step.
    app.python.runString(open +
                         "with usdcc.usd.edit('Make sphere', stage) as s:\n"
                         "    s.DefinePrim('/World/Sphere', 'Sphere')\n"
                         "    s.DefinePrim('/World/Sphere/Child')\n");
    CHECK(undoCount(stage) == 1 && undoText(stage) == "Make sphere", "edit() is one named step: %s",
          undoText(stage).c_str());

    // Inside a run, edit() blocks nest into the run's step.
    app.runner.run(open +
                       "with usdcc.usd.edit('First', stage):\n"
                       "    stage.DefinePrim('/World/One')\n"
                       "with usdcc.usd.edit('Second', stage):\n"
                       "    stage.DefinePrim('/World/Two')\n",
                   "nested.py");
    CHECK(undoCount(stage) == 2 && undoText(stage) == "Run nested.py", "nested edits join the run: %s",
          undoText(stage).c_str());

    app.stages.closeStage(stage);
}

USDCC_TEST(python_app_api) {
    auto& app = pythonTestApp();
    TempStageFile file("usdcc_app_api");
    const StageRefPtr stage = app.stages.openStage(file.path.string());
    app.stages.setCurrentStage(stage);

    CHECK(evaluate("__import__('usdcc.app').app.is_embedded()") == "True", "usdcc.app knows it's embedded");
    CHECK(evaluate("__import__('usdcc.usd').usd.EMBEDDED") == "True", "usdcc.usd uses the app's module");
    CHECK(evaluate("__import__('usdcc.usd').usd.current_stage().GetRootLayer().realPath.replace('\\\\', '/')") ==
              file.python(),
          "current_stage() is the app's current stage as a Usd.Stage");
    CHECK(evaluate("len(__import__('usdcc.usd').usd.stages())") == std::to_string(app.stages.stages().size()),
          "stages() lists the app's stages");
    CHECK(evaluate("__import__('usdcc.usd').usd.stage_id(__import__('usdcc.usd').usd.current_stage())") ==
              std::to_string(stage->cacheId()),
          "stage_id() is the app's id");

    app.python.runString("import usdcc.usd\nusdcc.usd.select(['/World'])\n");
    CHECK(app.stages.selectedPaths(stage) == std::vector<PXR_NS::SdfPath>({PXR_NS::SdfPath("/World")}),
          "select() with a path string");
    CHECK(evaluate("[str(p) for p in __import__('usdcc.usd').usd.selected_paths()]") == "['/World']",
          "selected_paths() as Sdf.Paths");
    CHECK(!app.python.runString("import usdcc.usd\nusdcc.usd.select(['not a path'])\n"),
          "a bad path raises ValueError");

    // What the Create menu offers (docs/CREATE_MENU.md): a plugin's entry reaches the
    // same catalog the menu asks.
    auto catalogHas = [](const std::string& category, const std::string& label) {
        for (const auto& c : usdcc::usd::PrimCreationCatalog::instance().categories()) {
            if (c.name != category) continue;
            for (const auto& group : c.groups) {
                for (const auto& item : group) {
                    if (item.label == label) return true;
                }
            }
        }
        return false;
    };
    CHECK(app.python.runString("import usdcc.usd\nusdcc.usd.set_create_entry('Cube', category='Studio', group=1, order=5)\n"),
          "set_create_entry()");
    CHECK(catalogHas("Studio", "Cube") && !catalogHas("Geometry", "Cube"), "the menu's catalog sees it");
    CHECK(evaluate("[e['category'] for e in __import__('usdcc.usd').usd.create_entries() if e['label'] == 'Cube']") == "['Studio']",
          "create_entries()");
    CHECK(evaluate("[e['type'] for e in __import__('usdcc.usd').usd.create_entries() if e['label'] == 'Capsule']") == "['Capsule_1']",
          "create_entries() says what's made");
    CHECK(evaluate("__import__('usdcc.usd').usd.remove_create_entry('Cube')") == "True" && catalogHas("Plugins", "Cube"),
          "remove_create_entry(): to Plugins");
    CHECK(evaluate("__import__('usdcc.usd').usd.remove_create_entry('Cube')") == "False", "there's none now");
    CHECK(app.python.runString("import usdcc.usd\nusdcc.usd.hide_create_entry('Sphere')\n") && !catalogHas("Geometry", "Sphere"),
          "hide_create_entry()");
    CHECK(!app.python.runString("import usdcc.usd\nusdcc.usd.set_create_entry('')\n"), "no type name raises ValueError");
    usdcc::usd::PrimCreationCatalog::instance().reset();

    // What relationships target (docs/RELATIONSHIPS.md): a plugin's rule reaches
    // the same registry the app's dialogs ask.
    CHECK(app.python.runString("import usdcc.usd\nusdcc.usd.set_relationship_targets('myStudio:look', types=['Material'], "
                               "severity='warn', single=True, reason='Usually a Material')\n"),
          "set_relationship_targets()");
    const auto rule = usdcc::usd::RelationshipTargetRules::instance().find("myStudio:look");
    CHECK(rule && rule->types == std::vector<std::string>({"Material"}) && rule->single &&
              rule->severity == usdcc::usd::RelationshipTargetRule::Severity::Warn && rule->reason == "Usually a Material",
          "it reaches C++");
    CHECK(evaluate("__import__('usdcc.usd').usd.relationship_targets('myStudio:look')['severity']") == "warn",
          "relationship_targets()");
    CHECK(evaluate("any(r['relationship'] == 'material:binding' for r in __import__('usdcc.usd').usd.relationship_target_rules())") == "True",
          "relationship_target_rules() lists the built-in ones");
    CHECK(!app.python.runString("import usdcc.usd\nusdcc.usd.set_relationship_targets('x', severity='maybe')\n"),
          "a bad severity raises");
    CHECK(evaluate("__import__('usdcc.usd').usd.remove_relationship_targets('myStudio:look')") == "True" &&
              !usdcc::usd::RelationshipTargetRules::instance().find("myStudio:look"),
          "remove_relationship_targets()");
    CHECK(evaluate("__import__('usdcc.usd').usd.relationship_targets('myStudio:look')") == "None", "no rule: None");

    // The edit target (docs/LAYER_EDITOR.md §3): the session layer, and back.
    int targetNotices = 0;
    auto targetConnection = stage->editTargetChanged.connect([&targetNotices]() { ++targetNotices; });
    app.python.runString(
        "import usdcc.usd\nusdcc.usd.set_edit_target(usdcc.usd.current_stage().GetSessionLayer())\n");
    CHECK(stage->editTargetLayer() == stage->usdStage()->GetSessionLayer() && targetNotices == 1,
          "set_edit_target(): the session layer, and the stage's notice");
    CHECK(evaluate("__import__('usdcc.usd').usd.edit_target() == "
                   "__import__('usdcc.usd').usd.current_stage().GetSessionLayer()") == "True",
          "edit_target()");
    CHECK(!app.python.runString(
              "import usdcc.usd\nfrom pxr import Sdf\nusdcc.usd.set_edit_target(Sdf.Layer.CreateAnonymous())\n"),
          "a layer not in the stack raises ValueError");
    app.python.runString(
        "import usdcc.usd\nusdcc.usd.set_edit_target(usdcc.usd.current_stage().GetRootLayer().identifier)\n");
    CHECK(stage->editTargetLayer() == stage->usdStage()->GetRootLayer(), "by identifier, back to the root layer");

    CHECK(evaluate("__import__('usdcc.tools').tools.tool_names()") == "['select', 'move', 'rotate', 'scale']",
          "tool names");
    app.python.runString("import usdcc.tools\nusdcc.tools.set_active_tool('move')\n");
    CHECK(app.tools.activeKind() == usdcc::tools::ToolKind::Move, "set_active_tool() drives the ToolSet");
    CHECK(evaluate("__import__('usdcc.tools').tools.active_tool()") == "move", "active_tool()");
    CHECK(!app.python.runString("import usdcc.tools\nusdcc.tools.set_active_tool('lasso')\n"),
          "an unknown tool raises");
    app.tools.setActiveKind(usdcc::tools::ToolKind::Select);

    CHECK(evaluate("__import__('usdcc.core').core.set_levels('info')") == "True", "usdcc.core.set_levels()");

    app.stages.closeStage(stage);
    CHECK(evaluate("__import__('usdcc.usd').usd.current_stage()") == "None" || !app.stages.stages().empty(),
          "current_stage() is None with no stage open");
}

// usdcc.usd outside the app: a plain python of the same version, with
// usdcc/_usd.pyd and its own StageManager.
USDCC_TEST(python_standalone_module) {
    auto& app = pythonTestApp();
    const auto& settings = app.python.settings();
    if (settings.paths.size() < 2) return;
    const std::filesystem::path usdRoot = settings.paths[1].parent_path().parent_path();  // <USD>/lib/site-packages
    TempStageFile stageFile("usdcc_standalone");

    const auto dir = std::filesystem::temp_directory_path();
    const auto script = dir / "usdcc_standalone_check.py";
    const auto output = dir / "usdcc_standalone_check.txt";
    {
        std::ofstream out(script);
        out << "import os, sys\n"
            << "root = r'" << usdRoot.generic_string() << "'\n"
            << "os.environ['PATH'] = os.pathsep.join([root, root + '/lib', root + '/bin', os.environ.get('PATH', '')])\n"
            << "sys.path[:0] = [r'" << settings.paths[0].generic_string() << "', r'"
            << settings.paths[1].generic_string() << "']\n"
            << "import usdcc.usd, usdcc.app\n"
            << "manager = usdcc.usd.StageManager()\n"
            << "cache_id = manager.open_stage(r'" << stageFile.python() << "')\n"
            << "try:\n    usdcc.usd.current_stage()\n    needs_app = False\n"
            << "except RuntimeError:\n    needs_app = True\n"
            << "print('OK' if (not usdcc.usd.EMBEDDED and not usdcc.app.is_embedded() and cache_id >= 0 "
            << "and manager.stage_cache_ids() == [cache_id] and needs_app) else 'WRONG')\n";
    }
    const std::string command = "\"\"" + (settings.home / "python.exe").string() + "\" \"" + script.string() +
                                "\" > \"" + output.string() + "\" 2>&1\"";
    const int exitCode = std::system(command.c_str());
    std::ifstream in(output);
    std::stringstream text;
    text << in.rdbuf();
    std::filesystem::remove(script);
    in.close();
    std::filesystem::remove(output);
    CHECK(exitCode == 0 && text.str().find("OK") != std::string::npos,
          "_usd.pyd works in a plain python (exit %d): %s", exitCode, text.str().c_str());
}

// usdcc.settings (docs/SETTINGS.md): the same registry C++ uses, read and
// written from Python, and settings a plugin defines.
USDCC_TEST(python_settings) {
    auto& app = pythonTestApp();
    auto& settings = usdcc::core::Settings::instance();
    settings.setBackend(nullptr);
    settings.define({.key = "Appearance/testAccent", .category = "Appearance", .label = "Accent",
                     .type = usdcc::core::SettingType::Color, .defaultValue = std::string("#3d8bfd")});
    std::vector<std::string> changes;
    auto connection = settings.changed.connect([&changes](const std::string& key) { changes.push_back(key); });

    CHECK(evaluate("__import__('usdcc.settings').settings.get('Appearance/testAccent')") == "#3d8bfd",
          "get() reads a C++ setting");
    CHECK(app.python.runString("import usdcc.settings\nusdcc.settings.set('Appearance/testAccent', '#E5484D')\n"),
          "set() takes a valid value");
    CHECK(settings.get<std::string>("Appearance/testAccent") == "#e5484d" && changes.size() == 1,
          "C++ sees it, and changed() fired");
    CHECK(evaluate("__import__('usdcc.settings').settings.is_default('Appearance/testAccent')") == "False",
          "is_default()");
    CHECK(!app.python.runString("import usdcc.settings\nusdcc.settings.set('Appearance/testAccent', 'red')\n"),
          "an invalid value raises");
    CHECK(app.python.runString("import usdcc.settings\n"
                               "try:\n    usdcc.settings.set('Appearance/testAccent', 5)\n"
                               "except ValueError:\n    pass\nelse:\n    raise AssertionError('no ValueError')\n"),
          "as a ValueError");
    CHECK(app.python.runString("import usdcc.settings\n"
                               "try:\n    usdcc.settings.get('No/such')\n"
                               "except KeyError:\n    pass\nelse:\n    raise AssertionError('no KeyError')\n"),
          "an unknown key raises KeyError");
    app.python.runString("import usdcc.settings\nusdcc.settings.reset('Appearance/testAccent')\n");
    CHECK(settings.isDefault("Appearance/testAccent"), "reset()");

    CHECK(app.python.runString("import usdcc.settings as s\n"
                               "s.define('MyPlugin/level', 'int', 3, label='Level', category='My Plugin',\n"
                               "         group='General', description='How much.', minimum=1, maximum=10)\n"
                               "s.define('MyPlugin/mode', 'choice', 'fast', category='My Plugin',\n"
                               "         choices=['fast', 'slow'])\n"
                               "s.define('MyPlugin/on', 'bool', True, category='My Plugin')\n"
                               "s.define('MyPlugin/ratio', 'float', 0.5, category='My Plugin')\n"
                               "s.define('MyPlugin/key', 'shortcut', 'Ctrl+K', category='My Plugin')\n"),
          "define() adds settings of each type");
    const usdcc::core::SettingDefinition* level = settings.definition("MyPlugin/level");
    CHECK(level && level->type == usdcc::core::SettingType::Int && level->category == "My Plugin" &&
              level->group == "General" && level->label == "Level" && level->minimum == 1 && level->maximum == 10,
          "with everything it was given");
    CHECK(evaluate("__import__('usdcc.settings').settings.keys('My Plugin')") ==
              "['MyPlugin/level', 'MyPlugin/mode', 'MyPlugin/on', 'MyPlugin/ratio', 'MyPlugin/key']",
          "keys(category)");
    CHECK(evaluate("'My Plugin' in __import__('usdcc.settings').settings.categories()") == "True", "categories()");
    CHECK(evaluate("__import__('usdcc.settings').settings.info('MyPlugin/mode')['choices']") == "['fast', 'slow']",
          "info() has the choices");
    CHECK(evaluate("__import__('usdcc.settings').settings.info('MyPlugin/level')['maximum']") == "10.0",
          "and the range");
    CHECK(evaluate("__import__('usdcc.settings').settings.get('MyPlugin/on')") == "True", "a bool reads as a bool");
    CHECK(app.python.runString("import usdcc.settings as s\ns.set('MyPlugin/level', 8)\ns.set('MyPlugin/ratio', 2)\n"),
          "an int, and an int for a float");
    CHECK(settings.get<int>("MyPlugin/level") == 8 && settings.get<double>("MyPlugin/ratio") == 2.0,
          "arrive as their types");
    CHECK(!app.python.runString("import usdcc.settings\nusdcc.settings.set('MyPlugin/level', 11)\n"),
          "out of range raises");
    CHECK(!app.python.runString("import usdcc.settings\nusdcc.settings.set('MyPlugin/mode', 'medium')\n"),
          "not one of the choices raises");
    CHECK(!app.python.runString("import usdcc.settings\nusdcc.settings.define('X/y', 'colour', '#000000')\n"),
          "an unknown type raises");
    CHECK(!app.python.runString("import usdcc.settings\nusdcc.settings.define('X/y', 'int', 50, minimum=0, maximum=10)\n"),
          "a default out of range raises");
    CHECK(!app.python.runString("import usdcc.settings\nusdcc.settings.define('X/y', 'int', 5, minimum=0)\n"),
          "a minimum without a maximum raises");
    settings.setBackend(nullptr);
}

namespace {
struct RecordingSink : usdcc::core::LogSink {
    std::vector<usdcc::core::LogRecord> records;
    void onRecord(const usdcc::core::LogRecord& record) override { records.push_back(record); }
};
}  // namespace

// usdcc.toast and the toast flag of usdcc.core.log / logging
// (docs/NOTIFICATIONS.md): toasts in a real MainWindow, and log records marked
// to show as one.
USDCC_TEST(python_toasts) {
    auto& app = pythonTestApp();
    CHECK(!app.python.runString("import usdcc.toast\nusdcc.toast.show('No window')\n"),
          "usdcc.toast needs a window");

    TestSettings settings;
    usdcc::ui::MainWindow window;
    window.restoreLayout();
    window.resize(1000, 700);
    window.show();
    QCoreApplication::processEvents();
    const usdcc::scripting::AppContextScope scope({&app.stages, &app.undo, &app.tools, &window});
    usdcc::ui::ToastManager& toasts = *window.toasts();

    CHECK(app.python.runString("import usdcc.toast\n"
                               "first = usdcc.toast.show('Exported', 'line 1\\nline 2', kind='success')\n"
                               "second = usdcc.toast.show('Stays', sticky=True, kind='error')\n"
                               "third = usdcc.toast.show('Quick', duration=0.5)\n"
                               "assert usdcc.toast.visible() == [third, second, first], usdcc.toast.visible()\n"),
          "show() of each sort, and visible() lists them newest first");
    const QList<usdcc::ui::ToastWidget*> shown = toasts.visibleToasts();
    CHECK(shown.size() == 3, "three toasts are up (%lld)", static_cast<long long>(shown.size()));
    if (shown.size() == 3) {
        CHECK(shown[2]->kind() == usdcc::core::ToastKind::Success && shown[2]->title() == "Exported" &&
                  shown[2]->body() == "line 1\nline 2",
              "kind, title and multiline body");
        CHECK(shown[1]->isSticky() && shown[0]->durationMs() == 500, "sticky, and a duration in seconds");
    }
    CHECK(!app.python.runString("import usdcc.toast\nusdcc.toast.show('Bad', kind='fatal')\n"),
          "an unknown kind raises");
    CHECK(!app.python.runString("import usdcc.toast\nusdcc.toast.show('Bad', duration=0)\n"),
          "a zero duration raises (sticky=True is how)");
    app.python.runString("import usdcc.toast\nusdcc.toast.dismiss_all()\n");
    CHECK(toasts.visibleToasts().isEmpty(), "dismiss_all()");

    auto sink = std::make_shared<RecordingSink>();
    usdcc::core::LogHub::instance().addSink(sink, false);
    CHECK(app.python.runString("import usdcc.core\n"
                               "usdcc.core.log(usdcc.core.DEBUG, 'my.tool', 'Quiet but shown', toast=True)\n"
                               "usdcc.core.log(usdcc.core.ERROR, 'my.tool', 'Boom', toast='sticky', title='Export',"
                               " kind='warning')\n"
                               "import logging\n"
                               "logging.getLogger('my.tool').warning('From logging', extra={'usdcc_toast': True,"
                               " 'usdcc_toast_title': 'Logged'})\n"),
          "usdcc.core.log(..., toast=...) and logging's extra");
    CHECK(!app.python.runString("import usdcc.core\nusdcc.core.log(usdcc.core.INFO, 'x', 'y', toast='loud')\n"),
          "an unknown toast mode raises");
    usdcc::core::LogHub::instance().removeSink(sink.get());
    std::vector<usdcc::core::LogRecord> marked;
    for (const auto& record : sink->records) {
        if (record.toast) marked.push_back(record);
    }
    CHECK(marked.size() == 3, "three records marked as toasts (%zu)", marked.size());
    if (marked.size() == 3) {
        CHECK(marked[0].level == usdcc::core::LogLevel::Debug && marked[0].toast->durationMs == -1,
              "a disabled level still posts a toast record");
        CHECK(marked[1].toast->durationMs == 0 && marked[1].toast->title == "Export" &&
                  marked[1].toast->kind == usdcc::core::ToastKind::Warning,
              "sticky, its title and kind");
        CHECK(marked[2].message == "From logging" && marked[2].toast->title == "Logged", "logging's extra");
    }
    const bool arrived = [&toasts]() {
        for (int i = 0; i < 200 && toasts.visibleToasts().size() < 3; ++i) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return toasts.visibleToasts().size() == 3;
    }();
    CHECK(arrived, "and the window shows them");
    toasts.dismissAll();
    window.close();
}
