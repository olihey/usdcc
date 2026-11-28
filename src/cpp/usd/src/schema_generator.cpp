#include "usdcc/usd/schema_generator.h"

#include "usdcc/assets/paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>

#include <utility>

namespace usdcc::usd {

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }
QString qs(const std::filesystem::path& path) { return qs(usdcc::assets::toUtf8(path)); }

struct ProcessResult {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QString output;  // stdout and stderr together
    QString stdOut;
};

// The child's environment: the app's, with OpenUSD's Python set up as the app has it.
QProcessEnvironment environmentFor(const SchemaPython& python, const std::vector<std::string>& baseLibraries) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QStringList pythonPath;
    for (const std::filesystem::path& path : python.paths) {
        pythonPath << QDir::toNativeSeparators(qs(path));
        // usdGenSchema imports jinja2 as a top-level package; usdcc ships it in _vendor.
        const std::filesystem::path vendor = path / "usdcc" / "_vendor";
        std::error_code error;
        if (std::filesystem::is_directory(vendor / "jinja2", error)) pythonPath << QDir::toNativeSeparators(qs(vendor));
    }
    environment.insert(QStringLiteral("PYTHONPATH"), pythonPath.join(QDir::listSeparator()));
    environment.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.remove(QStringLiteral("PYTHONHOME"));
    QStringList path;
    for (const std::filesystem::path& folder : python.dllFolders) path << QDir::toNativeSeparators(qs(folder));
    if (const QString existing = environment.value(QStringLiteral("PATH")); !existing.isEmpty()) path << existing;
    environment.insert(QStringLiteral("PATH"), path.join(QDir::listSeparator()));
    QStringList plugins;
    if (const QString existing = environment.value(QStringLiteral("PXR_PLUGINPATH_NAME")); !existing.isEmpty()) {
        plugins << existing;
    }
    for (const std::string& folder : baseLibraries) plugins << QDir::toNativeSeparators(qs(folder));
    if (plugins.isEmpty()) {
        environment.remove(QStringLiteral("PXR_PLUGINPATH_NAME"));
    } else {
        environment.insert(QStringLiteral("PXR_PLUGINPATH_NAME"), plugins.join(QDir::listSeparator()));
    }
    return environment;
}

ProcessResult run(const SchemaPython& python, const QStringList& arguments, const QString& workingDirectory,
                  const QProcessEnvironment& environment, int timeoutMs) {
    ProcessResult result;
    QProcess process;
    process.setProgram(qs(python.executable));
    process.setArguments(arguments);
    process.setWorkingDirectory(workingDirectory);
    process.setProcessEnvironment(environment);
    process.start();
    result.started = process.waitForStarted(30000);
    if (!result.started) {
        result.output = process.errorString();
        return result;
    }
    result.finished = process.waitForFinished(timeoutMs);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
    }
    result.stdOut = QString::fromUtf8(process.readAllStandardOutput());
    result.output = result.stdOut + QString::fromUtf8(process.readAllStandardError());
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return result;
}

// usdGenSchema's output without what it always says (docs/SCHEMAS.md §2, trap 6):
// the banner about USD_DISABLE_PRIM_DEFINITIONS_FOR_USDGENSCHEMA and the files a
// C++ library would need.
QString withoutBoilerplate(const QString& output) {
    QStringList lines;
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;
        if (trimmed.contains(QLatin1String("Run usdInitSchema to set up build necessities"))) continue;
        lines << line.trimmed();
    }
    return lines.join(QLatin1Char('\n'));
}

// Puts `from` at `to`, keeping the old file until the new one is there; on
// failure everything is as it was.
bool replaceFiles(const std::vector<std::pair<QString, QString>>& files, QString* error) {
    std::vector<std::pair<QString, QString>> done;  // (target, its backup or empty)
    auto rollBack = [&done]() {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            QFile::remove(it->first);
            if (!it->second.isEmpty()) QFile::rename(it->second, it->first);
        }
    };
    for (const auto& [from, to] : files) {
        const QString staged = to + QStringLiteral(".new");
        const QString backup = to + QStringLiteral(".old");
        QFile::remove(staged);
        QFile::remove(backup);
        if (!QFile::copy(from, staged)) {
            *error = QStringLiteral("Can't write %1").arg(staged);
            rollBack();
            return false;
        }
        const bool existed = QFileInfo::exists(to);
        if (existed && !QFile::rename(to, backup)) {
            QFile::remove(staged);
            *error = QStringLiteral("Can't replace %1: is it open somewhere?").arg(to);
            rollBack();
            return false;
        }
        if (!QFile::rename(staged, to)) {
            if (existed) QFile::rename(backup, to);
            QFile::remove(staged);
            *error = QStringLiteral("Can't write %1").arg(to);
            rollBack();
            return false;
        }
        done.emplace_back(to, existed ? backup : QString());
    }
    for (const auto& [target, backup] : done) {
        if (!backup.isEmpty()) QFile::remove(backup);
    }
    return true;
}

