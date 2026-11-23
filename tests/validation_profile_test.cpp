// Validation profiles and checks (docs/VALIDATION.md §5.3, §6): precedence, what a file keeps,
// the library, each kind of check finding and passing, severities, and ignored findings. Runs in
// usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/validation_profile.h"
#include "usdcc/usd/validation_run.h"

#include <pxr/base/js/json.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>

#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::usd::validation;

namespace {
Profile parsed(const std::string& text) {
    std::string error;
    const auto profile = Profile::parse(text, &error);
    CHECK(profile, "parses: %s", error.c_str());
    return profile.value_or(Profile());
}

Rule rule(const std::string& name, std::vector<std::string> keywords = {}) {
    Rule result;
    result.name = name;
    result.registered = name;
    result.keywords = std::move(keywords);
    return result;
}

CheckSettings checkOf(const std::string& templateId, const std::string& values) {
    CheckSettings settings;
    settings.id = "t";
    settings.templateId = templateId;
    const JsValue json = JsParseString(values);
    if (json.IsObject()) settings.values = json.GetJsObject();
    return settings;
}

// The messages a check has for the stage.
std::vector<Finding> found(const UsdStageRefPtr& stage, const CheckSettings& settings) {
    RunSettings run;
    run.checks.push_back(settings);
    ValidationRun validation(stage, run);
    validation.runAll();
    return validation.findings();
}

std::string paths(const std::vector<Finding>& findings) {
    std::string result;
    for (const Finding& finding : findings) result += (result.empty() ? "" : " ") + finding.sites.at(0).path.GetString();
    return result;
}

std::string refusal(const CheckSettings& settings) {
    std::string error;
    return Check::make(settings, &error) ? std::string() : error;
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
}  // namespace

USDCC_TEST(validation_profile_precedence) {
    const Profile profile = parsed(R"({"name": "P", "rules": {
        "*": {"severity": "info"},
        "keyword:a": {"enabled": false, "severity": "warning"},
        "keyword:b": {"enabled": true, "severity": "error"},
        "group:studio": {"severity": "warning", "parameters": {"x": 1, "y": 2}},
        "studio.one": {"enabled": true, "parameters": {"x": 5}},
        "gone.rule": {"enabled": false}}})");
    const RuleSettings one = profile.settingsFor(rule("studio.one", {"a"}));
    CHECK(one.enabled && *one.enabled, "a rule's own entry wins over a keyword's");
    CHECK(one.severity == Severity::Warning, "the group's over the keyword's and the default's");
    CHECK(one.parameters.size() == 2 && one.parameters.at("x").GetInt64() == 5 && one.parameters.at("y").GetInt64() == 2,
          "parameters one by one");
    CHECK(!profile.runs(rule("other.r", {"a"})), "a keyword turns rules off");
    const RuleSettings both = profile.settingsFor(rule("other.r", {"b", "a"}));
    CHECK(both.enabled == false && both.severity == Severity::Error, "of two keywords, off and the most severe win");
    CHECK(profile.settingsFor(rule("plain")).severity == Severity::Info && profile.runs(rule("plain")),
          "everything else: on, with the default severity");
    CHECK(profile.inheritedBy(rule("studio.one", {"a"})).enabled == false, "without its own entry it'd be off");

    const RunSettings run = profile.resolve({rule("studio.one"), rule("other.r", {"a"}), rule("plain")});
    CHECK(run.rules == (std::vector<std::string>{"studio.one", "plain"}), "what runs");
    CHECK(run.severities.at("studio.one") == Severity::Warning && run.parameters.count("studio.one") == 1 &&
              run.parameters.count("plain") == 0,
          "and how");
    CHECK(profile.missing({rule("studio.one")}) == std::vector<std::string>{"gone.rule"}, "a rule that isn't here");
}

