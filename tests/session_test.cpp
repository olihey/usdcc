// Sessions (docs/SESSIONS.md): saving the app and loading it back. Stages with their
// selection, edit target, muted layers, unloaded payloads and session layer; the view
// panels, their place and what they show; the Script Editor's tabs; plugins that
// differ; files that moved; unsaved edits; and the Python API.
#include "python_test_app.h"
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_editor_side_panel.h"
#include "usdcc/scripting/session_controller.h"
#include "usdcc/scripting/sessions_dialog.h"
#include "usdcc/scripting/code_editor.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/view_panel_registry.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/usd/outliner_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/usda_view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <DockManager.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QListWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeView>

#include <memory>

using usdcc::scripting::SessionController;
using usdcc::test::pythonTestApp;
using usdcc::usd::StageRefPtr;

namespace {
void settle(int ms = 50) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QCoreApplication::processEvents();
}

void write(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    (void)file.open(QIODevice::WriteOnly);
    file.write(text);
}

// A stage with a sublayer, a payload and a few prims, and a second, small one.
struct Scene {
    QTemporaryDir temp;
    QString root;
    QString other;
    explicit Scene(const QString& folder = {}) {
        const QString base = folder.isEmpty() ? temp.path() : folder;
        root = base + "/scene/root.usda";
        other = base + "/scene/other.usda";
        write(root, "#usda 1.0\n(\n    subLayers = [@./sub.usda@]\n)\n"
                    "def Xform \"World\" {\n"
                    "    def Xform \"Props\" (payload = @./payload.usda@</Props>) {}\n"
                    "    def Sphere \"Ball\" {}\n"
                    "}\n");
        write(base + "/scene/sub.usda", "#usda 1.0\nover \"World\" {\n    def Cube \"Box\" {}\n}\n");
        write(base + "/scene/payload.usda", "#usda 1.0\ndef Xform \"Props\" {\n    def Cube \"Chair\" {}\n}\n");
        write(other, "#usda 1.0\ndef Cone \"Cone\" {}\n");
    }
};

// A window like the app's: a fixed Outliner, and panel types to make more.
struct App {
    usdcc::usd::StageManager stages;
    std::shared_ptr<usdcc::usd::UndoManager> undo = std::make_shared<usdcc::usd::UndoManager>();
    usdcc::ui::ViewPanelRegistry types;
    std::unique_ptr<usdcc::ui::MainWindow> window = std::make_unique<usdcc::ui::MainWindow>();
    usdcc::usd::OutlinerViewPanel* outliner = nullptr;
    bool loading = false;
    int stagesOpenedWhileNotLoading = 0;
    usdcc::core::Signal<StageRefPtr>::Connection opened;

    App() {
        types.add("outliner", "Outliner", [this](const StageRefPtr& stage, const QString& title, QWidget* parent) -> usdcc::ui::ViewPanel* {
            return new usdcc::usd::OutlinerViewPanel(&stages, stage, undo, title, parent);
        });
        types.add("usda", "USDA", [this](const StageRefPtr& stage, const QString& title, QWidget* parent) -> usdcc::ui::ViewPanel* {
            return new usdcc::usd::UsdaViewPanel(&stages, stage, title, parent);
        });
        outliner = new usdcc::usd::OutlinerViewPanel(&stages, nullptr, undo, "Outliner", window.get());
        outliner->setProperty(usdcc::ui::ViewPanelRegistry::kTypeProperty, QStringLiteral("outliner"));
        window->dockManager()->addDockWidget(ads::CenterDockWidgetArea, outliner);
        window->resize(1000, 700);
        window->show();
        opened = stages.stageOpened.connect([this](const StageRefPtr&) {
            if (!loading) ++stagesOpenedWhileNotLoading;
        });
    }
    ~App() {
        for (const StageRefPtr& stage : std::vector<StageRefPtr>(stages.stages())) stages.closeStage(stage);
    }

    SessionController::Context context(usdcc::scripting::ScriptEditorSidePanel* scripts = nullptr,
                                        usdcc::scripting::PluginManager* plugins = nullptr) {
        SessionController::Context context;
        context.stageManager = &stages;
        context.window = window.get();
        context.panelTypes = &types;
        context.plugins = plugins;
        context.python = &pythonTestApp().python;
        context.scriptEditor = scripts;
        context.fixedPanels = {QStringLiteral("Outliner")};
        context.setLoading = [this](bool on) { loading = on; };
        return context;
    }
};

