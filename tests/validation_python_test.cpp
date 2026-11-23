// Validation rules in Python (docs/VALIDATION.md §4, §5): findings, sites and parameters, a rule
// that raises, replacing one, main-thread rules, rules files reloading, and a plugin's rules.
// Runs in usdcc_python_tests.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/validation_rule_files.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/validation_run.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::usd::validation;
using usdcc::test::pythonTestApp;

namespace {
bool python(const std::string& code) { return pythonTestApp().python.runString(code, "<validation test>"); }

std::string evaluated(const std::string& expression) {
    const auto value = pythonTestApp().python.evaluate(expression);
    return value ? *value : "?";
}

std::vector<Finding> check(const UsdStageRefPtr& stage, const std::vector<std::string>& names) {
    ValidationRun run(stage, names);
    run.runAll();
    return run.findings();
}

bool has(const std::string& name) {
    const auto all = rules();
    return std::any_of(all.begin(), all.end(), [&name](const Rule& rule) { return rule.name == name; });
}

Rule ruleNamed(const std::string& name) {
    for (const Rule& rule : rules()) {
        if (rule.name == name) return rule;
    }
    return {};
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Two meshes and an Xform.
UsdStageRefPtr smallStage() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomMesh::Define(stage, SdfPath("/geo_a"));
    UsdGeomMesh::Define(stage, SdfPath("/b"));
    UsdGeomXform::Define(stage, SdfPath("/x"));
    return stage;
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string namedRule(const std::string& id, const std::string& message, const std::string& type = "Mesh") {
    return "import usdcc.validation as v\n@v.rule('" + id + "', 'Label', types=['" + type +
           "'])\ndef f(prim, report):\n    report.warn(prim, '" + message + "')\n";
}

template <typename Condition>
bool waitUntil(Condition condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > 8000) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}
}  // namespace

USDCC_TEST(validation_python_rules) {
    const bool made = python(R"(
import usdcc.validation as v
from pxr import UsdGeom, Sdf

@v.rule("test.names", "Mesh names", types=["Mesh"], keywords=["testkw"])
def names(prim, report, prefix="geo_"):
    """Doc."""
    if not prim.GetName().startswith(prefix):
        report.warn(prim, "should start with " + prefix, name="BadPrefix")

@v.rule("test.sites", "Sites", types=[UsdGeom.Mesh])
def sites(prim, report):
    if prim.GetName() == "b":
        report.error(prim.GetAttribute("points"), "a property")
        report.info(UsdGeom.Mesh(prim), "a schema")
        report.info((prim.GetStage().GetRootLayer(), "/b"), "a pair")

@v.stage_rule("test.stage", "Stage")
def stage_rule(stage, report):
    report.warn(stage, "%d prims" % len(stage.GetPseudoRoot().GetChildren()))

@v.layer_rule("test.layer", "Layer")
def layer_rule(layer, report):
    report.warn(layer, "a layer")

@v.rule("test.times", "Times", time_dependent=True)
def times(prim, report, times):
    if prim.GetName() == "x":
        report.info(prim, "default" if times.IncludesTimeCodeDefault() else "no default")
)");
    CHECK(made, "the rules are made");
    const UsdStageRefPtr stage = smallStage();
    const Rule names = ruleNamed("test.names");
    CHECK(names.label == "Mesh names" && names.doc == "Doc." && names.group() == "test" &&
              names.schemaTypes == std::vector<std::string>{"UsdGeomMesh"},
          "the rule is listed, under its id");
    CHECK(rulesWithKeyword("testkw") == std::vector<std::string>{"test.names"}, "and has its keyword");
    const auto all = rules();
    CHECK(std::none_of(all.begin(), all.end(), [](const Rule& rule) { return rule.name.rfind("usdcc:", 0) == 0; }),
          "the names OpenUSD has aren't listed");

    auto found = check(stage, {"test.names"});
    CHECK(found.size() == 1 && found[0].prim() == SdfPath("/b") && found[0].rule == "test.names" &&
              found[0].name == "BadPrefix" && found[0].severity == Severity::Warning,
          "one finding, on the mesh that breaks it (%zu)", found.size());
    found = check(stage, {"test.sites"});
    CHECK(found.size() == 3 && found[0].sites[0].path == SdfPath("/b.points") && found[0].prim() == SdfPath("/b"),
          "at a property (%zu)", found.size());
    CHECK(found.size() == 3 && std::any_of(found.begin(), found.end(), [](const Finding& f) { return !f.layer().empty() && f.prim() == SdfPath("/b"); }),
          "and in a layer");
    found = check(stage, {"test.stage", "test.layer"});
    CHECK(found.size() >= 2 && std::any_of(found.begin(), found.end(), [](const Finding& f) { return f.message == "3 prims"; }),
          "stage and layer rules (%zu)", found.size());
    found = check(stage, {"test.times"});
    CHECK(found.size() == 1 && found[0].message == "default", "a time-dependent rule gets the times (%zu)", found.size());

    CHECK(evaluated("__import__('usdcc.validation').validation.rules()['test.names'].parameters") == "{'prefix': 'geo_'}",
          "a rule's parameters");
    CHECK(python("import usdcc.validation as v\nv.rules()['test.names'].parameters['prefix'] = 'b'\n"), "can be set");
    found = check(stage, {"test.names"});
    CHECK(found.size() == 1 && found[0].prim() == SdfPath("/geo_a"), "and are used");

    CHECK(python("import usdcc.validation as v\nfor id in list(v.rules()): v.remove_rule(id)\n"), "rules are removed");
    CHECK(!has("test.names") && check(stage, {"test.names"}).empty(), "and gone");

    // What's refused, when the file is loaded rather than on the first run.
    const auto refused = [](const std::string& code) {
        return python("import usdcc.validation as v\ntry:\n    " + code +
                      "\n    raise SystemError('not refused')\nexcept (ValueError, TypeError):\n    pass\n");
    };
    CHECK(refused("v.rule('a.b', 'l', types=['NoSuchSchema'])"), "an unknown schema");
    CHECK(refused("v.rule('a b', 'l')"), "an id with a space");
    CHECK(refused("v.rule('a:b', 'l')"), "or a colon");
    CHECK(refused("v.rule('a.b', 'l')(lambda prim, report, needed: None)"), "an argument nothing gives a value");
    CHECK(refused("v.rule('a.b')(lambda prim, report, types: None)"), "as any other");
    CHECK(!has("a.b"), "and none of them is there");
}