USDCC_TEST(validation_profile_files) {
    const std::string text = R"({"name": "Film", "future": [1, 2], "rules": {
        "gone.rule": {"enabled": false, "note": "kept"}, "keyword:a": {"severity": "error"}},
        "templates": [{"id": "low", "template": "name_pattern", "pattern": "[a-z]+", "more": 3},
                      {"id": "new", "template": "from_the_future", "enabled": false}]})";
    Profile profile = parsed(text);
    CHECK(profile.checks.size() == 2 && profile.checks[0].severity == Severity::Warning && profile.checks[0].enabled &&
              !profile.checks[1].enabled && profile.checks[0].ruleName() == "check.low",
          "checks, with their defaults");
    const Profile again = parsed(profile.toJson());
    CHECK(again.toJson() == profile.toJson(), "a round trip changes nothing");
    const JsObject json = JsParseString(profile.toJson()).GetJsObject();
    CHECK(json.count("future") == 1, "what isn't known is kept");
    CHECK(json.at("rules").GetJsObject().at("gone.rule").GetJsObject().count("note") == 1, "in a rule's entry");
    CHECK(json.at("templates").GetJsArray().at(0).GetJsObject().count("more") == 1, "and a check's");
    CHECK(json.at("templates").GetJsArray().size() == 2, "a check of a kind that isn't known too");

    const auto refused = [](const std::string& bad) {
        std::string error;
        return !Profile::parse(bad, &error) && !error.empty();
    };
    CHECK(refused("{"), "not JSON");
    CHECK(refused("[]"), "not an object");
    CHECK(refused("{}"), "no name");
    CHECK(refused(R"({"name": "a", "rules": []})"), "rules of the wrong type");
    CHECK(refused(R"({"name": "a", "templates": [{"template": "x"}]})"), "a check with no id");
    CHECK(refused(R"({"name": "a", "templates": [{"id": "x"}, {"id": "x"}]})"), "or the same as another's");
}

USDCC_TEST(validation_profile_library) {
    QTemporaryDir dir;
    const std::filesystem::path root(dir.path().toStdU16String());
    write(root / "studio/house.json", R"({"name": "House", "rules": {"a.b": {"enabled": false}}})");
    write(root / "studio/broken.json", "{");
    auto& library = ProfileLibrary::instance();
    const std::string before = library.currentName();
    int changes = 0;
    const auto connection = library.changed.connect([&changes]() { ++changes; });
    library.setFolders(root / "user", {root / "studio"});
    CHECK(changes == 1, "reading the folders is a change");
    CHECK(library.find("House") && library.find("House")->readOnly, "a studio's profile, read-only");
    CHECK(library.find("General") && library.find("Everything") && library.find("USDZ") && library.find("Physics"), "the built-in ones");
    CHECK(library.problems().size() == 1, "a file that isn't one is a problem");
    CHECK(!library.find("General")->runs(rule("x", {"UsdzValidators"})) && library.find("General")->runs(rule("x")) &&
              library.find("USDZ")->runs(rule("x", {"UsdzValidators"})) && !library.find("USDZ")->runs(rule("x")),
          "what the built-in ones run");

    library.setCurrent("Nowhere");
    CHECK(library.current().name == "General" && library.currentName() == "Nowhere", "a chosen one that isn't here: General, but it's kept");
    library.setCurrent("House");
    CHECK(library.current().name == "House", "chosen");

    Profile mine = *library.find("House");
    std::string error;
    CHECK(!library.save(mine, &error), "a read-only one can't be saved");
    mine.readOnly = false;
    mine.file.clear();
    CHECK(!library.save(mine, &error) && error.find("another") != std::string::npos, "nor one by a name that's taken");
    mine.name = library.unusedName("House");
    CHECK(mine.name == "House 2" && library.save(mine, &error), "saved as another [%s]", error.c_str());
    const Profile* saved = library.find("House 2");
    CHECK(saved && !saved->readOnly && saved->file == root / "user/house_2.json" && !saved->runs(rule("a.b")), "in the user's folder");
    if (saved) {
        Profile edited = *saved;
        edited.rules["c.d"].severity = Severity::Info;
        CHECK(library.save(edited, &error) && library.find("House 2")->rules.count("c.d") == 1, "and again, changed");
        CHECK(read(root / "user/house_2.json").find("c.d") != std::string::npos, "on disk");
    }
    write(root / "user/general.json", R"({"name": "General"})");
    library.reload();
    CHECK(library.find("General")->file == root / "user/general.json", "the user's own General stands in for the built-in one");
    CHECK(!library.remove("Everything", &error) && library.remove("General", &error) && library.find("General")->file.empty(),
          "deleted, the built-in one is back");
    library.setFolders({}, {});
    library.setCurrent(before);
    CHECK(library.find("House") == nullptr, "no folders, no profiles of one's own");
}

