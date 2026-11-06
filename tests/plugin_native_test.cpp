// C++ plugins (docs/PLUGIN_SDK.md): one built with usdcc_add_plugin() loads and
// shares the app's classes and singletons, and what can't load says why.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/view_panel.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/viewport_tool_bars.h"

#include <pxr/base/gf/ray.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QUndoStack>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

using usdcc::scripting::AppContextScope;
using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginState;
using usdcc::test::pythonTestApp;
using usdcc::ui::MainWindow;
using usdcc::ui::ViewPanelRegistry;
using usdcc::usd::PrimCreationCatalog;

namespace {
struct Collect : usdcc::core::LogSink {
    std::mutex mutex;
    std::vector<usdcc::core::LogRecord> records;
    void onRecord(const usdcc::core::LogRecord& record) override {
        std::lock_guard lock(mutex);
        records.push_back(record);
    }
    int count(const std::string& source, const std::string& text) {
        std::lock_guard lock(mutex);
        int n = 0;
        for (const auto& record : records) {
            if (record.source == source && record.message.find(text) != std::string::npos) ++n;
        }
        return n;
    }
};

struct Settings {
    Settings() {
        QStandardPaths::setTestModeEnabled(true);
        remove();
    }
    ~Settings() {
        remove();
        QStandardPaths::setTestModeEnabled(false);
    }
    static void remove() {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/usdcc.ini");
    }
};

struct Scratch {
    std::filesystem::path root;
    explicit Scratch(const std::string& name)
        : root(std::filesystem::temp_directory_path() /
               ("usdcc_native_" + name + "_" + std::to_string(QCoreApplication::applicationPid()))) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
    }
    ~Scratch() {
        std::error_code error;  // the loaded DLLs can't be removed; they're in the temp folder
        std::filesystem::remove_all(root, error);
    }
    void write(const std::filesystem::path& name, const std::string& text) const {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary) << text;
    }
    // A manifest for a C++ part, built for this app unless told otherwise.
    void manifest(const std::string& folder, const std::string& id, const std::string& library, const std::string& key = {},
                  const std::string& value = {}, const std::string& extra = {}) const {
        std::string builtFor;
        for (const auto& [name, text] : usdcc::scripting::pluginBuildInfo()) {
            if (!builtFor.empty()) builtFor += ", ";
            builtFor += "\"" + name + "\": \"" + (name == key ? value : text) + "\"";
        }
        write(folder + "/usdcc-plugin.json", "{\"id\": \"" + id + "\", \"name\": \"" + id + "\", \"version\": \"1\", \"api\": 1, " +
                                                 "\"native\": {\"library\": \"" + library + "\", \"built_for\": {" + builtFor + "}}" + extra + "}");
    }
    PluginManager::Options options() const {
        PluginManager::Options options;
        options.folders = {root};
        return options;
    }
};

const std::string platform() { return usdcc::scripting::pluginBuildInfo().at("platform"); }

const std::filesystem::path testPluginFolder() { return std::filesystem::u8path(USDCC_TEST_PLUGINS) / "test_native"; }

QAction* findAction(const QList<QAction*>& actions, const QString& text) {
    for (QAction* action : actions) {
        if (QString(action->text()).remove(QLatin1Char('&')) == text) return action;
    }
    return nullptr;
}

std::string reasonOf(const PluginManager& manager, const std::string& id) {
    const auto* plugin = manager.find(id);
    return plugin ? plugin->reason : std::string();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}  // namespace

