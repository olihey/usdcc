// A plugin's tab in the Script Editor (docs/PLUGIN_DEV.md): the tree, its own tabs,
// Reload saving first, the session, and files changing on disk.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/app_module.h"
#include "usdcc/scripting/code_editor.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_editor_side_panel.h"
#include "usdcc/ui/main_window.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeView>

#include <filesystem>
#include <fstream>
#include <string>

using usdcc::scripting::PluginManager;
using usdcc::scripting::PluginState;
using usdcc::scripting::ScriptEditorSidePanel;
using usdcc::test::pythonTestApp;

namespace {
void write(const QString& path, const QString& text) {
    std::filesystem::path file(path.toStdU16String());
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text.toStdString();
}

QString read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// Runs events until `done`, or a few seconds.
template <typename Check>
bool waitFor(Check done, int ms = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return done();
}

// The names under `parent`, as far as it's loaded.
QStringList namesIn(QAbstractItemModel* model, const QModelIndex& parent) {
    QStringList names;
    for (int row = 0; row < model->rowCount(parent); ++row) names << model->index(row, 0, parent).data().toString();
    return names;
}

struct Fixture {
    QTemporaryDir temp;
    QString folder;
    QString mainFile;
    Fixture() {
        folder = temp.filePath(QStringLiteral("plugins/tab_plugin"));
        mainFile = folder + "/python/usdcc_tab_plugin/__init__.py";
        write(folder + "/usdcc-plugin.json",
              R"({"id": "t.tab", "name": "Tab Plugin", "version": "1.0", "api": 1, "python": {"module": "usdcc_tab_plugin"}})");
        write(mainFile, "VALUE = 1\n");
        write(folder + "/python/usdcc_tab_plugin/other.py", "OTHER = 1\n");
        write(folder + "/python/usdcc_tab_plugin/__pycache__/x.cpython-312.pyc", "compiled");
        write(folder + "/python/usdcc_tab_plugin/stale.pyc", "compiled");
        write(folder + "/notes.txt", "some notes\n");
        std::ofstream(std::filesystem::path(folder.toStdU16String()) / "blob.bin", std::ios::binary) << std::string("a\0b", 3);
    }
    QString settings() const { return temp.filePath(QStringLiteral("settings.ini")); }
};
}  // namespace