USDCC_TEST(validation_checks) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform::Define(stage, SdfPath("/good_name"));
    UsdGeomXform::Define(stage, SdfPath("/BadName"));
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/mesh"));
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{3, 3, 3});
    mesh.CreateSubdivisionSchemeAttr().Set(TfToken("loop"));
    mesh.GetPrim().ApplyAPI(TfToken("CollectionAPI"), TfToken("lights"));
    UsdGeomSphere sphere = UsdGeomSphere::Define(stage, SdfPath("/sphere"));
    sphere.CreateRadiusAttr().Set(5.0);
    UsdGeomSphere animated = UsdGeomSphere::Define(stage, SdfPath("/animated"));
    animated.CreateRadiusAttr().Set(9.0, UsdTimeCode(3));

    CHECK(paths(found(stage, checkOf("name_pattern", R"({"pattern": "[a-z_]+"})"))) == "/BadName", "name_pattern finds");
    CHECK(found(stage, checkOf("name_pattern", R"({"pattern": "[a-z_]+", "types": ["Mesh"]})")).empty(), "and passes other types");
    CHECK(paths(found(stage, checkOf("name_pattern", R"({"pattern": "nothing", "types": ["UsdGeomSphere", "CollectionAPI"]})"))) ==
              "/animated /mesh /sphere",
          "a type by either name, an API schema with an instance");

    CHECK(found(stage, checkOf("required_property", R"({"property": "radius", "types": ["Sphere"]})")).empty(), "required_property passes");
    CHECK(paths(found(stage, checkOf("required_property", R"({"property": "extent", "types": ["Sphere"], "authored": true})"))) == "/animated /sphere",
          "and finds what isn't authored");
    CHECK(paths(found(stage, checkOf("required_property", R"({"property": "nope", "types": ["Mesh"]})"))) == "/mesh", "or isn't there");

    CHECK(paths(found(stage, checkOf("value", R"({"attribute": "subdivisionScheme", "values": ["none", "catmullClark"]})"))) ==
              "/mesh.subdivisionScheme",
          "value finds one not in the list");
    CHECK(found(stage, checkOf("value", R"({"attribute": "subdivisionScheme", "values": ["loop"]})")).empty(), "and passes one that is");
    CHECK(paths(found(stage, checkOf("value", R"({"attribute": "radius", "maximum": 6})"))) == "/animated.radius", "a range, animated too");
    CHECK(paths(found(stage, checkOf("value", R"({"attribute": "radius", "minimum": 5.5})"))) == "/sphere.radius", "and its other end");
    CHECK(paths(found(stage, checkOf("value", R"({"attribute": "subdivisionScheme", "minimum": 1})"))) == "/mesh.subdivisionScheme",
          "a range for what isn't a number");

    CHECK(found(stage, checkOf("required_schema", R"({"schema": "CollectionAPI", "types": ["Mesh"]})")).empty(), "required_schema passes");
    CHECK(paths(found(stage, checkOf("required_schema", R"({"schema": "MaterialBindingAPI", "types": ["Mesh"]})"))) == "/mesh", "and finds");

    CHECK(found(stage, checkOf("face_count", R"({"maximum": 3})")).empty(), "face_count passes");
    const auto faces = found(stage, checkOf("face_count", R"({"maximum": 2})"));
    CHECK(paths(faces) == "/mesh" && faces[0].message == "It has 3 faces, more than 2", "and finds");

    UsdPrim assets = stage->DefinePrim(SdfPath("/assets"));
    const auto asset = [&assets](const char* name, const char* path) {
        assets.CreateAttribute(TfToken(name), SdfValueTypeNames->Asset).Set(SdfAssetPath(path));
    };
    asset("rel", "./textures/a.png");
    asset("drive", "C:/textures/b.png");
    asset("url", "https://example.test/c.png");
    asset("udim", "./textures/d.<UDIM>.png");
    assets.CreateAttribute(TfToken("list"), SdfValueTypeNames->AssetArray).Set(VtArray<SdfAssetPath>{SdfAssetPath("/root/e.exr")});
    CHECK(paths(found(stage, checkOf("asset_path", R"({"relative": true})"))) == "/assets.drive /assets.list /assets.url", "asset_path: relative");
    CHECK(paths(found(stage, checkOf("asset_path", R"({"pattern": ".*\\.png"})"))) == "/assets.list", "a pattern, in arrays too");
    CHECK(found(stage, checkOf("asset_path", R"({"resolves": true, "pattern": ".*"})")).size() == 4, "can't be found, but for a UDIM");
    CHECK(found(stage, checkOf("asset_path", R"({"pattern": ".*"})")).empty(), "and passes");

    UsdModelAPI(UsdGeomXform::Define(stage, SdfPath("/asm"))).SetKind(KindTokens->assembly);
    UsdModelAPI(UsdGeomXform::Define(stage, SdfPath("/asm/comp"))).SetKind(KindTokens->component);
    UsdModelAPI(UsdGeomXform::Define(stage, SdfPath("/top"))).SetKind(KindTokens->component);
    UsdGeomXform::Define(stage, SdfPath("/plain"));
    UsdModelAPI(UsdGeomXform::Define(stage, SdfPath("/plain/comp"))).SetKind(KindTokens->component);
    CHECK(paths(found(stage, checkOf("kind_hierarchy", R"({"kinds": ["component"], "under": ["assembly", "group"]})"))) == "/plain/comp /top",
          "kind_hierarchy finds");
    CHECK(paths(found(stage, checkOf("kind_hierarchy", R"({"kinds": ["component"], "under": ["assembly", "/"]})"))) == "/plain/comp",
          "and passes, the root too");

    // What won't do.
    CHECK(refusal(checkOf("nope", "{}")).find("nope") != std::string::npos, "an unknown kind");
    CHECK(refusal(checkOf("name_pattern", "{}")) == "Pattern is missing", "a field that's missing");
    CHECK(!refusal(checkOf("name_pattern", R"({"pattern": "["})")).empty(), "a pattern that isn't one");
    CHECK(!refusal(checkOf("name_pattern", R"({"pattern": "a", "types": ["NoSuchSchema"]})")).empty(), "an unknown schema");
    CHECK(!refusal(checkOf("name_pattern", R"({"pattern": 3})")).empty(), "the wrong type");
    CHECK(!refusal(checkOf("value", R"({"attribute": "a"})")).empty(), "a value that could be anything");
    CHECK(!refusal(checkOf("asset_path", "{}")).empty(), "paths that could be anything");
    const auto broken = found(stage, checkOf("name_pattern", "{}"));
    CHECK(broken.size() == 1 && broken[0].name == "RuleFailed" && broken[0].rule == "check.t", "in a run it's a finding (%zu)", broken.size());

    CheckSettings info = checkOf("name_pattern", R"({"pattern": "[a-z_]+"})");
    info.severity = Severity::Info;
    const auto infos = found(stage, info);
    CHECK(!infos.empty() && infos[0].severity == Severity::Info && infos[0].rule == "check.t", "its severity and its rule's name");
}

