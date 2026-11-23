// src/python/stubs/_usdcc_app.pyi, the types of the module compiled into the app, is what
// tools/app_stub.py makes of the bindings now (docs/VSCODE.md §4). And setting up a folder
// for VS Code.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/editor_setup.h"
#include "usdcc/scripting/debugger.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/script_editor_side_panel.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>

#include <string>

using usdcc::test::pythonTestApp;
namespace editor_setup = usdcc::scripting::editor_setup;

namespace {
QByteArray read(const QString& file) {
    QFile in(file);
    return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}
void write(const QString& file, const QByteArray& text) {
    QFile out(file);
    if (out.open(QIODevice::WriteOnly)) out.write(text);
}
}  // namespace

USDCC_TEST(app_stub_is_current) {
    const std::string source = USDCC_SOURCE_DIR;
    const bool update = !qEnvironmentVariableIsEmpty("USDCC_UPDATE_STUBS");
    const bool ok = pythonTestApp().python.runString(
        "import sys, difflib, pathlib\n"
        "sys.path.insert(0, r'" + source + "/tools')\n"
        "try:\n"
        "    import app_stub, _usdcc_app\n"
        "finally:\n"
        "    del sys.path[0]\n"
        "file = pathlib.Path(r'" + source + "/src/python/stubs/_usdcc_app.pyi')\n"
        "made = app_stub.generate(_usdcc_app)\n"
        "compile(made, 'stub', 'exec')\n"
        "if " + (update ? "True" : "False") + ":\n"
        "    file.write_bytes(made.encode())\n"
        "kept = file.read_bytes().decode().replace('\\r\\n', '\\n')\n"
        "difference = list(difflib.unified_diff(kept.split('\\n'), made.split('\\n'), 'kept', 'made', lineterm='', n=0))\n"
        "if difference:\n"
        "    raise AssertionError('run this test with USDCC_UPDATE_STUBS=1 (tools/app_stub.py):\\n' + '\\n'.join(difference[:40]))\n");
    CHECK(ok, "the stub is what the bindings make now");
}

USDCC_TEST(editor_setup_folder) {
    QTemporaryDir folder;
    const QString settingsFile = folder.filePath(".vscode/settings.json");
    const QString launchFile = folder.filePath(".vscode/launch.json");
    QString error;
    CHECK(editor_setup::setUpFolder(folder.path(), {"C:/types", "C:/scripts"}, "C:/python.exe", 5679, &error), "from nothing: %s", qPrintable(error));
    QJsonObject settings = QJsonDocument::fromJson(read(settingsFile)).object();
    QJsonObject launch = QJsonDocument::fromJson(read(launchFile)).object();
    CHECK(settings.value("python.analysis.extraPaths").toArray() == QJsonArray({"C:/types", "C:/scripts"}), "the paths");
    CHECK(settings.value("python.defaultInterpreterPath").toString() == "C:/python.exe", "the interpreter");
    CHECK(launch.value("configurations").toArray().at(0).toObject().value("connect").toObject().value("port").toInt() == 5679, "the debugger");

    // Again, and with the user's own.
    settings["editor.tabSize"] = 3;
    settings["python.defaultInterpreterPath"] = "mine";
    settings["python.analysis.extraPaths"] = QJsonArray({"C:/mine", "C:/types"});
    write(settingsFile, QJsonDocument(settings).toJson());
    CHECK(editor_setup::setUpFolder(folder.path(), {"C:/types", "C:/new"}, "C:/python.exe", 5679), "again");
    settings = QJsonDocument::fromJson(read(settingsFile)).object();
    launch = QJsonDocument::fromJson(read(launchFile)).object();
    CHECK(settings.value("python.analysis.extraPaths").toArray() == QJsonArray({"C:/types", "C:/new", "C:/mine"}), "keeps the user's paths");
    CHECK(settings.value("editor.tabSize").toInt() == 3 && settings.value("python.defaultInterpreterPath").toString() == "mine", "and settings");
    CHECK(launch.value("configurations").toArray().size() == 1, "one debugger configuration");

    // Comments would be lost: left as it is.
    const QByteArray commented = "{\n  // mine\n  \"a\": 1\n}\n";
    write(settingsFile, commented);
    CHECK(!editor_setup::setUpFolder(folder.path(), {"C:/types"}, {}, 5678, &error) && !error.isEmpty(), "refused: %s", qPrintable(error));
    CHECK(read(settingsFile) == commented, "and not touched");
}

USDCC_TEST(editor_setup_menu) {
    QTemporaryDir folder;
    const QString file = folder.filePath("shot.py");
    write(file, "print(1)\n");
    auto& app = pythonTestApp();
    usdcc::scripting::ScriptEditorSidePanel panel(app.python, app.runner, nullptr, folder.filePath("editor.ini"));
    CHECK(panel.openFile(file), "a file is open");
    QStringList asked;
    QObject::connect(&panel, &usdcc::scripting::ScriptEditorSidePanel::setUpVsCodeRequested, &panel, [&](const QString& path) { asked << path; });
    QMenu menu;
    panel.populateMenu(&menu);
    QAction *setUp = nullptr, *open = nullptr, *debug = nullptr;
    for (QAction* action : menu.actions()) {
        if (action->text() == QLatin1String("Set Up VS Code...")) setUp = action;
        if (action->text() == QLatin1String("Open in VS Code")) open = action;
        if (action->text() == QLatin1String("Allow Debugger to Attach")) debug = action;
    }
    CHECK(setUp && open && debug, "the menu has them");
    if (!setUp || !open || !debug) return;
    CHECK(debug->isEnabled() == (usdcc::scripting::debugger::port() == 0), "debugging can be allowed");
    CHECK(open->isEnabled() == !editor_setup::findVsCode().isEmpty(), "VS Code is there, or greyed out: %s", qPrintable(open->toolTip()));
    setUp->trigger();
    CHECK(asked == QStringList(QFileInfo(file).absolutePath()), "the file's folder is set up: %s", qPrintable(asked.join(' ')));
}