USDCC_TEST(native_plugin_shares_the_app) {
    Settings settings;
    auto& app = pythonTestApp();
    Scratch scratch("shares");
    scratch.write("stage.usda", "#usda 1.0\ndef Xform \"A\"\n{\n}\n");
    const usdcc::usd::StageRefPtr stage = app.stages.openStage((scratch.root / "stage.usda").string());
    app.stages.setCurrentStage(stage);

    PluginManager::Options options;
    options.folders = {testPluginFolder().parent_path()};
    PluginManager manager(options);
    manager.scan();

    {
        MainWindow window;
        ViewPanelRegistry registry;
        auto tools = std::make_shared<usdcc::tools::ToolSet>();
        new usdcc::usd::ViewportToolBars(&window, tools, &app.stages);
        const AppContextScope scope({&app.stages, &app.undo, tools.get(), &window, &registry, &manager});

        auto sink = std::make_shared<Collect>();
        usdcc::core::LogHub::instance().addSink(sink, false);
        manager.activate(app.python);

        const auto* plugin = manager.find("test.native");
        CHECK(plugin && plugin->state == PluginState::Active, "it loads, and finds its own DLL: %s", plugin ? plugin->reason.c_str() : "not found");
        CHECK(sink->count("plugin:test.native", "native plugin active") == 1, "its log has the plugin as source");
        CHECK(plugin->contributions.size() == 5, "the Plugin Manager is told what it added: %d", int(plugin->contributions.size()));

        // One copy of the app's singletons: the plugin set this in the app's catalog.
        bool inItsCategory = false;
        for (const auto& category : PrimCreationCatalog::instance().categories()) {
            if (category.name != "From The Native Plugin") continue;
            for (const auto& group : category.groups) {
                for (const auto& item : group) inItsCategory = inItsCategory || item.label == "Cube";
            }
        }
        CHECK(inItsCategory, "a registration reaches the app's own catalog");
        PrimCreationCatalog::instance().reset();

        // A Q_OBJECT panel deriving from the app's ViewPanel, across the DLL boundary.
        usdcc::ui::ViewPanel* panel = registry.open("test.native.panel", stage, window.dockManager(), &window);
        CHECK(panel, "its panel opens");
        ads::CDockWidget* asDockWidget = panel;
        CHECK(qobject_cast<usdcc::ui::ViewPanel*>(asDockWidget) == panel, "and the app's qobject_cast recognizes it");
        CHECK(panel->metaObject()->superClass() == &usdcc::ui::ViewPanel::staticMetaObject, "with the app's own metaobject as its base");
        CHECK(panel->currentStage() == stage, "showing the stage");
        usdcc::ui::ViewPanel* copy = panel->duplicate(&window);
        CHECK(copy && copy->currentStage() == stage, "and it duplicates");
        delete copy;

        // Its menu action is one undo step, and a failing one is logged.
        QMenu* menu = nullptr;
        if (QAction* tools_ = findAction(window.mainMenuBar()->actions(), "Tools")) menu = tools_->menu();
        CHECK(menu, "its menu");
        QUndoStack* stack = app.undo.stack(stage);
        int before = stack->count();
        findAction(menu->actions(), "Native Two Edits")->trigger();
        const PXR_NS::UsdStageRefPtr usd = stage->usdStage();
        CHECK(usd->GetPrimAtPath(PXR_NS::SdfPath("/Native2")), "the action ran");
        CHECK(stack->count() == before + 1 && stack->undoText() == "Native Two Edits", "as one undo step");
        stack->undo();
        CHECK(!usd->GetPrimAtPath(PXR_NS::SdfPath("/Native1")), "undone together");
        findAction(menu->actions(), "Native Fails")->trigger();
        CHECK(sink->count("plugin:test.native", "the native action failed") == 1, "an exception from an action is logged");

        // Its prim menu entry.
        usdcc::usd::PrimMenuContext context;
        context.undo = &app.undo;
        context.stage = stage;
        context.paths = {PXR_NS::SdfPath("/A")};
        context.clicked = PXR_NS::SdfPath("/A");
        QMenu primMenu;
        usdcc::usd::populatePrimMenu(&primMenu, context);
        QAction* mark = findAction(primMenu.actions(), "Native Mark");
        CHECK(mark, "its prim menu entry");
        before = stack->index();  // not count(), which includes what can be redone
        mark->trigger();
        CHECK(usd->GetPrimAtPath(PXR_NS::SdfPath("/A")).HasCustomDataKey(PXR_NS::TfToken("nativeMark")), "it ran on the clicked prim");
        CHECK(stack->index() == before + 1 && stack->undoText() == "Native Mark", "as one undo step");

        // Its tool, driven as the viewport does.
        CHECK(tools->setActive("test.native.tool"), "its tool");
        usdcc::tools::ToolContext ctx;
        ctx.stage = usd;
        const auto press = tools->activeTool().mousePress(ctx, PXR_NS::GfVec2d(0, 0), PXR_NS::GfRay(PXR_NS::GfVec3d(0, 1, 0), PXR_NS::GfVec3d(0, -1, 0)));
        CHECK(press.startsEdit, "a press starts an edit");
        app.undo.beginEdit(stage, QStringLiteral("Drop"));
        tools->activeTool().mouseRelease();
        CHECK(app.undo.endEdit(stage), "which is an undo step");
        CHECK(usd->GetPrimAtPath(PXR_NS::SdfPath("/DroppedByNativeTool")), "and edited the stage");
        tools->setActive(usdcc::tools::kSelectTool);

        // A panel comes back, as for Python's: saved by type.
        registry.savePanels(window.dockManager());
        CHECK(usdcc::ui::appSettings().value(QStringLiteral("ViewPanels/opened")).toList().size() == 1, "its panel is saved for the next start");

        manager.deactivate(app.python);
        usdcc::core::LogHub::instance().removeSink(sink.get());
        CHECK(sink->count("plugin:test.native", "native plugin deactivated") == 1, "deactivate runs");
    }
    app.stages.closeStage(stage);
}

