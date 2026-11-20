// A codeless schema library's schema.usda read and written through Sdf, its
// fallbacks parsed, the checks before Generate, and plugInfo.json's
// placeholders (docs/SCHEMAS.md §5).
#include "test_harness.h"

#include "usdcc/usd/schema_library.h"

#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/schema.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <fstream>
#include <sstream>

using usdcc::usd::SchemaClassDef;
using usdcc::usd::SchemaInfo;
using usdcc::usd::SchemaIssue;
using usdcc::usd::SchemaLibraryDef;
using usdcc::usd::SchemaPropertyDef;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const std::string kTestLibrary = std::string(USDCC_TEST_DATA_DIR) + "/schemas/usdccTestSchemas/usdccTestSchemas/schema.usda";

const SchemaClassDef* findClass(const SchemaLibraryDef& library, const std::string& name) {
    const auto it = std::find_if(library.classes.begin(), library.classes.end(), [&](const SchemaClassDef& c) { return c.name == name; });
    return it == library.classes.end() ? nullptr : &*it;
}

const SchemaPropertyDef* findProperty(const SchemaClassDef* c, const std::string& name) {
    if (!c) return nullptr;
    const auto it = std::find_if(c->properties.begin(), c->properties.end(), [&](const SchemaPropertyDef& p) { return p.name == name; });
    return it == c->properties.end() ? nullptr : &*it;
}

// The first issue about `schema`/`property` whose message has `text`, or null.
const SchemaIssue* issueAbout(const std::vector<SchemaIssue>& issues, const std::string& schema, const std::string& property,
                              const std::string& text) {
    for (const SchemaIssue& issue : issues) {
        if (issue.schema == schema && issue.property == property && issue.message.find(text) != std::string::npos) return &issue;
    }
    return nullptr;
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// A small library that passes every check.
SchemaLibraryDef studio() {
    SchemaLibraryDef library;
    library.name = "studio";
    library.sublayers = {"usd/schema.usda", "usdGeom/schema.usda"};
    SchemaClassDef asset;
    asset.name = "StudioAsset";
    asset.kind = SchemaInfo::Kind::ConcreteTyped;
    asset.base = "Xformable";
    asset.documentation = "A studio asset.";
    asset.builtInAPIs = {"StudioShotAPI"};
    SchemaPropertyDef status;
    status.name = "studio:status";
    status.typeName = "token";
    status.uniform = true;
    status.fallback = "\"wip\"";
    status.allowedTokens = {"wip", "final"};
    asset.properties.push_back(status);
    SchemaClassDef shot;
    shot.name = "StudioShotAPI";
    shot.kind = SchemaInfo::Kind::SingleApplyAPI;
    shot.base = "APISchemaBase";
    SchemaPropertyDef frame;
    frame.name = "studio:shot:frameStart";
    frame.typeName = "int";
    frame.fallback = "1001";
    SchemaPropertyDef camera;
    camera.name = "studio:shot:camera";
    camera.relationship = true;
    camera.documentation = "Its camera.";
    shot.properties = {camera, frame};  // by name, as they read back
    library.classes = {asset, shot};
    return library;
}
}  // namespace