USDCC_TEST(plugin_tab_tree_and_tabs) {
    Fixture fixture;
    auto& app = pythonTestApp();
    ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
    panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
    panel.newScript();
    // A tree reads folders once it's on screen.
    panel.show();
    const int before = panel.tabCount();

    panel.openPlugin("t.tab", "Tab Plugin", fixture.folder, fixture.mainFile);
    CHECK(panel.tabCount() == before + 1, "a plugin is one tab");
    CHECK(panel.tabBar()->tabText(panel.currentTab()) == "Tab Plugin", "named after it");
    // The icons tell the two kinds apart.
    const QIcon pluginIcon = panel.tabBar()->tabIcon(panel.currentTab());
    const QIcon fileIcon = panel.tabBar()->tabIcon(before - 1);
    CHECK(!pluginIcon.isNull() && !fileIcon.isNull(), "both kinds of tab have an icon");
    CHECK(pluginIcon.cacheKey() != fileIcon.cacheKey(), "and they differ");
    CHECK(panel.currentPluginId() == "t.tab", "and it's current");
    CHECK(panel.currentName() == "__init__.py", "with its main file open: %s", qPrintable(panel.currentName()));
    CHECK(panel.editor() && panel.editor()->toPlainText() == "VALUE = 1\n", "showing its text");
    CHECK(!panel.isPreview(), "kept, not a preview");

    QTreeView* tree = panel.pluginTree();
    CHECK(tree != nullptr, "there's a tree");
    if (!tree) return;
    QAbstractItemModel* model = tree->model();
    CHECK(waitFor([&]() { return namesIn(model, tree->rootIndex()).contains("notes.txt"); }), "showing the plugin's folder");
    const QStringList top = namesIn(model, tree->rootIndex());
    CHECK(top.contains("python") && top.contains("usdcc-plugin.json"), "and only that: %s", qPrintable(top.join(",")));
    // Persistent: the model sorts and adds rows as it reads the disk.
    QPersistentModelIndex python;
    for (int row = 0; row < model->rowCount(tree->rootIndex()); ++row) {
        if (model->index(row, 0, tree->rootIndex()).data() == "python") python = model->index(row, 0, tree->rootIndex());
    }
    CHECK(waitFor([&]() { return tree->isExpanded(python) && model->rowCount(python) > 0; }), "the code is open at first");
    const QPersistentModelIndex package = model->index(0, 0, python);
    CHECK(waitFor([&]() { return namesIn(model, package).contains("other.py"); }), "down to the package");
    CHECK(!namesIn(model, package).contains("__pycache__") && !namesIn(model, package).contains("stale.pyc"), "without what Python leaves behind: %s",
          qPrintable(namesIn(model, package).join(",")));

    // Going again goes to the same one.
    const int tabs = panel.tabCount();
    panel.setCurrentTab(0);
    panel.openPlugin("t.tab", "Tab Plugin", fixture.folder, fixture.mainFile);
    CHECK(panel.tabCount() == tabs && panel.currentPluginId() == "t.tab", "opening it again doesn't add one");

    // Previews.
    QTabBar* bar = panel.pluginTabBar();
    CHECK(bar && bar->tabIcon(0).isNull(), "a plugin's own tabs don't need one");
    CHECK(bar && bar->count() == 1, "one file so far");
    CHECK(panel.openInPlugin(fixture.folder + "/python/usdcc_tab_plugin/other.py", true) && panel.isPreview(), "a click opens a preview");
    CHECK(bar->tabText(bar->currentIndex()).endsWith(QStringLiteral("◦")), "marked as one: %s", qPrintable(bar->tabText(bar->currentIndex())));
    CHECK(bar->count() == 2, "beside the first");
    CHECK(panel.openInPlugin(fixture.folder + "/notes.txt", true) && bar->count() == 2, "the next click replaces it");
    CHECK(panel.currentName() == "notes.txt" && panel.editor()->toPlainText() == "some notes\n", "showing the new file");
    CHECK(panel.openInPlugin(fixture.folder + "/notes.txt", false) && !panel.isPreview(), "a double-click keeps it");
    CHECK(bar->tabText(bar->currentIndex()) == "notes.txt", "with no mark: %s", qPrintable(bar->tabText(bar->currentIndex())));
    CHECK(panel.openInPlugin(fixture.folder + "/python/usdcc_tab_plugin/other.py", true) && bar->count() == 3, "so the next is another tab");
    panel.editor()->insertPlainText("# typed\n");
    CHECK(!panel.isPreview(), "typing keeps a preview too");
    CHECK(panel.openInPlugin(fixture.mainFile, true) && bar->count() == 3, "a file that's open just shows");
    CHECK(!panel.openInPlugin(fixture.folder + "/blob.bin", true) && bar->count() == 3, "a binary file isn't opened");

    // Ctrl+W on its files, then the tab.
    CHECK(panel.closeTab(panel.currentTab()), "closing the tab closes the plugin");
    CHECK(panel.currentPluginId().isEmpty() && panel.tabCount() == before, "leaving the others");
    CHECK(panel.editor() != nullptr, "and an editor to work in");
}

USDCC_TEST(plugin_tab_icons_follow_the_file) {
    Fixture fixture;
    auto& app = pythonTestApp();
    ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
    panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
    panel.openFile(fixture.mainFile);
    CHECK(!panel.tabBar()->tabIcon(panel.currentTab()).isNull(), "a Python file has one");
    panel.newScript();
    CHECK(!panel.tabBar()->tabIcon(panel.currentTab()).isNull(), "so does an untitled script");
    panel.openFile(fixture.folder + "/notes.txt");
    CHECK(panel.tabBar()->tabIcon(panel.currentTab()).isNull(), "another kind of file doesn't");
    const QString renamed = fixture.folder + "/notes.py";
    panel.renameTab(panel.currentTab(), "notes.py");
    CHECK(QFile::exists(renamed) && !panel.tabBar()->tabIcon(panel.currentTab()).isNull(), "until it's renamed to one");
}