USDCC_TEST(native_plugin_refused_and_failing) {
    Settings settings;
    auto& app = pythonTestApp();
    Scratch scratch("refused");
    // Built for something else: refused without loading anything.
    scratch.manifest("wrong_qt", "wrong.qt", "nothing_there", "qt", "1.2.3");
    scratch.manifest("wrong_usd", "wrong.usd", "nothing_there", "usd", "1999");
    scratch.manifest("wrong_compiler", "wrong.compiler", "nothing_there", "compiler", "Other 1.0");
    scratch.manifest("wrong_platform", "wrong.platform", "nothing_there", "platform", "amiga-68k");
    // A version no usdcc has: the release's tag is the version (docs/RELEASE.md), and the
    // first release was 0.0.1, which this test used to call wrong.
    scratch.manifest("wrong_usdcc", "wrong.usdcc", "nothing_there", "usdcc", "999.0.0");
    scratch.write("no_built_for/usdcc-plugin.json", R"({"id": "no.built", "name": "x", "version": "1", "api": 1, "native": {"library": "x"}})");
    scratch.write("no_library/usdcc-plugin.json", R"({"id": "no.library", "name": "x", "version": "1", "api": 1, "native": {"built_for": {}}})");

    // Right build, but it can't load.
    scratch.manifest("missing", "dll.missing", "not_built");
    scratch.manifest("garbage", "dll.garbage", "garbage");
    scratch.write("garbage/bin/" + platform() + "/garbage.dll", "this isn't a library");
    // A real DLL with no entry point.
    scratch.manifest("no_entry", "dll.noentry", "usdcc_test_plugin_helper");
    std::filesystem::create_directories(scratch.root / "no_entry/bin" / platform());
    std::filesystem::copy_file(testPluginFolder() / "bin" / platform() / "usdcc_test_plugin_helper.dll",
                               scratch.root / "no_entry/bin" / platform() / "usdcc_test_plugin_helper.dll");
    scratch.manifest("dependent", "dll.dependent", "not_built", {}, {}, R"(, "requires": ["dll.missing"])");

    PluginManager manager(scratch.options());
    manager.scan();
    manager.activate(app.python);

    CHECK(contains(reasonOf(manager, "wrong.qt"), "qt 1.2.3"), "a wrong Qt is named: %s", reasonOf(manager, "wrong.qt").c_str());
    CHECK(contains(reasonOf(manager, "wrong.usd"), "usd 1999"), "a wrong USD is named");
    CHECK(contains(reasonOf(manager, "wrong.compiler"), "Other 1.0"), "a wrong compiler is named");
    CHECK(contains(reasonOf(manager, "wrong.platform"), "amiga-68k"), "a wrong platform is named");
    CHECK(contains(reasonOf(manager, "wrong.usdcc"), "999.0.0"), "a wrong usdcc is named");
    for (const char* id : {"wrong.qt", "wrong.usd", "wrong.compiler", "wrong.platform", "wrong.usdcc", "no.built", "no.library"}) {
        CHECK(manager.find(id)->state == PluginState::Refused, "refused without running anything: %s", id);
    }
    CHECK(manager.find("dll.missing")->state == PluginState::Failed && contains(reasonOf(manager, "dll.missing"), "doesn't exist"), "a missing library says so");
    CHECK(manager.find("dll.garbage")->state == PluginState::Failed && contains(reasonOf(manager, "dll.garbage"), "can't be loaded"), "a broken one says so: %s", reasonOf(manager, "dll.garbage").c_str());
    CHECK(contains(reasonOf(manager, "dll.noentry"), "usdcc_plugin_activate"), "one without the entry point says so");
    CHECK(manager.find("dll.dependent")->state == PluginState::Refused, "what depends on a failed one is refused");
}

USDCC_TEST(native_plugin_that_throws) {
    Settings settings;
    auto& app = pythonTestApp();
    qputenv("USDCC_TEST_PLUGIN_THROW", "1");
    PluginManager::Options options;
    options.folders = {testPluginFolder().parent_path()};
    PluginManager manager(options);
    manager.scan();
    manager.activate(app.python);
    qunsetenv("USDCC_TEST_PLUGIN_THROW");
    CHECK(manager.find("test.native")->state == PluginState::Failed, "an exception from activate fails the plugin");
    CHECK(contains(reasonOf(manager, "test.native"), "the plugin chose to fail"), "saying why: %s", reasonOf(manager, "test.native").c_str());
}

// A plugin folder given relative to the working directory, as
// `usdcc --plugin-path build/plugins` does: its library still loads (Windows'
// LoadLibraryEx refuses a relative path with "The parameter is incorrect").
// The throwing mode shows it ran: a library that didn't load can't throw.
USDCC_TEST(native_plugin_from_a_relative_folder) {
    Settings settings;
    auto& app = pythonTestApp();
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(testPluginFolder().parent_path(), std::filesystem::current_path(), error);
    if (error || relative.empty() || relative.is_absolute()) return;  // another drive: no relative path
    qputenv("USDCC_TEST_PLUGIN_THROW", "1");
    PluginManager::Options options;
    options.folders = {relative};
    PluginManager manager(options);
    manager.scan();
    manager.activate(app.python);
    qunsetenv("USDCC_TEST_PLUGIN_THROW");
    CHECK(manager.find("test.native") != nullptr, "found in %s", relative.string().c_str());
    CHECK(contains(reasonOf(manager, "test.native"), "the plugin chose to fail"),
          "its library loaded and ran: %s", reasonOf(manager, "test.native").c_str());
}

USDCC_TEST(native_plugin_build_info) {
    const auto info = usdcc::scripting::pluginBuildInfo();
    CHECK(info.count("platform") && info.count("compiler") && info.count("qt") && info.count("usd") && info.count("usdcc"), "all five");
    for (const auto& [key, value] : info) CHECK(!value.empty(), "and none empty: %s", key.c_str());
}