USDCC_TEST(schema_library_read) {
    SchemaLibraryDef library;
    std::string error;
    CHECK(usdcc::usd::readSchemaLibrary(kTestLibrary, &library, &error), "reads: %s", error.c_str());
    CHECK(library.name == "usdccTestSchemas" && library.prefix.empty(), "its name");
    CHECK(library.sublayers == std::vector<std::string>({"usd/schema.usda", "usdGeom/schema.usda"}), "what it builds on");
    CHECK(library.classes.size() == 5, "every class: [%zu]", library.classes.size());

    const SchemaClassDef* base = findClass(library, "UsdccTestBase");
    CHECK(base && base->kind == SchemaInfo::Kind::AbstractTyped && base->base == "Xformable", "an abstract type");
    const SchemaPropertyDef* id = findProperty(base, "usdccTest:assetId");
    CHECK(id && id->typeName == "string" && id->fallback == "\"\"" && id->displayName == "Asset ID" &&
              id->displayGroup == "Tracking" && id->documentation == "Where the asset is tracked.",
          "a property, its display name and group");
    const SchemaClassDef* asset = findClass(library, "UsdccTestAsset");
    CHECK(asset && asset->kind == SchemaInfo::Kind::ConcreteTyped && asset->base == "UsdccTestBase" &&
              asset->builtInAPIs == std::vector<std::string>({"UsdccTestShotAPI"}),
          "a prim type with a built-in API");
    const SchemaPropertyDef* status = findProperty(asset, "usdccTest:status");
    CHECK(status && status->uniform && status->fallback == "\"wip\"" &&
              status->allowedTokens == std::vector<std::string>({"wip", "review", "final"}),
          "uniform, its fallback quoted, allowed tokens");
    const SchemaClassDef* shot = findClass(library, "UsdccTestShotAPI");
    CHECK(shot && shot->kind == SchemaInfo::Kind::SingleApplyAPI && shot->base == "APISchemaBase", "a single-apply API");
    const SchemaPropertyDef* camera = findProperty(shot, "usdccTest:shot:camera");
    CHECK(camera && camera->relationship && camera->documentation == "The shot's camera.", "a relationship");
    const SchemaClassDef* tag = findClass(library, "UsdccTestTagAPI");
    CHECK(tag && tag->kind == SchemaInfo::Kind::MultipleApplyAPI && tag->propertyNamespacePrefix == "usdccTestTag" &&
              findProperty(tag, "value"),
          "a multiple-apply API, its prefix, its property as written");
    const SchemaClassDef* mark = findClass(library, "UsdccTestMarkAPI");
    CHECK(mark && mark->autoApplyTo == std::vector<std::string>({"UsdccTestAsset"}) &&
              mark->canOnlyApplyTo == std::vector<std::string>({"UsdccTestAsset"}),
          "auto-apply and can-only-apply-to");

    CHECK(!usdcc::usd::readSchemaLibrary(std::string(USDCC_TEST_DATA_DIR) + "/cube_and_sphere.usda", &library, &error) &&
              error.find("GLOBAL") != std::string::npos,
          "a file that isn't one says so: %s", error.c_str());
    CHECK(!usdcc::usd::readSchemaLibrary("Z:/no/such/schema.usda", &library, &error), "no file");
}

