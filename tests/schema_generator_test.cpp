// Generating a codeless schema library (docs/SCHEMAS.md §5.3) with the app's
// own Python and OpenUSD's usdGenSchema: a library, one that builds on it,
// the checks stopping one before usdGenSchema runs, usdGenSchema's own failure
// leaving the old files, and the trial that loads the result in a process of
// its own.
#include "python_test_app.h"
#include "test_harness.h"

#include "usdcc/scripting/python_runtime.h"
#include "usdcc/usd/schema_generator.h"
#include "usdcc/usd/schema_library.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <fstream>
#include <sstream>

using usdcc::usd::SchemaClassDef;
using usdcc::usd::SchemaGenerateResult;
using usdcc::usd::SchemaInfo;
using usdcc::usd::SchemaLibraryDef;
using usdcc::usd::SchemaPropertyDef;

namespace {
std::string readText(const QString& path) {
    std::ifstream in(path.toStdString(), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

bool has(const std::vector<std::string>& list, const char* item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

SchemaPropertyDef property(const char* name, const char* type, const char* fallback) {
    SchemaPropertyDef p;
    p.name = name;
    p.typeName = type;
    p.fallback = fallback;
    return p;
}

SchemaLibraryDef studio() {
    SchemaLibraryDef library;
    library.name = "studio";
    library.sublayers = {"usd/schema.usda", "usdGeom/schema.usda"};
    SchemaClassDef asset;
    asset.name = "StudioAsset";
    asset.base = "Xformable";
    asset.builtInAPIs = {"StudioShotAPI"};
    SchemaPropertyDef status = property("studio:status", "token", "\"wip\"");
    status.uniform = true;
    status.allowedTokens = {"wip", "final"};
    asset.properties = {status};
    SchemaClassDef shot;
    shot.name = "StudioShotAPI";
    shot.kind = SchemaInfo::Kind::SingleApplyAPI;
    shot.base = "APISchemaBase";
    shot.properties = {property("studio:shot:frameStart", "int", "1001")};
    SchemaClassDef tag;
    tag.name = "StudioTagAPI";
    tag.kind = SchemaInfo::Kind::MultipleApplyAPI;
    tag.base = "APISchemaBase";
    tag.propertyNamespacePrefix = "studioTag";
    tag.properties = {property("value", "string", "\"\"")};
    library.classes = {asset, shot, tag};
    return library;
}

SchemaLibraryDef show() {
    SchemaLibraryDef library;
    library.name = "show";
    library.sublayers = {"studio/schema.usda"};
    SchemaClassDef hero;
    hero.name = "ShowHero";
    hero.base = "StudioAsset";
    hero.properties = {property("show:heroScale", "float", "1")};
    SchemaClassDef look;
    look.name = "ShowLookAPI";
    look.kind = SchemaInfo::Kind::SingleApplyAPI;
    look.base = "APISchemaBase";
    look.canOnlyApplyTo = {"ShowHero"};
    look.properties = {property("show:look:lut", "asset", "")};
    library.classes = {hero, look};
    return library;
}
}  // namespace

USDCC_TEST(schema_generator) {
    usdcc::scripting::PythonRuntime& runtime = usdcc::test::pythonTestApp().python;
    usdcc::usd::SchemaPython python{runtime.executable(), runtime.settings().paths, {}};
    QTemporaryDir root;
    const QString studioUsd = root.filePath(QStringLiteral("studio/usd"));
    const QString showUsd = root.filePath(QStringLiteral("show/usd"));
    std::string error;

    // A library: generated, placeholders filled, tried, in place.
    CHECK(usdcc::usd::writeSchemaLibrary(studio(), usdcc::usd::schemaSourcePath(studioUsd.toStdString(), "studio"), &error),
          "writes the source: %s", error.c_str());
    SchemaGenerateResult result = usdcc::usd::generateSchemaLibrary(studioUsd.toStdString(), "studio", python);
    CHECK(result.ok, "generates: %s\n%s", result.error.c_str(), result.output.c_str());
    const QString plugInfo = QDir(studioUsd).filePath(QStringLiteral("plugInfo.json"));
    const QString generated = QDir(studioUsd).filePath(QStringLiteral("generatedSchema.usda"));
    CHECK(QFileInfo::exists(plugInfo) && QFileInfo::exists(generated), "both files in place");
    CHECK(readText(plugInfo).find("@PLUG_INFO") == std::string::npos && readText(plugInfo).find("\"Name\": \"studio\"") != std::string::npos,
          "plugInfo.json has its placeholders filled");
    CHECK(has(result.tried, "StudioAsset") && has(result.tried, "StudioShotAPI") && has(result.tried, "StudioTagAPI"),
          "every schema tried in a process of its own: [%zu]", result.tried.size());
    CHECK(result.output.find("usdInitSchema") == std::string::npos && result.output.find("####") == std::string::npos,
          "without usdGenSchema's boilerplate: [%s]", result.output.c_str());
    CHECK(!QFileInfo::exists(plugInfo + QStringLiteral(".new")) && !QFileInfo::exists(generated + QStringLiteral(".old")),
          "nothing left over");

    // One that builds on it needs it while generating (trap 5).
    CHECK(usdcc::usd::writeSchemaLibrary(show(), usdcc::usd::schemaSourcePath(showUsd.toStdString(), "show"), &error),
          "writes show's source");
    result = usdcc::usd::generateSchemaLibrary(showUsd.toStdString(), "show", python);
    CHECK(!result.ok && result.error.find("StudioAsset") != std::string::npos, "without studio, the check says so: %s",
          result.error.c_str());
    CHECK(!QFileInfo::exists(QDir(showUsd).filePath(QStringLiteral("generatedSchema.usda"))), "and nothing is written");
    result = usdcc::usd::generateSchemaLibrary(showUsd.toStdString(), "show", python, {studioUsd.toStdString()});
    CHECK(result.ok && has(result.tried, "ShowHero") && has(result.tried, "ShowLookAPI"),
          "with studio, it generates and its API applies to its type: %s", result.error.c_str());

    // usdGenSchema's own failure leaves the files as they were: Xformable's
    // file isn't among the sublayers, which the checks don't see.
    const std::string before = readText(generated);
    SchemaLibraryDef broken = studio();
    broken.sublayers = {"usd/schema.usda"};
    broken.classes[0].properties[0].fallback = "\"final\"";
    usdcc::usd::writeSchemaLibrary(broken, usdcc::usd::schemaSourcePath(studioUsd.toStdString(), "studio"), &error);
    result = usdcc::usd::generateSchemaLibrary(studioUsd.toStdString(), "studio", python);
    CHECK(!result.ok && result.error.find("Xformable") != std::string::npos, "usdGenSchema's error is said: %s",
          result.error.c_str());
    CHECK(readText(generated) == before, "and the old files stay");

    // The checks stop it before usdGenSchema runs.
    SchemaLibraryDef clash = studio();
    clash.classes[0].name = "Cube";
    clash.classes[0].builtInAPIs.clear();
    usdcc::usd::writeSchemaLibrary(clash, usdcc::usd::schemaSourcePath(studioUsd.toStdString(), "studio"), &error);
    result = usdcc::usd::generateSchemaLibrary(studioUsd.toStdString(), "studio", python);
    CHECK(!result.ok && result.error.find("usdGeom plugin has a schema of this name") != std::string::npos && result.output.empty(),
          "a name another plugin has: %s", result.error.c_str());
    CHECK(readText(generated) == before, "nothing changed");

    // No Python, or a folder that isn't the library's.
    usdcc::usd::SchemaPython nowhere = python;
    nowhere.executable = "Z:/no/python.exe";
    usdcc::usd::writeSchemaLibrary(studio(), usdcc::usd::schemaSourcePath(studioUsd.toStdString(), "studio"), &error);
    result = usdcc::usd::generateSchemaLibrary(studioUsd.toStdString(), "studio", nowhere);
    CHECK(!result.ok && result.error.find("No Python") != std::string::npos, "no Python: %s", result.error.c_str());
    QDir().mkpath(QDir(studioUsd).filePath(QStringLiteral("other")));
    QFile::copy(QString::fromStdString(usdcc::usd::schemaSourcePath(studioUsd.toStdString(), "studio")),
                QDir(studioUsd).filePath(QStringLiteral("other/schema.usda")));
    result = usdcc::usd::generateSchemaLibrary(studioUsd.toStdString(), "other", python);
    CHECK(!result.ok && result.error.find("libraryName is 'studio'") != std::string::npos, "a name that doesn't match: %s",
          result.error.c_str());

    // A library whose last schema was removed generates too: its plugin stays,
    // with no types, so the schema is gone at the next start.
    const QString lastUsd = root.filePath(QStringLiteral("last/usd"));
    SchemaLibraryDef last;
    last.name = "last";
    last.sublayers = {"usd/schema.usda"};
    SchemaClassDef only;
    only.name = "LastOnlyAPI";
    only.kind = SchemaInfo::Kind::SingleApplyAPI;
    only.base = "APISchemaBase";
    only.properties = {property("last:value", "int", "1")};
    last.classes = {only};
    usdcc::usd::writeSchemaLibrary(last, usdcc::usd::schemaSourcePath(lastUsd.toStdString(), "last"), &error);
    result = usdcc::usd::generateSchemaLibrary(lastUsd.toStdString(), "last", python);
    CHECK(result.ok && has(result.tried, "LastOnlyAPI"), "a library of one schema generates: %s", result.error.c_str());
    last.classes.clear();
    usdcc::usd::writeSchemaLibrary(last, usdcc::usd::schemaSourcePath(lastUsd.toStdString(), "last"), &error);
    result = usdcc::usd::generateSchemaLibrary(lastUsd.toStdString(), "last", python);
    CHECK(result.ok && result.tried.empty(), "emptied, it still generates: %s\n%s", result.error.c_str(), result.output.c_str());
    const std::string lastPlugInfo = readText(QDir(lastUsd).filePath(QStringLiteral("plugInfo.json")));
    CHECK(lastPlugInfo.find("LastOnlyAPI") == std::string::npos && lastPlugInfo.find("\"Name\": \"last\"") != std::string::npos,
          "its plugInfo.json keeps the plugin and drops the type: %s", lastPlugInfo.c_str());
    CHECK(readText(QDir(lastUsd).filePath(QStringLiteral("generatedSchema.usda"))).find("LastOnlyAPI") == std::string::npos,
          "and generatedSchema.usda no longer has it");
    CHECK(lastPlugInfo.rfind("# Portions of this file", 0) == 0 && lastPlugInfo.find("\"Type\": \"resource\"") != std::string::npos,
          "the rest of plugInfo.json as usdGenSchema wrote it");

    // One that never had a schema has no plugin to make.
    const QString noneUsd = root.filePath(QStringLiteral("none/usd"));
    SchemaLibraryDef none;
    none.name = "none";
    none.sublayers = {"usd/schema.usda"};
    usdcc::usd::writeSchemaLibrary(none, usdcc::usd::schemaSourcePath(noneUsd.toStdString(), "none"), &error);
    result = usdcc::usd::generateSchemaLibrary(noneUsd.toStdString(), "none", python);
    CHECK(!result.ok && result.error.find("no schemas yet") != std::string::npos, "a library with no schema says so: %s",
          result.error.c_str());
    CHECK(!QFileInfo::exists(QDir(noneUsd).filePath(QStringLiteral("plugInfo.json"))), "and writes nothing");
}