SessionController::LoadOptions quiet(bool discard = true) {
    SessionController::LoadOptions options;
    options.interactive = false;
    options.discardUnsaved = discard;
    return options;
}

QStringList stageFiles(const usdcc::usd::StageManager& stages) {
    QStringList files;
    for (const StageRefPtr& stage : stages.stages()) {
        files << QDir::fromNativeSeparators(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetRealPath()));
    }
    return files;
}
}  // namespace

USDCC_TEST(session_save_load_round_trip) {
    TestSettings settings;
    QTemporaryDir folder;
    Scene scene;
    App app;
    usdcc::scripting::ScriptEditorSidePanel scripts(pythonTestApp().python, pythonTestApp().runner, nullptr,
                                                   folder.filePath("scripts.ini"));
    SessionController sessions(app.context(&scripts), folder.filePath("sessions"));

    const StageRefPtr root = app.stages.openStage(scene.root.toStdString());
    const StageRefPtr other = app.stages.openStage(scene.other.toStdString());
    CHECK(root && other, "the stages open");
    if (!root || !other) return;
    const PXR_NS::UsdStageRefPtr usdRoot = root->usdStage();
    CHECK(usdRoot->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Chair")), "the payload is loaded at first");

    // What a session keeps of a stage.
    app.stages.setSelectedPaths(root, {PXR_NS::SdfPath("/World/Ball")});
    usdRoot->Unload(PXR_NS::SdfPath("/World/Props"));
    usdRoot->MuteLayer(QFileInfo(scene.root).absolutePath().toStdString() + "/sub.usda");
    CHECK(!usdRoot->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")), "muting the sublayer hides its cube");
    root->setEditTargetLayer(usdRoot->GetSessionLayer());
    usdRoot->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball"))
        .CreateAttribute(PXR_NS::TfToken("note"), PXR_NS::SdfValueTypeNames->String)
        .Set(std::string("in the session layer"));
    app.stages.setCurrentStage(other);

    // Panels: the Outliner on `root`, expanded, and a USDA panel on `other`.
    app.outliner->setStage(root);
    auto* tree = app.outliner->findChild<QTreeView*>();
    if (tree && tree->model()->rowCount() > 0) tree->expand(tree->model()->index(0, 0));
    settle();
    usdcc::ui::ViewPanel* usda = app.types.open("usda", other, app.window->dockManager(), app.window.get());
    const QString usdaName = usda ? usda->objectName() : QString();
    CHECK(usda != nullptr, "a USDA panel opens");

    // The Script Editor: a scratch script and a file.
    write(folder.filePath("tool.py"), "print('tool')\n");
    scripts.newScript();
    scripts.editor()->setPlainText("print('scratch')\n");
    CHECK(scripts.openFile(folder.filePath("tool.py")), "a script file opens");
    const int scriptTabs = scripts.tabCount();

    QString error;
    CHECK(sessions.saveWarning().isEmpty(), "nothing unsaved to warn about: %s", qPrintable(sessions.saveWarning()));
    CHECK(sessions.save("Round trip", QImage(), false, &error), "saves: %s", qPrintable(error));
    CHECK(sessions.currentName() == "Round trip", "and it's the current session");
    const auto json = sessions.store().read("Round trip");
    CHECK(json && json->value("stages").toArray().size() == 2, "both stages are in it");
    CHECK(json && json->value("layout").toObject().value("dockLayout").toString().startsWith("<?xml"),
          "the dock layout as readable XML");

    // Everything changes.
    if (usda) usda->deleteDockWidget();
    settle();
    for (const StageRefPtr& stage : std::vector<StageRefPtr>(app.stages.stages())) app.stages.closeStage(stage);
    app.stages.openStage(USDCC_TEST_DATA_DIR "/cube_and_sphere.usda");
    scripts.newScript();
    scripts.editor()->setPlainText("print('not in the session')\n");

    app.stagesOpenedWhileNotLoading = 0;
    const auto result = sessions.load("Round trip", quiet(), &error);
    settle();
    CHECK(result == SessionController::LoadResult::Loaded, "loads: %s", qPrintable(error));
    CHECK(app.stagesOpenedWhileNotLoading == 0, "the stages open while the app knows a session is loading");
    const QStringList files = stageFiles(app.stages);
    CHECK(files == QStringList({QDir::fromNativeSeparators(QFileInfo(scene.root).canonicalFilePath()),
                                QDir::fromNativeSeparators(QFileInfo(scene.other).canonicalFilePath())}),
          "the same stages, in order, and only those: %s", qPrintable(files.join(", ")));
    if (app.stages.stages().size() != 2) return;
    const StageRefPtr root2 = app.stages.stages()[0];
    const StageRefPtr other2 = app.stages.stages()[1];
    const PXR_NS::UsdStageRefPtr usd2 = root2->usdStage();
    CHECK(app.stages.currentStage() == other2, "the current stage");
    CHECK(app.stages.selectedPaths(root2) == std::vector<PXR_NS::SdfPath>{PXR_NS::SdfPath("/World/Ball")}, "the selection");
    CHECK(!usd2->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")), "the sublayer muted");
    CHECK(usd2->GetPrimAtPath(PXR_NS::SdfPath("/World/Props")) && !usd2->GetPrimAtPath(PXR_NS::SdfPath("/World/Props")).IsLoaded(),
          "the payload unloaded");
    CHECK(!usd2->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Chair")), "so what's in it isn't there");
    CHECK(root2->editTargetLayer() == usd2->GetSessionLayer(), "edits go to the session layer again");
    std::string note;
    usd2->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).GetAttribute(PXR_NS::TfToken("note")).Get(&note);
    CHECK(note == "in the session layer", "with what was in it: '%s'", note.c_str());

    CHECK(app.outliner->currentStage() == root2, "the Outliner shows its stage");
    auto* usda2 = qobject_cast<usdcc::ui::ViewPanel*>(app.window->dockManager()->findDockWidget(usdaName));
    CHECK(usda2 && usda2->currentStage() == other2 && !usda2->isClosed(), "the USDA panel is back, on its stage");
    auto* tree2 = app.outliner->findChild<QTreeView*>();
    CHECK(tree2 && tree2->model()->rowCount() > 0 && tree2->isExpanded(tree2->model()->index(0, 0)), "the Outliner's expanded prim");

    CHECK(scripts.tabCount() == scriptTabs, "the Script Editor's tabs, and only those: %d of %d", scripts.tabCount(), scriptTabs);
    bool scratch = false;
    bool file = false;
    for (int i = 0; i < scripts.tabCount(); ++i) {
        scripts.setCurrentTab(i);
        scratch = scratch || (scripts.currentFile().isEmpty() && scripts.editor()->toPlainText() == "print('scratch')\n");
        file = file || scripts.currentFile() == folder.filePath("tool.py");
        CHECK(!(scripts.currentFile().isEmpty() && scripts.editor()->toPlainText().contains("not in the session")),
              "the script that wasn't in it is gone");
    }
    CHECK(scratch && file, "the scratch script with its text, and the file");
}

USDCC_TEST(session_unsaved_edits) {
    TestSettings settings;
    QTemporaryDir folder;
    Scene scene;
    App app;
    SessionController sessions(app.context(), folder.filePath("sessions"));
    const StageRefPtr root = app.stages.openStage(scene.root.toStdString());
    const StageRefPtr memory = app.stages.createStage();
    if (!root || !memory) return;
    memory->usdStage()->DefinePrim(PXR_NS::SdfPath("/Made"));
    root->usdStage()->GetRootLayer()->SetDocumentation("edited");
    const QString warning = sessions.saveWarning();
    CHECK(warning.contains("root.usda") && warning.contains("unsaved"), "warns about unsaved edits: %s", qPrintable(warning));
    CHECK(!warning.contains("in memory") && !warning.contains("Untitled"), "not about the stage in memory, which is kept whole");

    QString asked;
    sessions.prompts().saveDespite = [&asked](const QString& message) {
        asked = message;
        return false;
    };
    QString error;
    CHECK(!sessions.save("Unsaved", QImage(), true, &error) && error.isEmpty(), "saving can be cancelled");
    CHECK(asked == warning && !sessions.store().contains("Unsaved"), "after the warning, and nothing is written");
    sessions.prompts().saveDespite = [](const QString&) { return true; };
    CHECK(sessions.save("Unsaved", QImage(), true, &error), "or saved anyway: %s", qPrintable(error));
    const auto json = sessions.store().read("Unsaved");
    CHECK(json && json->value("stages").toArray().size() == 2 && json->value("stages").toArray()[1].toObject().contains("inMemory"),
          "the in-memory stage is in it, as its text");

    CHECK(sessions.load("Unsaved", quiet(false), &error) == SessionController::LoadResult::Failed && error.contains("unsaved"),
          "a quiet load refuses while edits are unsaved: %s", qPrintable(error));
    CHECK(app.stages.stages().size() == 2, "and changes nothing");
    CHECK(sessions.load("Unsaved", quiet(true), &error) == SessionController::LoadResult::Loaded, "unless told to drop them: %s",
          qPrintable(error));
    CHECK(app.stages.stages().size() == 2 &&
              app.stages.stages()[0]->usdStage()->GetRootLayer()->GetDocumentation().empty(),
          "the file comes back as it is on disk, not as it was edited");
    CHECK(app.stages.stages().size() == 2 && app.stages.stages()[1]->usdStage()->GetRootLayer()->IsAnonymous() &&
              app.stages.stages()[1]->usdStage()->GetPrimAtPath(PXR_NS::SdfPath("/Made")),
          "and the in-memory stage comes back with what was in it");
}

USDCC_TEST(session_moved_files) {
    TestSettings settings;
    QTemporaryDir folder;
    QTemporaryDir elsewhere;
    QString saved;
    {
        Scene scene(folder.filePath("anna"));
        App app;
        SessionController sessions(app.context(), folder.filePath("sessions"));
        app.stages.openStage(scene.root.toStdString());
        app.stages.openStage(scene.other.toStdString());
        sessions.save("Shared", QImage(), false);
        saved = folder.filePath("Shared.json");
        sessions.store().exportTo("Shared", saved);
    }
    // On another machine: the project is somewhere else, and not where it was.
    QDir(folder.filePath("anna")).removeRecursively();
    Scene moved(elsewhere.filePath("work"));
    App app;
    SessionController sessions(app.context(), elsewhere.filePath("sessions"));
    QString error;
    CHECK(sessions.store().importFile(saved, false, &error) == "Shared", "imports: %s", qPrintable(error));
    const auto missing = usdcc::ui::session::missingFiles(*sessions.store().read("Shared"));
    CHECK(missing.size() == 2, "both stage files are missing here: %d", int(missing.size()));

    QStringList asked;
    sessions.prompts().locate = [&](const QString& file) {
        asked << file;
        return elsewhere.filePath("work/scene/") + QFileInfo(file).fileName();
    };
    SessionController::LoadOptions options;
    options.source = "Shared";
    CHECK(sessions.load("Shared", options, &error) == SessionController::LoadResult::Loaded, "loads: %s", qPrintable(error));
    CHECK(asked.size() == 1, "locating one file finds the other beside it: asked %d times", int(asked.size()));
    CHECK(app.stages.stages().size() == 2, "both stages open from where they are now");
    const auto stored = sessions.store().read("Shared");
    CHECK(stored && usdcc::ui::session::missingFiles(*stored).isEmpty(), "and the session remembers, so it's asked once");
}

USDCC_TEST(session_plugins_differ) {
    TestSettings settings;
    QTemporaryDir folder;
    const QString root = folder.filePath("plugins");
    for (const char* id : {"sess_on", "sess_off"}) {
        write(root + "/" + id + "/usdcc-plugin.json",
              QByteArray("{\"id\": \"") + id + "\", \"name\": \"" + id + "\", \"version\": \"1.0\", \"api\": 1, "
                                                        "\"python\": {\"module\": \"" + id + "_module\"}}");
        write(root + "/" + id + "/python/" + id + "_module/__init__.py", "");
    }
    usdcc::scripting::PluginManager::Options pluginOptions;
    pluginOptions.folders = {root.toStdU16String()};
    usdcc::scripting::PluginManager plugins(pluginOptions);
    plugins.setEnabled("sess_off", false);
    plugins.scan();
    plugins.activate(pythonTestApp().python);
    CHECK(plugins.find("sess_on") && plugins.find("sess_on")->state == usdcc::scripting::PluginState::Active, "one plugin runs");

    App app;
    QStringList restartedWith;
    bool restartWorks = false;
    auto context = app.context(nullptr, &plugins);
    context.restart = [&](const QStringList& arguments) {
        restartedWith = arguments;
        return restartWorks;
    };
    SessionController sessions(context, folder.filePath("sessions"));
    QJsonObject json = sessions.capture("Plugins", QImage());
    CHECK(json.value("plugins").toArray().size() == 1, "the session lists the running plugin");

    // Saved elsewhere with the other one, and one this machine doesn't have.
    QJsonArray other;
    QJsonObject off;
    off.insert("id", "sess_off");
    QJsonObject absent;
    absent.insert("id", "sess_absent");
    absent.insert("name", "Absent Plugin");
    other << off << absent;
    json.insert("plugins", other);
    const auto difference = sessions.pluginDifference(json);
    CHECK(difference.enable == QStringList{"sess_off"} && difference.disable == QStringList{"sess_on"},
          "one to turn on, one off: +%s -%s", qPrintable(difference.enable.join(",")), qPrintable(difference.disable.join(",")));
    CHECK(difference.missing == QStringList{"Absent Plugin"}, "and one that isn't here: %s", qPrintable(difference.missing.join(",")));

    QString error;
    CHECK(sessions.load(json, quiet(), &error) == SessionController::LoadResult::Failed && error.contains("restart"),
          "a quiet load can't restart: %s", qPrintable(error));

    sessions.prompts().plugins = [](const QString&) { return SessionController::Prompts::PluginAnswer::RestartAndLoad; };
    SessionController::LoadOptions options;
    options.source = "Plugins";
    CHECK(sessions.load(json, options, &error) == SessionController::LoadResult::Cancelled, "a restart the user cancels");
    CHECK(plugins.isEnabled("sess_on") && !plugins.isEnabled("sess_off"), "changes no plugin");
    restartWorks = true;
    CHECK(sessions.load(json, options, &error) == SessionController::LoadResult::Restarting, "restarts");
    CHECK(restartedWith == QStringList({"--open-session", "Plugins"}), "into the session: %s", qPrintable(restartedWith.join(" ")));
    CHECK(!plugins.isEnabled("sess_on") && plugins.isEnabled("sess_off"), "with the plugins switched for the next start");

    sessions.prompts().plugins = [](const QString&) { return SessionController::Prompts::PluginAnswer::LoadWithout; };
    CHECK(sessions.load(json, options, &error) == SessionController::LoadResult::Loaded, "or loads without them: %s",
          qPrintable(error));
    CHECK(sessions.warnings().join("\n").contains("Absent Plugin"), "saying what isn't here");
    plugins.deactivate(pythonTestApp().python);
}

USDCC_TEST(session_python_api) {
    TestSettings settings;
    QTemporaryDir folder;
    Scene scene;
    App app;
    SessionController sessions(app.context(), folder.filePath("sessions"));
    auto& python = pythonTestApp().python;
    const usdcc::scripting::AppContextScope scope(
        {&app.stages, app.undo.get(), &pythonTestApp().tools, app.window.get(), &app.types, nullptr, &sessions});
    app.stages.openStage(scene.root.toStdString());

    CHECK(python.runString("import usdcc.app\n"
                           "saved = {}\n"
                           "usdcc.app.on_session_save(lambda: {'preset': 'draft'}, key='test.sessions')\n"
                           "usdcc.app.on_session_load(lambda data: saved.update(data), key='test.sessions')\n"
                           "usdcc.app.save_session('From Python')\n"
                           "assert usdcc.app.sessions() == ['From Python'], usdcc.app.sessions()\n"
                           "assert usdcc.app.current_session() == 'From Python'\n",
                           "session_api.py", usdcc::scripting::PythonRuntime::Namespace::Session),
          "saves from Python");
    const auto json = sessions.store().read("From Python");
    CHECK(json && json->value("pluginData").toObject().value("test.sessions").toObject().value("preset").toString() == "draft",
          "with what the hook gave");

    CHECK(python.runString("import usdcc.app\n"
                           "usdcc.app.load_session('From Python')\n"
                           "assert saved == {'preset': 'draft'}, saved\n"
                           "try:\n"
                           "    usdcc.app.load_session('Nope')\n"
                           "    raise AssertionError('loaded a session that is not there')\n"
                           "except RuntimeError as error:\n"
                           "    assert 'Nope' in str(error), error\n"
                           "path = usdcc.app.export_session('From Python', r'" +
                               folder.filePath("out.json").toStdString() + "')\n"
                           "usdcc.app.delete_session('From Python')\n"
                           "assert usdcc.app.sessions() == []\n"
                           "assert usdcc.app.import_session(r'" + folder.filePath("out.json").toStdString() +
                               "') == 'From Python'\n",
                           "session_api2.py", usdcc::scripting::PythonRuntime::Namespace::Session),
          "loads, deletes, exports and imports from Python, and the load hook gets its data");
    python.resetSession();
}

USDCC_TEST(session_window_layout) {
    using usdcc::ui::fitOnScreens;
    const QList<QRect> screens{QRect(0, 0, 1920, 1040), QRect(1920, 0, 2560, 1400)};
    CHECK(fitOnScreens(QRect(100, 100, 400, 300), screens) == QRect(100, 100, 400, 300), "inside a screen: unchanged");
    CHECK(fitOnScreens(QRect(4300, 100, 400, 300), screens) == QRect(4080, 100, 400, 300), "half off the right: moved in");
    const QRect huge = fitOnScreens(QRect(-50, -50, 3000, 2000), screens);
    CHECK(huge == QRect(0, 0, 1920, 1040), "bigger than its screen: shrunk to it: %d,%d %dx%d", huge.x(), huge.y(), huge.width(),
          huge.height());
    const QRect lost = fitOnScreens(QRect(9000, 5000, 400, 300), screens);
    CHECK(QRect(1920, 0, 2560, 1400).contains(lost), "off every screen: onto the nearest");

    TestSettings settings;
    usdcc::ui::MainWindow window;
    window.resize(1200, 800);
    window.show();
    settle();
    const QJsonObject state = window.layoutState();
    CHECK(state.value("windowSize").toArray().at(0).toInt() == 1200, "the window's size is kept");
    CHECK(state.value("dockLayout").toString().startsWith("<?xml"), "the dock layout as XML");
    window.restoreLayoutState(state, false);
    CHECK(window.size() == QSize(1200, 800), "restoring into the running window keeps its size");
}

USDCC_TEST(session_dialog) {
    TestSettings settings;
    QTemporaryDir folder;
    Scene scene;
    App app;
    SessionController sessions(app.context(), folder.filePath("sessions"));
    sessions.prompts().saveDespite = [](const QString&) { return true; };
    app.stages.openStage(scene.root.toStdString());

    QImage picture(1600, 900, QImage::Format_RGB32);
    picture.fill(Qt::darkGreen);
    usdcc::scripting::SessionsDialog dialog(&sessions, picture, app.window.get());
    dialog.show();
    settle();
    CHECK(dialog.list()->count() == 0, "no sessions at first");
    CHECK(dialog.saveAs("First"), "saves from the name field");
    CHECK(dialog.list()->count() == 1 && dialog.selected() == "First", "it's listed and selected");
    const auto json = sessions.store().read("First");
    CHECK(json && usdcc::ui::session::preview(*json).width() == 1280, "with the picture taken before, at 1280 wide");

    settle(20);
    CHECK(dialog.saveAs("Second"), "a second");
    CHECK(dialog.list()->count() == 2 && dialog.list()->item(0)->data(Qt::UserRole).toString() == "Second", "newest first");
    settle(50);
    CHECK(!dialog.list()->item(1)->icon().isNull(), "with a thumbnail, filled in after");

    dialog.select("First");
    CHECK(dialog.renameSelected("Renamed"), "renames");
    CHECK(sessions.store().contains("Renamed") && !sessions.store().contains("First"), "the file too");
    CHECK(dialog.exportSelected(folder.filePath("sent.json")), "exports");
    CHECK(dialog.deleteSelected(), "deletes");
    CHECK(dialog.list()->count() == 1, "and it's gone from the list");
    CHECK(dialog.importFile(folder.filePath("sent.json")) && dialog.selected() == "Renamed", "imports it back");

    dialog.select("Second");
    CHECK(dialog.loadSelected(), "loads");
    CHECK(sessions.currentName() == "Second" && dialog.result() == QDialog::Accepted, "and closes");
}