USDCC_TEST(plugin_tab_reload_saves_first) {
    Fixture fixture;
    auto& app = pythonTestApp();
    PluginManager::Options options;
    options.folders = {std::filesystem::path(fixture.temp.filePath("plugins").toStdU16String())};
    options.crashMarker = std::filesystem::path(fixture.temp.filePath("marker.txt").toStdU16String());
    PluginManager manager(options);
    manager.scan();
    manager.activate(app.python);
    CHECK(manager.find("t.tab")->state == PluginState::Active, "it starts: %s", manager.find("t.tab")->reason.c_str());

    ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
    panel.setPluginHooks({[&manager](const std::string& id) { return manager.whyNotReloadable(id); },
                          [&manager, &app](const std::string& id) { return manager.reload(id, app.python); }});
    int signalled = 0;
    QObject::connect(&panel, &ScriptEditorSidePanel::pluginReloaded, &panel, [&signalled]() { ++signalled; });
    panel.openPlugin("t.tab", "Tab Plugin", fixture.folder, fixture.mainFile);
    panel.editor()->setPlainText("VALUE = 2\n");
    panel.editor()->document()->setModified(true);

    CHECK(panel.run(), "Run on a plugin's tab is Reload: %s", qPrintable(panel.output()->toPlainText()));
    CHECK(read(fixture.mainFile) == "VALUE = 2\n", "which saved it first");
    CHECK(!panel.isModified(), "so nothing is unsaved");
    const auto value = app.python.evaluate("__import__('usdcc_tab_plugin').VALUE");
    CHECK(value && *value == "2", "and the new code runs");
    CHECK(panel.output()->toPlainText().contains("Reload Tab Plugin"), "the output says so: %s", qPrintable(panel.output()->toPlainText()));
    CHECK(signalled == 1, "and tells the Plugins panel");

    panel.editor()->setPlainText("def broken(:\n");
    panel.editor()->document()->setModified(true);
    CHECK(!panel.run(), "a failure returns false");
    CHECK(panel.output()->toPlainText().contains("SyntaxError"), "with the error in the output: %s", qPrintable(panel.output()->toPlainText()));
    CHECK(panel.output()->toPlainText().contains("__init__.py"), "naming the file");
    panel.editor()->setPlainText("VALUE = 3\n");
    panel.editor()->document()->setModified(true);
    CHECK(panel.run(), "and fixing it works");
    manager.deactivate(app.python);
}

USDCC_TEST(plugin_tab_session) {
    Fixture fixture;
    auto& app = pythonTestApp();
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
        panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
        panel.openPlugin("t.tab", "Tab Plugin", fixture.folder, fixture.mainFile);
        panel.openInPlugin(fixture.folder + "/notes.txt", false);
        panel.editor()->insertPlainText("unsaved ");
    }
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
        panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
        CHECK(panel.currentPluginId() == "t.tab", "the plugin's tab comes back");
        CHECK(panel.pluginTabBar() && panel.pluginTabBar()->count() == 2, "with its files");
        CHECK(panel.currentName() == "notes.txt", "the same one current: %s", qPrintable(panel.currentName()));
        CHECK(panel.isModified() && panel.editor()->toPlainText().startsWith("unsaved "), "and its unsaved text");
        // An old session's plain tab next to it still works.
        panel.newScript();
        CHECK(panel.currentPluginId().isEmpty(), "other tabs carry on");
    }
    std::filesystem::remove_all(std::filesystem::path(fixture.folder.toStdU16String()));
    {
        ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
        bool any = false;
        for (int i = 0; i < panel.tabCount(); ++i) {
            panel.setCurrentTab(i);
            any = any || !panel.currentPluginId().isEmpty();
        }
        CHECK(!any, "a plugin that's gone drops out");
        CHECK(panel.tabCount() >= 1, "leaving something to work in");
    }
}

USDCC_TEST(plugin_tab_file_actions) {
    Fixture fixture;
    auto& app = pythonTestApp();
    ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
    panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
    panel.openPlugin("t.tab", "Tab Plugin", fixture.folder, fixture.mainFile);
    QTabBar* bar = panel.pluginTabBar();

    const QString package = fixture.folder + "/python/usdcc_tab_plugin";
    CHECK(panel.newPluginFile(package, "new_file.py") && QFile::exists(package + "/new_file.py"), "a new file");
    CHECK(panel.currentName() == "new_file.py", "is opened");
    CHECK(!panel.newPluginFile(package, "new_file.py"), "and isn't made twice");
    CHECK(panel.newPluginFolder(package, "sub") && QFileInfo(package + "/sub").isDir(), "a new folder");

    CHECK(panel.renamePluginPath(package + "/new_file.py", "renamed.py"), "renaming");
    CHECK(panel.currentName() == "renamed.py" && panel.currentFile().endsWith("renamed.py"), "the tab follows: %s", qPrintable(panel.currentFile()));
    write(package + "/sub/inner.py", "x = 1\n");
    panel.openInPlugin(package + "/sub/inner.py", false);
    CHECK(panel.renamePluginPath(package + "/sub", "sub2") && panel.currentFile().endsWith("sub2/inner.py"), "a folder's tabs too: %s", qPrintable(panel.currentFile()));

    const int open = bar->count();
    CHECK(panel.deletePluginPath(package + "/sub2") && !QFile::exists(package + "/sub2"), "deleting");
    CHECK(bar->count() == open - 1, "closes what was open in it");
}