USDCC_TEST(schema_library_write) {
    QTemporaryDir dir;
    const std::string path = dir.filePath(QStringLiteral("usd/usdccTestSchemas/schema.usda")).toStdString();
    QDir().mkpath(dir.filePath(QStringLiteral("usd/usdccTestSchemas")));
    QFile::copy(QString::fromStdString(kTestLibrary), QString::fromStdString(path));
    QFile(QString::fromStdString(path)).setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    SchemaLibraryDef library;
    std::string error;
    CHECK(usdcc::usd::readSchemaLibrary(path, &library, &error), "reads the copy");
    CHECK(usdcc::usd::writeSchemaLibrary(library, path, &error), "writes it back: %s", error.c_str());
    SchemaLibraryDef again;
    CHECK(usdcc::usd::readSchemaLibrary(path, &again, &error) && again == library, "unchanged by the round trip");
    const std::string text = readText(path);
    CHECK(text.find("A codeless schema library for usdcc's tests") != std::string::npos,
          "the layer's own comment is kept");
    CHECK(text.find("skipCodeGeneration = 1") != std::string::npos || text.find("skipCodeGeneration = true") != std::string::npos,
          "it stays codeless");

    // What the definition doesn't cover is kept.
    {
        const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(path);
        layer->GetPrimAtPath(SdfPath("/UsdccTestAsset"))->SetCustomData("studioNote", VtValue(std::string("keep me")));
        layer->Save();
    }
    // Edit: a property added and one removed, a fallback changed, a class removed.
    SchemaClassDef* asset = nullptr;
    for (SchemaClassDef& c : library.classes) {
        if (c.name == "UsdccTestAsset") asset = &c;
    }
    SchemaPropertyDef scale;
    scale.name = "usdccTest:scale";
    scale.typeName = "float3";
    scale.fallback = "(1, 2, 3)";
    for (SchemaPropertyDef& p : asset->properties) {
        if (p.name == "usdccTest:status") p.fallback = "\"final\"";
    }
    asset->properties.push_back(scale);
    std::sort(asset->properties.begin(), asset->properties.end(),
              [](const SchemaPropertyDef& a, const SchemaPropertyDef& b) { return a.name < b.name; });
    library.classes.erase(std::remove_if(library.classes.begin(), library.classes.end(),
                                         [](const SchemaClassDef& c) { return c.name == "UsdccTestMarkAPI"; }),
                          library.classes.end());
    CHECK(usdcc::usd::writeSchemaLibrary(library, path, &error), "writes the edits: %s", error.c_str());
    CHECK(usdcc::usd::readSchemaLibrary(path, &again, &error) && again == library, "they read back as written");
    const SdfLayerRefPtr layer = SdfLayer::OpenAsAnonymous(path);
    const SdfPrimSpecHandle edited = layer->GetPrimAtPath(SdfPath("/UsdccTestAsset"));
    const VtValue note = edited ? edited->GetInfo(SdfFieldKeys->CustomData) : VtValue();
    CHECK(note.IsHolding<VtDictionary>() && note.UncheckedGet<VtDictionary>().count("studioNote"),
          "a class's own customData is kept");
    CHECK(!layer->GetPrimAtPath(SdfPath("/UsdccTestMarkAPI")), "a class it no longer lists is gone");

    // A bad fallback is refused, and nothing written.
    const std::string before = readText(path);
    for (SchemaPropertyDef& p : asset->properties) {
        if (p.name == "usdccTest:scale") p.fallback = "(1, 2";
    }
    CHECK(!usdcc::usd::writeSchemaLibrary(library, path, &error) && error.find("usdccTest:scale") != std::string::npos,
          "a fallback that doesn't parse is refused: %s", error.c_str());
    CHECK(readText(path) == before, "and the file is as it was");

    // A new library from nothing.
    const std::string fresh = dir.filePath(QStringLiteral("fresh/studio/schema.usda")).toStdString();
    CHECK(usdcc::usd::writeSchemaLibrary(studio(), fresh, &error), "a new one: %s", error.c_str());
    CHECK(usdcc::usd::readSchemaLibrary(fresh, &again, &error) && again == studio(), "reads back as written");
}

USDCC_TEST(schema_library_fallbacks) {
    VtValue value;
    std::string error;
    CHECK(usdcc::usd::parseFallback("int", "1001", &value, &error) && value == VtValue(1001), "an int");
    CHECK(usdcc::usd::parseFallback("token", "\"wip\"", &value, &error) && value == VtValue(TfToken("wip")), "a token");
    CHECK(usdcc::usd::parseFallback("float3", "(0, 1, 2)", &value, &error) && usdcc::usd::usdaValueText(value) == "(0, 1, 2)",
          "a vector, and back");
    CHECK(usdcc::usd::parseFallback("token[]", "[\"a\", \"b\"]", &value, &error) &&
              usdcc::usd::usdaValueText(value) == "[\"a\", \"b\"]",
          "a token array, quoted back");
    CHECK(usdcc::usd::parseFallback("bool", "true", &value, &error) && usdcc::usd::usdaValueText(value) == "true", "a bool");
    CHECK(!usdcc::usd::parseFallback("int", "x", &value, &error) && error.find("'x' isn't a int") == 0, "refused: %s",
          error.c_str());
    CHECK(!usdcc::usd::parseFallback("nonsense", "1", &value, &error) && error.find("no value type") != std::string::npos,
          "an unknown type");
}

