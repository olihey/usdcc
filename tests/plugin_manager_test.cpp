// Plugins (docs/PLUGINS.md): manifests, order, what stops one loading, and
// third-party modules. Nothing here needs a window. Each test uses module names
// of its own, as Python keeps what it imported for the whole run.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using usdcc::core::LogHub;
using usdcc::core::LogRecord;
using usdcc::core::LogSink;
using usdcc::scripting::PluginInfo;
using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginState;
using usdcc::scripting::PythonRuntime;
using usdcc::test::pythonTestApp;

namespace {
struct Collect : LogSink {
    std::mutex mutex;
    std::vector<LogRecord> records;
    void onRecord(const LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    int count(const std::string& source, const std::string& text) {
        std::lock_guard lock(mutex);
        int n = 0;
        for (const LogRecord& record : records) {
            if (record.source == source && record.message.find(text) != std::string::npos) ++n;
        }
        return n;
    }
};

// A folder of plugin folders, and usdcc.ini out of the user's way.
struct Plugins {
    std::filesystem::path root;
    explicit Plugins(const std::string& name)
        : root(std::filesystem::temp_directory_path() /
               ("usdcc_plugins_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
        QStandardPaths::setTestModeEnabled(true);
        removeSettings();
    }
    ~Plugins() {
        removeSettings();
        QStandardPaths::setTestModeEnabled(false);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    static void removeSettings() {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/usdcc.ini");
    }
    void write(const std::filesystem::path& name, const std::string& text) const {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary) << text;
    }
    // A Python plugin whose package holds `code`.
    void plugin(const std::string& id, const std::string& module, const std::string& code,
                const std::string& extraJson = {}, const std::string& pythonJson = {}) const {
        write(id + "/usdcc-plugin.json", "{\"id\": \"" + id + "\", \"name\": \"" + id + "\", \"version\": \"1.0\", \"api\": 1, " +
                                             "\"python\": {\"module\": \"" + module + "\"" + pythonJson + "}" + extraJson + "}");
        write(id + "/python/" + module + "/__init__.py", code);
    }
    // A package in the plugin's deps/, as pip install --target leaves it.
    void dependency(const std::string& id, const std::string& package, const std::string& version) const {
        const std::string deps = id + "/deps/";
        write(deps + package + "/__init__.py", "VERSION = '" + version + "'\nFROM = '" + id + "'\n");
        const std::string info = deps + package + "-" + version + ".dist-info/";
        write(info + "METADATA", "Metadata-Version: 2.1\nName: " + package + "\nVersion: " + version + "\n");
        write(info + "RECORD", package + "/__init__.py,,\n");
    }
    PluginManager::Options options() const {
        PluginManager::Options options;
        options.folders = {root};
        options.crashMarker = root / "marker.txt";
        return options;
    }
};

PythonRuntime& python() { return pythonTestApp().python; }

std::string evaluated(const std::string& expression) {
    const auto value = python().evaluate(expression);
    return value ? *value : "?";
}

PluginState stateOf(const PluginManager& manager, const std::string& id) {
    const PluginInfo* plugin = manager.find(id);
    return plugin ? plugin->state : PluginState::Ready;
}

std::string reasonOf(const PluginManager& manager, const std::string& id) {
    const PluginInfo* plugin = manager.find(id);
    return plugin ? plugin->reason : std::string();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}  // namespace

USDCC_TEST(plugin_manifests) {
    Plugins plugins("manifests");
    plugins.write("broken/usdcc-plugin.json", "{ not json");
    plugins.write("noid/usdcc-plugin.json", R"({"name": "No Id", "version": "1", "api": 1})");
    plugins.write("badid/usdcc-plugin.json", R"({"id": "a b", "name": "x", "version": "1", "api": 1})");
    plugins.write("oldapi/usdcc-plugin.json", R"({"id": "old", "name": "Old", "version": "1", "api": 99})");
    plugins.write("native/usdcc-plugin.json", R"({"id": "cpp", "name": "Cpp", "version": "1", "api": 1, "native": {}})");
    plugins.write("noversion/usdcc-plugin.json", R"({"id": "nov", "name": "x", "api": 1})");
    plugins.write("unrelated/readme.txt", "not a plugin");

    PluginManager manager(plugins.options());
    manager.scan();
    manager.activate(python());
    CHECK(manager.plugins().size() == 6, "every folder with a manifest is listed: %d", int(manager.plugins().size()));
    for (const PluginInfo& plugin : manager.plugins()) {
        CHECK(plugin.state == PluginState::Refused && !plugin.reason.empty(), "each is refused, saying why: %s",
              plugin.manifest.id.c_str());
    }
    CHECK(contains(reasonOf(manager, "broken"), "JSON"), "invalid JSON says so");
    CHECK(contains(reasonOf(manager, "old"), "99"), "a wrong API names it");
    CHECK(contains(reasonOf(manager, "cpp"), "native"), "a C++ part with no library says so");

    PluginManager skipped([&] {
        auto options = plugins.options();
        options.enabled = false;
        return options;
    }());
    skipped.scan();
    CHECK(skipped.plugins().empty(), "--no-plugins finds nothing");
}

USDCC_TEST(plugin_retired_python_rtx_without_native) {
    Plugins plugins("retired_rtx");
    plugins.plugin("org.usdcc.rtx-render", "usdcc_test_retired_rtx",
                   "raise RuntimeError('retired RTX must never be imported')\n",
                   {}, ", \"requires\": [\"ovrtx==0.6.0\"]");
    plugins.write("bad-copy/usdcc-plugin.json", R"({"id":"org.usdcc.rtx-render","name":"Bad old copy",
        "version":"0.2.0","api":1,"python":{"module":"usdcc_test_retired_rtx","requires":["https://invalid.example/wheel"]}})");
    PluginManager manager(plugins.options());
    manager.scan();
    manager.activate(python());
    CHECK(!manager.find("org.usdcc.rtx-render"), "an RTX-free build ignores an old Python RTX user copy");
    CHECK(manager.packageRequests(python(), nullptr).empty(), "a retired copy cannot request Python RTX packages");
    CHECK(evaluated("'usdcc_test_retired_rtx' in __import__('sys').modules") == "False", "the retired module is not imported");
}

USDCC_TEST(plugin_order_and_failures) {
    Plugins plugins("order");
    python().runString("import builtins\nbuiltins.usdcc_test_order = []\n");
    const auto records = [](const std::string& name) { return "import builtins\nbuiltins.usdcc_test_order.append('" + name + "')\n"; };
    // By id these would run a, b; "requires" turns that round.
    plugins.plugin("a.first", "usdcc_test_order_a", records("a"), R"(, "requires": ["z.base"])");
    plugins.plugin("z.base", "usdcc_test_order_z", records("z"));
    plugins.plugin("m.missing", "usdcc_test_order_m", records("m"), R"(, "requires": ["no.such"])");
    plugins.plugin("c.one", "usdcc_test_order_c1", records("c1"), R"(, "requires": ["c.two"])");
    plugins.plugin("c.two", "usdcc_test_order_c2", records("c2"), R"(, "requires": ["c.one"])");
    plugins.plugin("f.fails", "usdcc_test_order_f", "def activate(plugin):\n    raise RuntimeError('plugin f failed')\n");
    plugins.plugin("g.after", "usdcc_test_order_g", records("g"), R"(, "requires": ["f.fails"])");
    plugins.plugin("h.late", "usdcc_test_order_h", records("h"));
    plugins.write("usd_only/usdcc-plugin.json", R"({"id": "usd.only", "name": "Usd Only", "version": "1", "api": 1})");

    auto sink = std::make_shared<Collect>();
    LogHub::instance().addSink(sink, false);
    PluginManager manager(plugins.options());
    manager.scan();
    manager.activate(python());
    manager.deactivate(python());
    LogHub::instance().removeSink(sink.get());

    CHECK(evaluated("__import__('builtins').usdcc_test_order") == "['z', 'a', 'h']",
          "dependencies first, past a failing plugin: %s", evaluated("__import__('builtins').usdcc_test_order").c_str());
    CHECK(stateOf(manager, "z.base") == PluginState::Active && stateOf(manager, "a.first") == PluginState::Active, "both run");
    CHECK(stateOf(manager, "usd.only") == PluginState::Active, "a plugin with no code is simply active");
    CHECK(contains(reasonOf(manager, "m.missing"), "no.such"), "a missing dependency is named");
    CHECK(stateOf(manager, "c.one") == PluginState::Refused && stateOf(manager, "c.two") == PluginState::Refused, "a cycle is refused");
    CHECK(stateOf(manager, "f.fails") == PluginState::Failed && contains(reasonOf(manager, "f.fails"), "plugin f failed"), "a failure says why");
    CHECK(stateOf(manager, "g.after") == PluginState::Refused && contains(reasonOf(manager, "g.after"), "f.fails"), "what depends on it is refused");
    CHECK(sink->count("plugin:f.fails", "plugin f failed") == 1, "logged with the plugin as its source");
}

USDCC_TEST(plugin_api_object_and_settings) {
    Plugins plugins("api");
    plugins.plugin("api.plugin", "usdcc_test_api", R"(
import usdcc.app
seen = {}
def activate(plugin):
    seen["current"] = usdcc.app.current_plugin() is plugin
    seen["id"] = plugin.id
    seen["version"] = plugin.version
    seen["default"] = plugin.settings.value("count", 41)
    plugin.settings.set_value("count", {"n": 42})
    seen["kept"] = plugin.settings.value("count")
    plugin.log.warning("hello from the plugin")
def deactivate(plugin):
    seen["deactivated"] = True
)");
    auto sink = std::make_shared<Collect>();
    LogHub::instance().addSink(sink, false);
    PluginManager manager(plugins.options());
    manager.scan();
    {
        const usdcc::scripting::AppContextScope scope(
            {&pythonTestApp().stages, &pythonTestApp().undo, &pythonTestApp().tools, nullptr, nullptr, &manager});
        manager.activate(python());
        manager.deactivate(python());
    }
    LogHub::instance().removeSink(sink.get());

    CHECK(evaluated("__import__('usdcc_test_api').seen['current']") == "True", "current_plugin() during activate()");
    CHECK(evaluated("__import__('usdcc_test_api').seen['id']") == "api.plugin", "the plugin object has its id");
    CHECK(evaluated("__import__('usdcc_test_api').seen['default']") == "41", "a missing setting gives the default");
    CHECK(evaluated("__import__('usdcc_test_api').seen['kept']") == "{'n': 42}", "settings hold JSON values");
    CHECK(evaluated("__import__('usdcc_test_api').seen.get('deactivated')") == "True", "deactivate() runs");
    CHECK(sink->count("plugin:api.plugin", "hello from the plugin") == 1, "plugin.log has the plugin as its source");
    CHECK(evaluated("__import__('usdcc.app').app.current_plugin()") == "None", "and no current plugin afterwards");
}

USDCC_TEST(plugin_clashes) {
    Plugins plugins("clashes");
    plugins.plugin("clash.one", "usdcc_test_clash", "ran = True\n");
    plugins.plugin("clash.two", "usdcc_test_clash", "raise RuntimeError('should not run')\n");
    plugins.plugin("clash.stdlib", "json", "raise RuntimeError('should not run')\n");
    plugins.plugin("clash.app", "usdcc", "raise RuntimeError('should not run')\n");

    // The same id in a second folder: the first folder wins.
    Plugins second("clashes2");
    second.plugin("clash.one", "usdcc_test_clash_other", "raise RuntimeError('should not run')\n");

    auto options = plugins.options();
    options.folders.push_back(second.root);
    PluginManager manager(options);
    manager.scan();
    manager.activate(python());
    CHECK(stateOf(manager, "clash.one") == PluginState::Active, "the first with a module loads");
    CHECK(manager.find("clash.one")->manifest.folder.parent_path() == plugins.root, "the first folder wins an id");
    CHECK(contains(reasonOf(manager, "clash.two"), "usdcc_test_clash"), "a second claiming the module is refused");
    CHECK(stateOf(manager, "clash.stdlib") == PluginState::Refused, "a module named like one Python has is refused");
    CHECK(stateOf(manager, "clash.app") == PluginState::Refused, "and like the app's own");
    CHECK(evaluated("'json' in __import__('sys').modules and hasattr(__import__('json'), 'dumps')") == "True", "json is untouched");
}

USDCC_TEST(plugin_third_party_modules) {
    Plugins plugins("deps");
    // Imports its bundled package, which only deps/ has.
    plugins.plugin("dep.user", "usdcc_test_dep_user", "import usdcc_test_pkg_own\nversion = usdcc_test_pkg_own.VERSION\n", {},
                   R"(, "requires": ["usdcc-test-pkg-own>=1"])");
    plugins.dependency("dep.user", "usdcc_test_pkg_own", "1.5");

    // Two plugins bundling the same package: one copy, the newest both accept.
    plugins.plugin("share.old", "usdcc_test_share_old", "import usdcc_test_pkg_share as p\nfrom_ = p.FROM\n", {},
                   R"(, "requires": ["usdcc-test-pkg-share>=1"])");
    plugins.dependency("share.old", "usdcc_test_pkg_share", "1.0");
    plugins.plugin("share.new", "usdcc_test_share_new", "import usdcc_test_pkg_share as p\nfrom_ = p.FROM\n", {},
                   R"(, "requires": ["usdcc-test-pkg-share>=2"])");
    plugins.dependency("share.new", "usdcc_test_pkg_share", "2.0");

    // No version suits both: the later one is refused, and none of its code runs.
    plugins.plugin("conf.a", "usdcc_test_conf_a", "ran = True\n", {}, R"(, "requires": ["usdcc-test-pkg-conf<2"])");
    plugins.dependency("conf.a", "usdcc_test_pkg_conf", "1.0");
    plugins.plugin("conf.b", "usdcc_test_conf_b", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-pkg-conf>=2"])");
    plugins.dependency("conf.b", "usdcc_test_pkg_conf", "2.0");

    plugins.plugin("miss.ing", "usdcc_test_missing", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-not-anywhere"])");
    plugins.plugin("bad.req", "usdcc_test_badreq", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["not a requirement!!"])");
    plugins.plugin("too.old", "usdcc_test_tooold", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-pkg-old>=5"])");
    plugins.dependency("too.old", "usdcc_test_pkg_old", "1.0");

    // What the app provides can be asked for, but not bundled.
    plugins.plugin("asks.pxr", "usdcc_test_asks", "ran = True\n", {}, R"(, "requires": ["PySide6", "usd-core"])");
    plugins.plugin("bundles.qt", "usdcc_test_bundles", "raise RuntimeError('should not run')\n");
    plugins.write("bundles.qt/deps/PySide6/__init__.py", "");
    plugins.plugin("bundles.pxr", "usdcc_test_bundles_pxr", "raise RuntimeError('should not run')\n");
    plugins.write("bundles.pxr/deps/pxr/__init__.py", "");

    PluginManager manager(plugins.options());
    manager.scan();
    manager.activate(python());

    CHECK(stateOf(manager, "dep.user") == PluginState::Active, "a module from deps/ imports: %s", reasonOf(manager, "dep.user").c_str());
    CHECK(evaluated("__import__('usdcc_test_dep_user').version") == "1.5", "the bundled version");
    CHECK(stateOf(manager, "share.old") == PluginState::Active && stateOf(manager, "share.new") == PluginState::Active, "both sharing load");
    CHECK(evaluated("__import__('usdcc_test_share_old').from_") == "share.new", "the newer copy serves the plugin bundling the older");
    CHECK(evaluated("__import__('usdcc_test_share_new').from_") == "share.new", "and the plugin bundling it");

    CHECK(stateOf(manager, "conf.a") == PluginState::Active, "the first of a conflict loads");
    CHECK(stateOf(manager, "conf.b") == PluginState::Refused, "the later is refused");
    CHECK(contains(reasonOf(manager, "conf.b"), "conf.a") && contains(reasonOf(manager, "conf.b"), "usdcc-test-pkg-conf"),
          "naming the other plugin and the package: %s", reasonOf(manager, "conf.b").c_str());

    CHECK(contains(reasonOf(manager, "miss.ing"), "usdcc-test-not-anywhere"), "a missing package is named");
    CHECK(contains(reasonOf(manager, "bad.req"), "invalid requirement"), "an invalid requirement says so");
    CHECK(contains(reasonOf(manager, "too.old"), "usdcc-test-pkg-old"), "a bundled copy too old for its own plugin");
    CHECK(stateOf(manager, "asks.pxr") == PluginState::Active, "what the app provides meets a requirement: %s", reasonOf(manager, "asks.pxr").c_str());
    CHECK(contains(reasonOf(manager, "bundles.qt"), "PySide6"), "bundling PySide6 is refused");
    CHECK(contains(reasonOf(manager, "bundles.pxr"), "pxr"), "bundling pxr is refused");
    CHECK(evaluated("__import__('pxr').__file__.count('bundles')") == "0", "pxr is still the app's");
}

USDCC_TEST(plugin_shared_package_folder) {
    Plugins plugins("shared");
    // An installed copy is the one used, and can't be replaced by a bundled one.
    plugins.write("packages/usdcc_test_pkg_inst/__init__.py", "VERSION = '3.0'\n");
    plugins.write("packages/usdcc_test_pkg_inst-3.0.dist-info/METADATA",
                  "Metadata-Version: 2.1\nName: usdcc_test_pkg_inst\nVersion: 3.0\n");
    plugins.write("packages/usdcc_test_pkg_inst-3.0.dist-info/RECORD", "usdcc_test_pkg_inst/__init__.py,,\n");
    plugins.plugin("inst.fine", "usdcc_test_inst_fine", "import usdcc_test_pkg_inst as p\nversion = p.VERSION\n", {},
                   R"(, "requires": ["usdcc-test-pkg-inst>=2"])");
    plugins.plugin("inst.old", "usdcc_test_inst_old", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-pkg-inst<2"])");
    plugins.dependency("inst.old", "usdcc_test_pkg_inst", "1.0");
    // Needing it after the one it doesn't suit, which is the one to blame.
    plugins.plugin("inst.zlast", "usdcc_test_inst_zlast", "ran = True\n", {}, R"(, "requires": ["usdcc-test-pkg-inst>=3"])");

    std::filesystem::create_directories(plugins.root / "folders");
    std::filesystem::rename(plugins.root / "inst.fine", plugins.root / "folders" / "inst.fine");
    std::filesystem::rename(plugins.root / "inst.old", plugins.root / "folders" / "inst.old");
    std::filesystem::rename(plugins.root / "inst.zlast", plugins.root / "folders" / "inst.zlast");
    auto options = plugins.options();
    options.folders = {plugins.root / "folders"};
    options.pythonPackageFolders = {plugins.root / "packages"};
    PluginManager manager(options);
    manager.scan();
    manager.activate(python());

    CHECK(stateOf(manager, "inst.fine") == PluginState::Active, "a package from the shared folder: %s", reasonOf(manager, "inst.fine").c_str());
    CHECK(evaluated("__import__('usdcc_test_inst_fine').version") == "3.0", "that copy");
    CHECK(contains(reasonOf(manager, "inst.old"), "3.0"), "and a plugin it doesn't suit is told which version is installed: %s",
          reasonOf(manager, "inst.old").c_str());
    CHECK(stateOf(manager, "inst.old") == PluginState::Refused, "installing can't change a ready-made folder");
    CHECK(stateOf(manager, "inst.zlast") == PluginState::Active, "a plugin it suits isn't blamed for coming last: %s",
          reasonOf(manager, "inst.zlast").c_str());
}

USDCC_TEST(plugin_requirements_in_the_manifest) {
    Plugins plugins("reqs");
    plugins.plugin("two.missing", "usdcc_test_two_missing", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-nowhere-a", "usdcc-test-nowhere-b>=2"])");
    plugins.plugin("elsewhere.only", "usdcc_test_elsewhere", "ran = True\n", {},
                   R"(, "requires": ["usdcc-test-nowhere-c; sys_platform == 'no-such-platform'"])");
    plugins.plugin("names.url", "usdcc_test_names_url", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-url @ https://example.com/a.whl"])");
    plugins.plugin("dep.waits", "usdcc_test_dep_waits", "raise RuntimeError('should not run')\n", R"(, "requires": ["two.missing"])");

    PluginManager manager(plugins.options());
    manager.scan();
    manager.activate(python());

    CHECK(stateOf(manager, "two.missing") == PluginState::NeedsPackages, "a missing package could be installed");
    CHECK(contains(reasonOf(manager, "two.missing"), "usdcc-test-nowhere-a") && contains(reasonOf(manager, "two.missing"), "usdcc-test-nowhere-b"),
          "everything missing is named at once: %s", reasonOf(manager, "two.missing").c_str());
    CHECK(stateOf(manager, "elsewhere.only") == PluginState::Active, "a marker that doesn't apply needs nothing: %s",
          reasonOf(manager, "elsewhere.only").c_str());
    CHECK(stateOf(manager, "names.url") == PluginState::Refused && contains(reasonOf(manager, "names.url"), "URL"),
          "a URL is refused: %s", reasonOf(manager, "names.url").c_str());
    CHECK(stateOf(manager, "dep.waits") == PluginState::Refused, "a plugin requiring one that needs packages doesn't load");
    CHECK(manager.packagesPending(), "an install is waiting");

    std::vector<std::string> blocked;
    const auto requests = manager.packageRequests(python(), &blocked);
    CHECK(requests.size() == 2, "the two with something to ask for: %d", int(requests.size()));
    CHECK(std::find(blocked.begin(), blocked.end(), "pyside6") != blocked.end(), "PySide6 is never installed");
    for (const auto& request : requests) {
        if (request.id == "names.url") CHECK(contains(request.problem, "URL"), "not even tried");
        if (request.id == "two.missing") CHECK(request.requirements.size() == 2, "both go to the installer");
    }
}

// Settings > Plugins > Plugin folders: where defaultOptions() looks. --plugin-path first,
// then the user's folder, the added folders in their order, and the shipped one last.
// Of two plugins with one id, the one in the earlier folder runs.
USDCC_TEST(plugin_folders_setting) {
    Plugins plugins("folders");
    usdcc::scripting::definePluginSettings();
    auto& settings = usdcc::core::Settings::instance();
    const auto* definition = settings.definition(usdcc::scripting::kPluginFoldersKey);
    CHECK(definition && definition->type == usdcc::core::SettingType::FolderList && definition->appliesAtNextStart,
          "a folder list, used from the next start");
    if (!definition) return;
    CHECK(definition->fixedFolders.size() == 2 &&
              std::filesystem::path(std::filesystem::u8path(definition->fixedFolders[0].folder)) == PluginManager::userFolder() &&
              !definition->fixedFolders[0].after &&
              std::filesystem::path(std::filesystem::u8path(definition->fixedFolders[1].folder)) == PluginManager::shippedFolder() &&
              definition->fixedFolders[1].after,
          "the user's folder before the added ones, the shipped one after, fixed");

    const std::filesystem::path first = plugins.root / "first";
    const std::filesystem::path second = plugins.root / "second";
    const auto utf8 = [](const std::filesystem::path& path) {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    };
    settings.setValue(usdcc::scripting::kPluginFoldersKey, utf8(first) + ";" + utf8(second));
    const auto options = PluginManager::defaultOptions({plugins.root / "cli"}, true);
    const auto at = [&options](const std::filesystem::path& folder) {
        return int(std::find(options.folders.begin(), options.folders.end(), folder) - options.folders.begin());
    };
    const int cli = at(plugins.root / "cli"), user = at(PluginManager::userFolder()), a = at(first), b = at(second),
              shipped = at(PluginManager::shippedFolder());
    CHECK(cli == 0 && cli < user && user < a && a < b && b < shipped && shipped == int(options.folders.size()) - 1,
          "--plugin-path, the user's, the added ones in order, then the shipped one: %d %d %d %d %d", cli, user, a, b, shipped);

    // The same id in two added folders: the earlier one's runs.
    const auto write = [](const std::filesystem::path& folder, const std::string& version) {
        std::filesystem::create_directories(folder / "twin");
        std::ofstream(folder / "twin" / "usdcc-plugin.json")
            << R"({"id": "org.usdcc.twin", "name": "Twin", "version": ")" << version << R"(", "api": 1})";
    };
    write(first, "1");
    write(second, "2");
    PluginManager::Options scan = plugins.options();
    scan.folders = {first, second};
    PluginManager manager(scan);
    manager.scan();
    const PluginInfo* twin = manager.find("org.usdcc.twin");
    CHECK(twin && twin->manifest.version == "1", "the first folder's twin wins: %s", twin ? twin->manifest.version.c_str() : "none");
    settings.reset(usdcc::scripting::kPluginFoldersKey);
}

// python.indexes: extra package indexes a manifest names for its requirements, such as
// NVIDIA's for ovrtx. Only plain https URLs, and only with requirements.
USDCC_TEST(plugin_package_indexes) {
    Plugins plugins("indexes");
    plugins.plugin("index.good", "usdcc_test_index_good", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-idx-a"], "indexes": ["https://pypi.example.com/simple"])");
    plugins.plugin("index.http", "usdcc_test_index_http", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-idx-b"], "indexes": ["http://pypi.example.com/simple"])");
    plugins.plugin("index.file", "usdcc_test_index_file", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-idx-c"], "indexes": ["file:///C:/wheels"])");
    plugins.plugin("index.user", "usdcc_test_index_user", "raise RuntimeError('should not run')\n", {},
                   R"(, "requires": ["usdcc-test-idx-d"], "indexes": ["https://me:secret@pypi.example.com/simple"])");
    plugins.plugin("index.alone", "usdcc_test_index_alone", "raise RuntimeError('should not run')\n", {},
                   R"(, "indexes": ["https://pypi.example.com/simple"])");

    PluginManager manager(plugins.options());
    manager.scan();
    manager.activate(python());

    CHECK(stateOf(manager, "index.good") == PluginState::NeedsPackages, "an https index is fine: %s",
          reasonOf(manager, "index.good").c_str());
    CHECK(stateOf(manager, "index.http") == PluginState::Refused && contains(reasonOf(manager, "index.http"), "https"),
          "plain http is refused: %s", reasonOf(manager, "index.http").c_str());
    CHECK(stateOf(manager, "index.file") == PluginState::Refused && contains(reasonOf(manager, "index.file"), "https"),
          "so is a file URL: %s", reasonOf(manager, "index.file").c_str());
    CHECK(stateOf(manager, "index.user") == PluginState::Refused && contains(reasonOf(manager, "index.user"), "password"),
          "and one with a password in it: %s", reasonOf(manager, "index.user").c_str());
    CHECK(stateOf(manager, "index.alone") == PluginState::Refused && contains(reasonOf(manager, "index.alone"), "requires"),
          "an index with nothing to install from it is refused: %s", reasonOf(manager, "index.alone").c_str());

    std::vector<std::string> blocked;
    const auto requests = manager.packageRequests(python(), &blocked);
    CHECK(requests.size() == 1 && requests[0].id == "index.good", "only the good one asks to install: %d", int(requests.size()));
    if (!requests.empty()) {
        CHECK(requests[0].indexes == std::vector<std::string>{"https://pypi.example.com/simple"}, "with its index");
    }
}

USDCC_TEST(plugin_usd_folder) {
    Plugins plugins("usd");
    plugins.write("schemas/usdcc-plugin.json",
                  R"({"id": "usd.schemas", "name": "Schemas", "version": "1", "api": 1, "usd": ["usd", "nothing_here"]})");
    plugins.write("schemas/usd/plugInfo.json", R"({"Plugins": [{"Name": "usdccTestPluginSchemas", "Type": "resource",
        "Root": ".", "ResourcePath": ".", "Info": {}}]})");
    plugins.write("off/usdcc-plugin.json", R"({"id": "usd.off", "name": "Off", "version": "1", "api": 1, "usd": ["usd"]})");
    plugins.write("off/usd/plugInfo.json", R"({"Plugins": [{"Name": "usdccTestPluginOff", "Type": "resource",
        "Root": ".", "ResourcePath": ".", "Info": {}}]})");

    PluginManager first(plugins.options());
    first.setEnabled("usd.off", false);
    first.scan();
    CHECK(PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccTestPluginSchemas"), "a USD folder registers during scan()");
    CHECK(!PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccTestPluginOff"), "not a disabled plugin's");
    first.activate(python());
    CHECK(stateOf(first, "usd.schemas") == PluginState::Active, "and it needs no Python");
}

USDCC_TEST(plugin_enabling_and_crash_marker) {
    Plugins plugins("enable");
    python().runString("import builtins\nbuiltins.usdcc_test_enable = []\n");
    plugins.plugin("en.one", "usdcc_test_en_one", "import builtins\nbuiltins.usdcc_test_enable.append('one')\n");
    plugins.plugin("en.dep", "usdcc_test_en_dep", "raise RuntimeError('should not run')\n", R"(, "requires": ["en.two"])");
    plugins.plugin("en.two", "usdcc_test_en_two", "raise RuntimeError('should not run')\n");

    PluginManager manager(plugins.options());
    manager.setEnabled("en.two", false);
    manager.scan();
    manager.activate(python());
    CHECK(stateOf(manager, "en.two") == PluginState::Disabled, "off stays off, across a new manager");
    CHECK(contains(reasonOf(manager, "en.dep"), "turned off"), "what needs it says why");
    CHECK(!manager.hasPendingChanges(), "nothing differs from what's running");
    manager.setEnabled("en.one", false);
    CHECK(manager.hasPendingChanges(), "turning one off is a change for the next start");
    CHECK(stateOf(manager, "en.one") == PluginState::Active, "and it keeps running this time");
    manager.setEnabled("en.one", true);
    CHECK(!manager.hasPendingChanges(), "turning it back on cancels that");
    CHECK(!std::filesystem::exists(plugins.root / "marker.txt"), "no marker is left after activating");
    manager.setEnabled("en.two", true);

    // A start that ended while activating en.one.
    plugins.write("marker.txt", "en.one");
    PluginManager afterCrash(plugins.options());
    afterCrash.scan();
    CHECK(afterCrash.crashedLastStart() == "en.one", "the marker names the plugin");
    CHECK(stateOf(afterCrash, "en.one") == PluginState::Disabled && contains(reasonOf(afterCrash, "en.one"), "crashed"), "it's off, saying why");
    CHECK(!std::filesystem::exists(plugins.root / "marker.txt"), "the marker has been dealt with");

    PluginManager nextStart(plugins.options());
    nextStart.scan();
    CHECK(nextStart.crashedLastStart().empty(), "only the next start reports it");
    CHECK(stateOf(nextStart, "en.one") == PluginState::Disabled && contains(reasonOf(nextStart, "en.one"), "crashed"), "but it stays off");
    nextStart.setEnabled("en.one", true);

    PluginManager reenabled(plugins.options());
    reenabled.scan();
    CHECK(stateOf(reenabled, "en.one") == PluginState::Ready && reasonOf(reenabled, "en.one").empty(), "until it's turned on again");
}