USDCC_TEST(validation_severities_and_ignored) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSphere::Define(stage, SdfPath("/a"));
    UsdGeomSphere::Define(stage, SdfPath("/a/b"));
    UsdGeomSphere::Define(stage, SdfPath("/a/c"));
    const std::string nested = "usdGeomValidators:EncapsulationChecker";
    RunSettings settings;
    settings.rules = {nested};
    settings.severities[nested] = Severity::Info;
    const auto run = [&]() {
        ValidationRun validation(stage, settings);
        validation.runAll();
        return validation.findings();
    };
    auto findings = run();
    CHECK(findings.size() == 2 && findings[0].severity == Severity::Info && !findings[0].ignored, "a rule's severity is the profile's");

    const UsdPrim b = stage->GetPrimAtPath(SdfPath("/a/b"));
    CHECK(setIgnored(b, nested, true) && !setIgnored(b, nested, true), "ignored, once");
    CHECK(setIgnored(b, "other", true) && ignoredRules(b).size() == 2, "with another rule");
    findings = run();
    CHECK(findings.size() == 2 && findings[0].ignored && !findings[1].ignored, "the finding is there, marked");
    CHECK(setIgnored(b, nested, false) && ignoredRules(b) == std::vector<std::string>{"other"} && !run()[0].ignored, "not any more");
    CHECK(setIgnored(b, "other", false) && !b.HasAuthoredCustomData(), "and no trace is left");
    CHECK(!setIgnored(b, "other", false), "there's nothing to stop");

    // A weaker layer says so: the edit target has to say otherwise.
    const SdfLayerRefPtr weak = SdfLayer::CreateAnonymous(".usda");
    stage->GetRootLayer()->InsertSubLayerPath(weak->GetIdentifier());
    stage->SetEditTarget(weak);
    stage->OverridePrim(SdfPath("/a/c"));
    const UsdPrim c = stage->GetPrimAtPath(SdfPath("/a/c"));
    setIgnored(c, nested, true);
    stage->SetEditTarget(stage->GetRootLayer());
    CHECK(setIgnored(c, nested, false) && ignoredRules(c).empty(), "an empty list over a weaker one");

    CHECK(suggestedProfile(stage).empty(), "a stage asks for none");
    VtDictionary usdcc, data;
    usdcc["validationProfile"] = std::string("Film");
    data["usdcc"] = usdcc;
    stage->GetRootLayer()->SetCustomLayerData(data);
    CHECK(suggestedProfile(stage) == "Film", "or for one");
}
