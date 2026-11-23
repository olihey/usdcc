#include "usdcc/scripting/editor_setup.h"

#include "usdcc/core/log.h"
#include "usdcc/scripting/plugin_manager.h"
#include "usdcc/scripting/python_runtime.h"
#include "usdcc/scripting/startup_scripts.h"
#include "usdcc/ui/settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <optional>

namespace usdcc::scripting::editor_setup {

namespace {
QString qs(const std::filesystem::path& path) { return QString::fromStdU16String(path.generic_u16string()); }

QString typesFolder() { return usdcc::ui::userDataFolder() + QStringLiteral("/types"); }

// A deploy's stubs: python/stubs beside the Python on Windows, lib/../python/stubs elsewhere.
QString deployedStubs() { return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../python/stubs")); }

QString buildStubs() {
#ifdef USDCC_STUBS_DIR
    return QStringLiteral(USDCC_STUBS_DIR);
#else
    return {};
#endif
}

bool sameContent(const QString& a, const QString& b) {
    QFile first(a), second(b);
    return first.open(QIODevice::ReadOnly) && second.open(QIODevice::ReadOnly) && first.size() == second.size() && first.readAll() == second.readAll();
}

// Brings `target` to `source`'s files of these names, in place: an editor may be watching
// the folder, so it can't be replaced. `kept` gets what belongs there.
bool mirror(const QString& source, const QString& target, const QStringList& names, QSet<QString>* kept) {
    QDirIterator it(source, names, QDir::Files, QDirIterator::Subdirectories);
    bool ok = true;
    while (it.hasNext()) {
        const QString file = it.next();
        const QString relative = QDir(source).relativeFilePath(file);
        if (relative.contains(QLatin1String("__pycache__"))) continue;
        const QString to = target + QLatin1Char('/') + relative;
        kept->insert(QDir::cleanPath(to));
        if (sameContent(file, to)) continue;
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile in(file);
        QSaveFile out(to);
        ok = in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly) && out.write(in.readAll()) >= 0 && out.commit() && ok;
    }
    return ok;
}

// A JSON object's file: an empty one when there's none, and no value when it isn't one.
std::optional<QJsonObject> readObject(const QString& file) {
    QFile in(file);
    if (!in.exists()) return QJsonObject();
    if (!in.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray text = in.readAll();
    if (text.trimmed().isEmpty()) return QJsonObject();
    const QJsonDocument document = QJsonDocument::fromJson(text);
    if (!document.isObject()) return std::nullopt;
    return document.object();
}

bool writeObject(const QString& file, const QJsonObject& object) {
    QSaveFile out(file);
    return out.open(QIODevice::WriteOnly) && out.write(QJsonDocument(object).toJson()) >= 0 && out.commit();
}
}  // namespace

bool updateTypes(const PythonRuntime& python, QString* error) {
    if (!python.settings().bundled) return true;
    const QString target = typesFolder();
    QSet<QString> kept;
    bool ok = QFileInfo(deployedStubs()).isDir();
    if (!ok) {
        if (error) *error = QCoreApplication::translate("EditorSetup", "this usdcc has no types for editors (%1)").arg(deployedStubs());
        return false;
    }
    ok = mirror(deployedStubs(), target, {QStringLiteral("*.pyi")}, &kept);
    for (const std::filesystem::path& path : python.settings().paths) {
        const QString package = qs(path / "usdcc");
        if (QFileInfo::exists(package + QStringLiteral("/__init__.py"))) {
            ok = mirror(package, target + QStringLiteral("/usdcc"), {QStringLiteral("*.py")}, &kept) && ok;
            break;
        }
    }
    QStringList stale;
    QDirIterator it(target, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        if (!kept.contains(QDir::cleanPath(file))) stale << file;
    }
    for (const QString& file : stale) QFile::remove(file);
    if (!ok && error) *error = QCoreApplication::translate("EditorSetup", "the types couldn't all be written to %1").arg(QDir::toNativeSeparators(target));
    return ok;
}

void refreshTypes(const PythonRuntime& python) {
    if (!python.settings().bundled || !QFileInfo(typesFolder()).isDir()) return;
    QString error;
    if (!updateTypes(python, &error)) USDCC_LOG_WARNING("usdcc.editor", "Completion in editors may be out of date: {}", error.toStdString());
}

QStringList paths(const PythonRuntime& python, const PluginManager& plugins) {
    QStringList list;
    if (python.settings().bundled) {
        list << typesFolder();
    } else {
        // The stubs first: an editor takes a module from the first folder that has it.
        if (!buildStubs().isEmpty()) list << buildStubs();
        for (const auto& path : python.settings().paths) list << qs(path);
    }
    for (const auto& path : plugins.packageFoldersInUse()) list << qs(path);
    list << qs(userScriptsFolder());
    list.removeDuplicates();
    return list;
}

QString interpreter(const PythonRuntime& python) {
    return python.settings().bundled || !python.isAvailable() ? QString() : qs(python.executable());
}

bool setUpFolder(const QString& folder, const QStringList& paths, const QString& interpreter, int debugPort, QString* error) {
    const QString settingsFile = folder + QStringLiteral("/.vscode/settings.json");
    const QString launchFile = folder + QStringLiteral("/.vscode/launch.json");
    std::optional<QJsonObject> settings = readObject(settingsFile);
    std::optional<QJsonObject> launch = readObject(launchFile);
    if (!settings || !launch) {
        if (error) {
            *error = QCoreApplication::translate("EditorSetup", "%1 isn't plain JSON (it may have comments), so it's left as it is.\n\n"
                                                                "The extension's \"usdcc: Set Up Workspace\" can change it.")
                         .arg(QDir::toNativeSeparators(settings ? launchFile : settingsFile));
        }
        return false;
    }

    // Ours first, then the user's own.
    QJsonArray extra;
    QSet<QString> seen;
    const auto add = [&](const QString& path) {
        if (!path.isEmpty() && !seen.contains(path)) {
            seen.insert(path);
            extra.append(path);
        }
    };
    for (const QString& path : paths) add(path);
    for (const QJsonValue& path : settings->value(QStringLiteral("python.analysis.extraPaths")).toArray()) add(path.toString());
    settings->insert(QStringLiteral("python.analysis.extraPaths"), extra);
    if (!interpreter.isEmpty() && !settings->contains(QStringLiteral("python.defaultInterpreterPath"))) {
        settings->insert(QStringLiteral("python.defaultInterpreterPath"), interpreter);
    }

    const QString name = QStringLiteral("Attach to usdcc");
    QJsonArray configurations = launch->value(QStringLiteral("configurations")).toArray();
    bool there = false;
    for (const QJsonValue& each : configurations) there = there || each.toObject().value(QStringLiteral("name")).toString() == name;
    if (!there) {
        configurations.append(QJsonObject{{"name", name},
                                          {"type", "debugpy"},
                                          {"request", "attach"},
                                          {"connect", QJsonObject{{"host", "127.0.0.1"}, {"port", debugPort}}},
                                          {"justMyCode", true}});
        launch->insert(QStringLiteral("configurations"), configurations);
        if (!launch->contains(QStringLiteral("version"))) launch->insert(QStringLiteral("version"), QStringLiteral("0.2.0"));
    }

    if (!QDir().mkpath(folder + QStringLiteral("/.vscode")) || !writeObject(settingsFile, *settings) || (!there && !writeObject(launchFile, *launch))) {
        if (error) *error = QCoreApplication::translate("EditorSetup", "%1 can't be written to.").arg(QDir::toNativeSeparators(folder + "/.vscode"));
        return false;
    }
    return true;
}

QString findVsCode() {
#if defined(_WIN32)
    // code.cmd is a script; its program is beside its folder.
    QStringList commands{QStandardPaths::findExecutable(QStringLiteral("code.cmd"))};
    for (const char* variable : {"LOCALAPPDATA", "ProgramFiles"}) {
        const QString root = qEnvironmentVariable(variable);
        if (root.isEmpty()) continue;
        commands << root + QLatin1String(qstrcmp(variable, "LOCALAPPDATA") == 0 ? "/Programs" : "") + QStringLiteral("/Microsoft VS Code/bin/code.cmd");
    }
    for (const QString& command : commands) {
        if (command.isEmpty()) continue;
        const QString program = QDir::cleanPath(QFileInfo(command).absolutePath() + QStringLiteral("/../Code.exe"));
        if (QFileInfo(program).isExecutable()) return program;
    }
    return {};
#else
    return QStandardPaths::findExecutable(QStringLiteral("code"));
#endif
}

bool openInVsCode(const QString& folder) {
    const QString program = findVsCode();
    return !program.isEmpty() && QProcess::startDetached(program, {QDir::toNativeSeparators(folder)});
}

}  // namespace usdcc::scripting::editor_setup