USDCC_TEST(validation_python_failing) {
    CHECK(python(R"(
import usdcc.validation as v
CALLS = []
@v.rule("fail.always", "Always")
def always(prim, report):
    v.CALLS.append(1)
    raise RuntimeError("boom")
v.CALLS = CALLS
@v.rule("fail.one", "One")
def one(prim, report):
    report.info(prim, "before")
    if prim.GetName() == "p7": raise KeyError("just one")
@v.rule("fail.site", "Site")
def site(prim, report):
    if prim.GetName() == "p7": report.warn(42, "not a site")
@v.rule("good.one", "Good", types=["Mesh"])
def good(prim, report): report.warn(prim, "fine")
)"), "the rules are made");
    UsdStageRefPtr stage = smallStage();
    for (int i = 0; i < 2000; ++i) stage->DefinePrim(SdfPath("/p" + std::to_string(i)));
    auto found = check(stage, {"fail.always", "good.one"});
    const auto failures = std::count_if(found.begin(), found.end(), [](const Finding& f) { return f.name == "RuleFailed"; });
    CHECK(failures == 10, "ten failures, then it's left alone (%d)", int(failures));
    CHECK(std::all_of(found.begin(), found.end(), [](const Finding& f) { return f.name != "RuleFailed" || (f.rule == "fail.always" && contains(f.message, "RuntimeError: boom") && !f.prim().IsEmpty()); }),
          "saying which rule, what and where");
    CHECK(std::any_of(found.begin(), found.end(), [](const Finding& f) { return contains(f.message, "isn't run any further"); }),
          "and that it was left alone");
    CHECK(found.size() == size_t(failures) + 2, "the other rule's findings are there (%zu)", found.size());
    const int calls = std::atoi(evaluated("len(__import__('usdcc.validation').validation.CALLS)").c_str());
    CHECK(calls >= 10 && calls < 200, "it isn't called for every prim (%d)", calls);

    // The next run starts afresh.
    found = check(stage, {"fail.always"});
    CHECK(found.size() == 10, "again in the next run (%zu)", found.size());

    found = check(stage, {"fail.one"});
    CHECK(found.size() == 2003 + 1, "what it reported before raising, and the one failure (%zu)", found.size());
    found = check(stage, {"fail.site"});
    CHECK(found.size() == 1 && contains(found[0].message, "TypeError"), "a wrong site is its failure too (%zu)", found.size());
    python("import usdcc.validation as v\nfor id in list(v.rules()): v.remove_rule(id)\n");
}

USDCC_TEST(validation_python_threads) {
    CHECK(python(R"(
import threading
import usdcc.validation as v
v.SEEN = set()
v.MAIN = set()
v.EVERY = []
@v.rule("thread.any", "Any")
def any_thread(prim, report): v.SEEN.add(threading.get_ident())
@v.rule("thread.main", "Main", types=["Mesh", "MaterialBindingAPI"], main_thread=True)
def main(prim, report):
    v.MAIN.add(threading.get_ident()); v.EVERY.append(prim.GetName()); report.info(prim, "seen")
@v.stage_rule("thread.stage", "Main stage", main_thread=True)
def stage(stage, report): v.MAIN.add(threading.get_ident()); report.info(stage, "stage")
v.MAIN_ID = threading.get_ident()
)"), "the rules are made");
    UsdStageRefPtr stage = smallStage();
    for (int i = 0; i < 500; ++i) stage->DefinePrim(SdfPath("/p" + std::to_string(i)));
    stage->DefinePrim(SdfPath("/api")).ApplyAPI(TfToken("MaterialBindingAPI"));
    const auto found = check(stage, {"thread.main", "thread.any", "thread.stage"});
    CHECK(found.size() == 4, "three prims and the stage (%zu)", found.size());
    CHECK(evaluated("sorted(__import__('usdcc.validation').validation.EVERY)") == "['api', 'b', 'geo_a']",
          "only the types it's for, each once: %s", evaluated("sorted(__import__('usdcc.validation').validation.EVERY)").c_str());
    CHECK(evaluated("__import__('usdcc.validation').validation.MAIN == {__import__('usdcc.validation').validation.MAIN_ID}") == "True",
          "main_thread rules run on the main thread");

    // From Python, which holds the GIL: Check Physics runs the engine.
    CHECK(python(R"(
import usdcc.validation as v
@v.rule("thread.physics", "Physics", keywords=["UsdPhysicsValidators"])
def physics(prim, report): report.warn(prim, "from Python")
)"), "a rule Check Physics runs");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "usdcc_validation_gil.usda";
    stage->GetRootLayer()->Export(file.string());
    const auto opened = pythonTestApp().stages.openStage(file.string());
    CHECK(python("import usdcc.physics, usdcc.usd\n"
                 "found = usdcc.physics.check(usdcc.usd.stages()[-1])\n"
                 "assert any(message == 'from Python' for _, message, _ in found), found\n"),
          "a run started from Python ends, with Python rules in it");
    // And OpenUSD's own context.
    CHECK(python("from pxr import Usd, UsdValidation as V\n"
                 "s = Usd.Stage.CreateInMemory(); s.DefinePrim('/a')\n"
                 "m = V.ValidationRegistry().GetOrLoadValidatorByName('usdcc:thread.physics#1')\n"
                 "assert len(V.ValidationContext(validators=[m]).Validate(s)) == 1\n"),
          "nor does OpenUSD's own context deadlock");
    pythonTestApp().stages.closeStage(opened);
    std::error_code error;
    std::filesystem::remove(file, error);
    python("import usdcc.validation as v\nfor id in list(v.rules()): v.remove_rule(id)\n");
}

USDCC_TEST(validation_python_replace) {
    const UsdStageRefPtr stage = smallStage();
    CHECK(python(namedRule("rep.r", "one")), "a rule");
    CHECK(ruleNamed("rep.r").registered == "usdcc:rep.r#1", "under one name");
    CHECK(python(namedRule("rep.r", "two")), "the same again, changed");
    CHECK(ruleNamed("rep.r").registered == "usdcc:rep.r#1" && check(stage, {"rep.r"}).size() == 2 &&
              check(stage, {"rep.r"})[0].message == "two",
          "the same name runs the new code");
    CHECK(python(namedRule("rep.r", "three", "Xform")), "for another schema");
    auto found = check(stage, {"rep.r"});
    CHECK(ruleNamed("rep.r").registered == "usdcc:rep.r#2" && found.size() == 1 && found[0].prim() == SdfPath("/x"),
          "another name, and only the new one's findings (%zu)", found.size());
    CHECK(python(namedRule("rep.r", "four")), "back again");
    found = check(stage, {"rep.r"});
    CHECK(ruleNamed("rep.r").registered == "usdcc:rep.r#1" && found.size() == 2, "the first name is used again (%zu)", found.size());
    python("import usdcc.validation as v\nv.remove_rule('rep.r')\n");
}

USDCC_TEST(validation_python_files) {
    QTemporaryDir dir;
    const std::filesystem::path folder = std::filesystem::path(dir.path().toStdU16String()) / "rules";
    usdcc::scripting::ValidationRuleFiles files(pythonTestApp().python, {folder});
    files.sync();
    CHECK(std::filesystem::is_directory(folder), "the folder is made");
    const UsdStageRefPtr stage = smallStage();

    write(folder / "a.py", namedRule("file.a", "one"));
    write(folder / "_helper.py", "raise SystemError('not a rules file')\n");
    CHECK(waitUntil([]() { return has("file.a"); }), "a new file is loaded by itself");
    CHECK(contains(ruleNamed("file.a").file, "a.py"), "the rule knows its file [%s]", ruleNamed("file.a").file.c_str());
    CHECK(UserRules::instance().problems().empty(), "with no problems");

    write(folder / "a.py", namedRule("file.a", "two"));  // the same length
    CHECK(waitUntil([&]() { const auto found = check(stage, {"file.a"}); return !found.empty() && found[0].message == "two"; }),
          "and again when it's saved");

    write(folder / "a.py", "import usdcc.validation as v\n@v.rule('file.a')\ndef f(prim, report): pass\nraise ValueError('broken')\n");
    CHECK(waitUntil([]() { return UserRules::instance().problems().size() == 1; }), "a broken file is a problem");
    CHECK(!has("file.a"), "and none of its rules is there");
    if (!UserRules::instance().problems().empty()) {
        CHECK(contains(UserRules::instance().problems()[0].message, "ValueError: broken"), "saying what");
    }
    write(folder / "b.py", "this isn't Python\n");
    CHECK(waitUntil([]() { return UserRules::instance().problems().size() == 2; }), "a file that doesn't compile too");
    write(folder / "b.py", namedRule("file.b", "b"));
    write(folder / "a.py", namedRule("file.a2", "renamed"));
    CHECK(waitUntil([]() { return has("file.b") && has("file.a2") && UserRules::instance().problems().empty(); }),
          "fixed: loaded, and no problem any more");

    // Replacing the file, as some editors save. Tried again while Windows refuses: another
    // program (a virus scanner) may have the file open just after it was written.
    write(folder / "b.tmp", namedRule("file.b", "swapped"));
    std::error_code error;
    CHECK(waitUntil([&]() {
              std::filesystem::rename(folder / "b.tmp", folder / "b.py", error);
              return !error;
          }),
          "the file is replaced [%s]", error.message().c_str());
    CHECK(waitUntil([&]() { const auto found = check(stage, {"file.b"}); return !found.empty() && found[0].message == "swapped"; }),
          "a file put in its place");

    CHECK(waitUntil([&]() {
              std::filesystem::remove(folder / "b.py", error);
              return !error;
          }),
          "the file is deleted [%s]", error.message().c_str());
    CHECK(waitUntil([]() { return !has("file.b"); }) && has("file.a2"), "a deleted file's rules go");

    QString why;
    const std::filesystem::path made = files.create(QStringLiteral("My \"First\" Rule"), &why);
    CHECK(made.filename() == "my_first_rule.py" && why.isEmpty() && has("my.my_first_rule"), "New Rule makes one that loads [%s]",
          qPrintable(why));
    CHECK(ruleNamed("my.my_first_rule").label == "My \"First\" Rule", "with its name");
    CHECK(files.create(QStringLiteral("my first rule"), &why).empty() && !why.isEmpty(), "not over another");
    CHECK(files.create(QStringLiteral("!!"), &why).empty(), "nor with no name");
    CHECK(!files.create(QStringLiteral("2 things"), &why).empty() && has("my.2_things"), "a name starting with a digit is fine");
    CHECK(UserRules::instance().problems().empty(), "they all load");

    std::filesystem::remove_all(folder, error);
    files.sync();
    CHECK(UserRules::instance().rules().empty(), "with the folder gone, no rules are left (%zu)", UserRules::instance().rules().size());
}

USDCC_TEST(validation_python_plugin) {
    QTemporaryDir dir;
    const std::filesystem::path root(dir.path().toStdU16String());
    write(root / "plugins/valplug/usdcc-plugin.json",
          R"({"id": "test.valplug", "name": "Val", "version": "1.0", "api": 1, "python": {"module": "valplug_module"}})");
    const auto source = root / "plugins/valplug/python/valplug_module/__init__.py";
    const auto text = [](const std::string& id) {
        return "import usdcc.validation as v\ndef activate(plugin):\n    @v.rule('" + id +
               "', 'Plugin rule')\n    def f(prim, report): report.info(prim, 'plugin')\n";
    };
    write(source, text("plug.one"));
    usdcc::scripting::PluginManager::Options options;
    options.folders = {root / "plugins"};
    options.crashMarker = root / "marker.txt";
    usdcc::scripting::PluginManager plugins(options);
    plugins.scan();
    const usdcc::scripting::AppContextScope context(
        {&pythonTestApp().stages, &pythonTestApp().undo, &pythonTestApp().tools, nullptr, nullptr, &plugins});
    plugins.activate(pythonTestApp().python);
    CHECK(has("plug.one"), "a plugin's rule");
    const auto* info = plugins.find("test.valplug");
    CHECK(info && info->contributions.size() == 1 && info->contributions[0].kind == "Validation rule" &&
              info->contributions[0].name == "Plugin rule",
          "is listed as the plugin's");
    write(source, text("plug.two"));
    CHECK(plugins.reload("test.valplug", pythonTestApp().python).ok, "reloaded");
    CHECK(has("plug.two") && !has("plug.one"), "its old rules go with a reload");
    plugins.deactivate(pythonTestApp().python);
    python("import usdcc.validation as v\nfor id in list(v.rules()): v.remove_rule(id)\n");
}