USDCC_TEST(schema_library_checks) {
    CHECK(!usdcc::usd::hasErrors(usdcc::usd::checkSchemaLibrary(studio())), "a sound library has no errors");
    CHECK(usdcc::usd::libraryTypePrefix(studio()) == "Studio", "usdGenSchema's prefix: the name, capitalized");

    SchemaLibraryDef library = studio();
    library.classes[0].name = "Cube";  // trap 3
    std::vector<SchemaIssue> issues = usdcc::usd::checkSchemaLibrary(library);
    CHECK(issueAbout(issues, "Cube", "", "usdGeom plugin has a schema of this name"), "a name another plugin has");

    library = studio();
    library.classes[1].name = "StudioShot";
    library.classes[0].builtInAPIs = {"StudioShot"};
    issues = usdcc::usd::checkSchemaLibrary(library);
    CHECK(issueAbout(issues, "StudioShot", "", "ends in API"), "an API's name");

    library = studio();
    library.classes[0].properties[0].fallback = "\"done\"";
    issues = usdcc::usd::checkSchemaLibrary(library);
    CHECK(issueAbout(issues, "StudioAsset", "studio:status", "isn't one of its allowed tokens"), "a fallback not allowed");

    library = studio();
    SchemaPropertyDef visibility;
    visibility.name = "visibility";
    visibility.typeName = "int";
    library.classes[0].properties.push_back(visibility);  // trap 4
    issues = usdcc::usd::checkSchemaLibrary(library);
    const SchemaIssue* replaced = issueAbout(issues, "StudioAsset", "visibility", "token in Imageable");
    CHECK(replaced && replaced->severity == SchemaIssue::Severity::Warning, "an inherited property's type replaced: a warning");
    CHECK(issueAbout(issues, "StudioAsset", "visibility", "no namespace"), "no namespace: a warning");
    CHECK(!usdcc::usd::hasErrors(issues), "warnings only");

    library = studio();
    library.classes[0].base = "NoSuchBase";
    CHECK(issueAbout(usdcc::usd::checkSchemaLibrary(library), "StudioAsset", "", "isn't a prim type"), "an unknown base");

    library = studio();
    library.classes[1].kind = SchemaInfo::Kind::MultipleApplyAPI;
    CHECK(issueAbout(usdcc::usd::checkSchemaLibrary(library), "StudioShotAPI", "", "namespace prefix"),
          "a multiple-apply API with properties needs a prefix");

    library = studio();
    library.classes[0].builtInAPIs = {"NoSuchAPI"};
    CHECK(issueAbout(usdcc::usd::checkSchemaLibrary(library), "StudioAsset", "", "builds in NoSuchAPI"), "an unknown built-in API");

    library = studio();
    library.classes[1].properties[1].typeName = "integer";  // frameStart, after camera
    CHECK(issueAbout(usdcc::usd::checkSchemaLibrary(library), "StudioShotAPI", "studio:shot:frameStart", "No value type"),
          "an unknown value type");
}

USDCC_TEST(schema_library_plug_info) {
    QTemporaryDir dir;
    const std::string path = dir.filePath(QStringLiteral("plugInfo.json")).toStdString();
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"Plugins": [{"LibraryPath": "@PLUG_INFO_LIBRARY_PATH@", "ResourcePath": "@PLUG_INFO_RESOURCE_PATH@", "Root": "@PLUG_INFO_ROOT@"}]})";
    }
    std::string error;
    CHECK(usdcc::usd::fillPlugInfoPlaceholders(path, &error), "fills them: %s", error.c_str());
    CHECK(readText(path) == R"({"Plugins": [{"LibraryPath": "", "ResourcePath": ".", "Root": "."}]})", "as a codeless plugin needs: %s",
          readText(path).c_str());
}