USDCC_TEST(plugin_tab_files_change_on_disk) {
    Fixture fixture;
    auto& app = pythonTestApp();
    ScriptEditorSidePanel panel(app.python, app.runner, nullptr, fixture.settings());
    panel.setSavePrompt([](const QString&) { return QMessageBox::Discard; });
    int asked = 0;
    bool answer = false;
    panel.setDiskChangePrompt([&](const QString&) {
        ++asked;
        return answer;
    });
    panel.openFile(fixture.mainFile);
    // The watcher starts on a thread of its own, a moment after the file opens.
    waitFor([]() { return false; }, 300);

    write(fixture.mainFile, "VALUE = 5\n");
    CHECK(waitFor([&]() { return panel.editor()->toPlainText() == "VALUE = 5\n"; }), "an unmodified tab just updates: %s", qPrintable(panel.editor()->toPlainText()));
    CHECK(asked == 0 && !panel.isModified(), "without asking");

    // Our own save isn't a change.
    panel.editor()->insertPlainText("# mine\n");
    panel.save();
    waitFor([]() { return false; }, 400);
    CHECK(asked == 0, "saving here doesn't ask");

    // And it still follows after a save replaced the file.
    write(fixture.mainFile, "VALUE = 6\n");
    CHECK(waitFor([&]() { return panel.editor()->toPlainText() == "VALUE = 6\n"; }), "it goes on watching after a save");

    panel.editor()->insertPlainText("# unsaved\n");
    write(fixture.mainFile, "VALUE = 7\n");
    CHECK(waitFor([&]() { return asked == 1; }), "unsaved changes ask");
    CHECK(panel.editor()->toPlainText().contains("# unsaved"), "and No keeps them");

    answer = true;
    write(fixture.mainFile, "VALUE = 8\n");
    CHECK(waitFor([&]() { return panel.editor()->toPlainText() == "VALUE = 8\n"; }), "Yes loads the file");
    CHECK(!panel.isModified(), "which isn't a change");
    panel.editor()->undo();
    CHECK(panel.editor()->toPlainText().contains("# unsaved"), "and can be undone");
}

USDCC_TEST(plugin_new_from_template) {
    QTemporaryDir temp;
    auto& app = pythonTestApp();
    const std::filesystem::path parent(temp.path().toStdU16String());
    PluginManager::Options options;
    options.folders = {parent};
    options.crashMarker = parent / "marker.txt";
    PluginManager manager(options);
    // Its template adds a menu action.
    QStandardPaths::setTestModeEnabled(true);
    usdcc::ui::MainWindow window;
    const usdcc::scripting::AppContextScope scope({&app.stages, &app.undo, &app.tools, &window});
    manager.scan();
    manager.activate(app.python);

    std::string error;
    const std::string id = manager.createPlugin("My \"New\" Tool!", parent, app.python, &error);
    CHECK(id == "user.my_new_tool", "an id from its name: %s (%s)", id.c_str(), error.c_str());
    const auto* plugin = manager.find(id);
    CHECK(plugin && plugin->state == PluginState::Active, "and it's running with no restart: %s", plugin ? plugin->reason.c_str() : "");
    CHECK(plugin && plugin->manifest.name == "My \"New\" Tool!", "keeping its name");
    CHECK(std::filesystem::exists(parent / "my_new_tool" / "usdcc-plugin.json"), "a manifest");
    const auto file = parent / "my_new_tool" / "python" / "my_new_tool_plugin" / "__init__.py";
    CHECK(std::filesystem::exists(file), "and code");
    const auto loaded = app.python.evaluate("hasattr(__import__('my_new_tool_plugin'), 'activate')");
    CHECK(loaded && *loaded == "True", "that was imported");

    const std::string second = manager.createPlugin("My New Tool", parent, app.python, &error);
    CHECK(second == "user.my_new_tool_2", "the same name again gets its own: %s", second.c_str());
    CHECK(manager.createPlugin("???", parent, app.python, &error).empty() && !error.empty(), "a name with nothing usable says so");

    // A later start finds what was made.
    manager.deactivate(app.python);
    PluginManager later(options);
    later.scan();
    CHECK(later.find(id) != nullptr, "it's found at the next start");
    QStandardPaths::setTestModeEnabled(false);
}