// Takes the types usdGenSchema wrote (autoGenerated) out of plugin `library`'s
// entry in a plugInfo.json, as usdGenSchema itself does before writing a
// library's types anew; the comment lines at its top are kept.
bool dropGeneratedTypes(const QString& path, const std::string& library, std::string* error) {
    auto fail = [error, &path](const char* why) {
        if (error) *error = std::string(why) + ": " + path.toStdString();
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail("It can't be read");
    QByteArray comments, json;
    for (const QByteArray& line : file.readAll().split('\n')) {
        (line.trimmed().startsWith('#') ? comments : json) += line + '\n';
    }
    file.close();
    QJsonParseError parse;
    QJsonObject info = QJsonDocument::fromJson(json, &parse).object();
    if (parse.error != QJsonParseError::NoError) return fail("It isn't JSON");
    auto withoutGenerated = [](QJsonObject owner) {
        QJsonObject types = owner.value(QStringLiteral("Types")).toObject();
        for (const QString& name : types.keys()) {
            if (types.value(name).toObject().value(QStringLiteral("autoGenerated")).toBool()) types.remove(name);
        }
        owner.insert(QStringLiteral("Types"), types);
        return owner;
    };
    if (info.contains(QStringLiteral("Plugins"))) {
        QJsonArray plugins = info.value(QStringLiteral("Plugins")).toArray();
        for (int i = 0; i < plugins.size(); ++i) {
            QJsonObject plugin = plugins.at(i).toObject();
            if (plugin.value(QStringLiteral("Name")).toString().toStdString() != library) continue;
            plugin.insert(QStringLiteral("Info"), withoutGenerated(plugin.value(QStringLiteral("Info")).toObject()));
            plugins.replace(i, plugin);
        }
        info.insert(QStringLiteral("Plugins"), plugins);
    } else {
        info = withoutGenerated(info);
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail("It can't be written");
    file.write(comments + QJsonDocument(info).toJson(QJsonDocument::Indented));
    return file.error() == QFileDevice::NoError ? true : fail("It can't be written");
}

}  // namespace

SchemaGenerateResult generateSchemaLibrary(const std::string& usdFolder, const std::string& library,
                                           const SchemaPython& python, const std::vector<std::string>& baseLibraries,
                                           int timeoutMs) {
    SchemaGenerateResult result;
    auto fail = [&result](const std::string& why) {
        result.error = why;
        return result;
    };

    // 1. The source, and the checks USD makes too late or not at all.
    const std::string source = schemaSourcePath(usdFolder, library);
    SchemaLibraryDef definition;
    std::string error;
    if (!readSchemaLibrary(source, &definition, &error)) return fail(error);
    if (definition.name != library) {
        return fail("Its libraryName is '" + definition.name + "', not '" + library + "', the folder it's in");
    }
    std::vector<SchemaLibraryDef> bases;
    for (const std::string& folder : baseLibraries) {
        SchemaLibraryDef base;
        if (!findSchemaLibrary(folder, &base, &error)) return fail("A library it builds on: " + error);
        bases.push_back(std::move(base));
    }
    result.issues = checkSchemaLibrary(definition, bases);
    if (hasErrors(result.issues)) {
        for (const SchemaIssue& issue : result.issues) {
            if (issue.severity == SchemaIssue::Severity::Error) return fail(issueText(issue));
        }
    }
    std::error_code ec;
    if (python.executable.empty() || !std::filesystem::exists(python.executable, ec)) {
        return fail("No Python to run usdGenSchema with: " + usdcc::assets::toUtf8(python.executable));
    }

    // usdGenSchema writes no plugInfo.json for a library with no schemas: without
    // one from before, there is no plugin to make.
    const QString currentPlugInfo = qs(plugInfoPath(usdFolder));
    if (definition.classes.empty() && !QFileInfo::exists(currentPlugInfo)) {
        return fail("It has no schemas yet: there's nothing to generate");
    }

    // 2. usdGenSchema, into a folder of its own. It keeps what plugInfo.json has
    // beyond its own types, so the current one goes there first.
    QTemporaryDir work;
    if (!work.isValid()) return fail("No temporary folder: " + work.errorString().toStdString());
    const QString workPath = work.path();
    if (QFileInfo::exists(currentPlugInfo)) QFile::copy(currentPlugInfo, work.filePath(QStringLiteral("plugInfo.json")));
    const QProcessEnvironment environment = environmentFor(python, baseLibraries);
    // usdGenSchema sets this itself, but run as a module, importing pxr.Usd first
    // has OpenUSD read it before then; a library that builds on another then
    // fails with "list index out of range". Not for the trial below, which needs
    // the prim definitions this turns off.
    QProcessEnvironment generateEnvironment = environment;
    generateEnvironment.insert(QStringLiteral("USD_DISABLE_PRIM_DEFINITIONS_FOR_USDGENSCHEMA"), QStringLiteral("1"));
    const ProcessResult generated =
        run(python, {QStringLiteral("-m"), QStringLiteral("pxr.Usd.usdGenSchema"), QStringLiteral("-q"),
                     QDir::toNativeSeparators(qs(source)), QDir::toNativeSeparators(workPath)},
            workPath, generateEnvironment, timeoutMs);
    result.output = withoutBoilerplate(generated.output).toStdString();
    if (!generated.started) return fail("Python didn't start: " + generated.output.toStdString());
    if (!generated.finished) return fail("usdGenSchema took longer than " + std::to_string(timeoutMs / 1000) + " s");
    const QString newPlugInfo = work.filePath(QStringLiteral("plugInfo.json"));
    const QString newSchema = work.filePath(QStringLiteral("generatedSchema.usda"));
    if (generated.exitCode != 0 || !QFileInfo::exists(newPlugInfo) || !QFileInfo::exists(newSchema)) {
        return fail(result.output.empty() ? "usdGenSchema failed (exit " + std::to_string(generated.exitCode) + ")"
                                          : result.output);
    }

    // With no schema left, usdGenSchema leaves plugInfo.json as it was, the removed
    // schemas' types still in it: they would load again at the next start.
    if (definition.classes.empty() && !dropGeneratedTypes(newPlugInfo, library, &error)) return fail(error);

    // 3. What OpenUSD's CMake would fill in (trap 2).
    if (!fillPlugInfoPlaceholders(newPlugInfo.toStdString(), &error)) return fail(error);

    // 4. Try it where a failure can't hurt: a process of its own.
    const ProcessResult tried = run(python, {QStringLiteral("-m"), QStringLiteral("usdcc._schema_check"),
                                             QDir::toNativeSeparators(workPath)},
                                    workPath, environment, timeoutMs);
    if (!tried.finished || tried.exitCode != 0) {
        return fail("Trying it failed: " + withoutBoilerplate(tried.output).toStdString());
    }
    QJsonObject report;
    const QStringList lines = tried.stdOut.trimmed().split(QLatin1Char('\n'));
    if (!lines.isEmpty()) report = QJsonDocument::fromJson(lines.last().trimmed().toUtf8()).object();
    if (!report.contains(QStringLiteral("errors"))) {
        return fail("Trying it said nothing it could read: " + tried.output.toStdString());
    }
    for (const QJsonValue& name : report.value(QStringLiteral("schemas")).toArray()) result.tried.push_back(name.toString().toStdString());
    const QJsonArray errors = report.value(QStringLiteral("errors")).toArray();
    if (!errors.isEmpty()) {
        QStringList messages;
        for (const QJsonValue& message : errors) messages << message.toString();
        return fail(messages.join(QLatin1Char('\n')).toStdString());
    }

    // 5. In place, both or neither.
    QString replaceError;
    if (!replaceFiles({{newPlugInfo, currentPlugInfo}, {newSchema, qs(generatedSchemaPath(usdFolder))}}, &replaceError)) {
        return fail(replaceError.toStdString());
    }
    result.ok = true;
    return result;
}

}  // namespace usdcc::usd
