// Checking a stage with OpenUSD's validators (docs/VALIDATION.md §3): the rules known, what a
// run finds and where, a selection's scope, instances, merging, stopping, and that it runs in
// steps. Runs in usdcc_tests.
#include "test_harness.h"

#include "usdcc/usd/validation_run.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usdValidation/usdValidation/error.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdcc::usd::validation;

namespace {
std::vector<std::string> allRules() {
    std::vector<std::string> names;
    for (const Rule& rule : rules()) {
        if (rule.name.rfind("usdccTest", 0) != 0) names.push_back(rule.name);
    }
    return names;
}

const Finding* find(const ValidationRun& run, const std::string& name) {
    for (const Finding& finding : run.findings()) {
        if (finding.name == name) return &finding;
    }
    return nullptr;
}

// A stage that's fine: metadata, a default prim.
UsdStageRefPtr goodStage() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
    stage->SetDefaultPrim(UsdGeomXform::Define(stage, SdfPath("/World")).GetPrim());
    return stage;
}

// Records the prims it's asked about, and has a finding on each called "bad".
std::mutex visitedMutex;
std::multiset<std::string> visited;

std::string visitor(const TfTokenVector& schemaTypes, const char* name) {
    UsdValidationRegistry& registry = UsdValidationRegistry::GetInstance();
    if (!registry.HasValidator(TfToken(name))) {
        UsdValidationValidatorMetadata metadata;
        metadata.name = TfToken(name);
        metadata.doc = "Test";
        metadata.schemaTypes = schemaTypes;
        registry.RegisterValidator(metadata,
                                   UsdValidatePrimTaskFn([](const UsdPrim& prim, const UsdValidationTimeRange&) {
                                       {
                                           const std::lock_guard<std::mutex> lock(visitedMutex);
                                           visited.insert(prim.GetPath().GetString());
                                       }
                                       UsdValidationErrorVector errors;
                                       if (prim.GetName() == TfToken("bad")) {
                                           errors.emplace_back(TfToken("Bad"), UsdValidationErrorType::Warn,
                                                               UsdValidationErrorSites{UsdValidationErrorSite(
                                                                   prim.GetStage(), prim.GetPath())},
                                                               "It's bad");
                                       }
                                       return errors;
                                   }));
    }
    visited.clear();
    return name;
}
}  // namespace

USDCC_TEST(validation_rules) {
    const std::vector<Rule> known = rules();
    const auto it = std::find_if(known.begin(), known.end(),
                                 [](const Rule& rule) { return rule.name == "usdGeomValidators:StageMetadataChecker"; });
    CHECK(it != known.end(), "OpenUSD's rules are known (%zu)", known.size());
    if (it != known.end()) CHECK(!it->doc.empty() && it->group() == "usdGeomValidators", "with their doc and plugin");
    CHECK(rulesWithKeyword("UsdPhysicsValidators").size() == 4, "the four physics rules by keyword (%zu)",
          rulesWithKeyword("UsdPhysicsValidators").size());
    CHECK(rulesWithKeyword("NoSuchKeyword").empty(), "none for an unknown one");
}

USDCC_TEST(validation_findings) {
    {
        ValidationRun run(goodStage(), allRules());
        run.runAll();
        CHECK(run.findings().empty(), "nothing on a stage that's fine (%zu: %s)", run.findings().size(),
              run.findings().empty() ? "" : run.findings().front().message.c_str());
    }
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomCube::Define(stage, SdfPath("/Cube"));
    UsdGeomSphere::Define(stage, SdfPath("/Cube/Nested"));
    UsdPrim bound = UsdGeomXform::Define(stage, SdfPath("/Bound")).GetPrim();
    stage->DefinePrim(SdfPath("/Mat"), TfToken("Material"));
    bound.CreateRelationship(TfToken("material:binding"), false).AddTarget(SdfPath("/Mat"));
    bound.GetReferences().AddReference("./there_is_no_such_file.usda");

    ValidationRun run(stage, allRules());
    run.runAll();
    CHECK(run.isDone() && run.progress() == 1.0 && run.checkedPrims() == 4 && run.totalPrims() == 4,
          "four prims checked (%zu of %zu)", run.checkedPrims(), run.totalPrims());

    const Finding* upAxis = find(run, "MissingUpAxisMetadata");
    CHECK(upAxis && upAxis->severity == Severity::Error && upAxis->prim().IsEmpty() &&
              upAxis->rule == "usdGeomValidators:StageMetadataChecker",
          "no up axis: an error on the stage, not on a prim");
    const bool nested = std::any_of(run.findings().begin(), run.findings().end(), [](const Finding& finding) {
        return finding.rule == "usdGeomValidators:EncapsulationChecker" && finding.prim() == SdfPath("/Cube/Nested");
    });
    CHECK(nested, "the nested gprim, at its prim");
    const bool binding = std::any_of(run.findings().begin(), run.findings().end(), [](const Finding& finding) {
        return finding.rule == "usdShadeValidators:MaterialBindingApiAppliedValidator" && finding.prim() == SdfPath("/Bound");
    });
    CHECK(binding, "the binding without its API");
    const bool missing = std::any_of(run.findings().begin(), run.findings().end(), [](const Finding& finding) {
        return finding.rule == "usdValidation:CompositionErrorTest" &&
               finding.message.find("there_is_no_such_file") != std::string::npos;
    });
    CHECK(missing, "the missing reference");
    CHECK(std::is_sorted(run.findings().begin(), run.findings().end(),
                         [](const Finding& a, const Finding& b) { return a.severity < b.severity; }),
          "errors first");

    // The same again, in the same order, whichever threads ran it.
    ValidationRun again(stage, allRules());
    again.runAll();
    bool same = again.findings().size() == run.findings().size();
    for (size_t i = 0; same && i < run.findings().size(); ++i) same = run.findings()[i].message == again.findings()[i].message;
    CHECK(same, "the order doesn't change between runs");

    ValidationRun none(stage, {"no:SuchRule"});
    CHECK(none.isDone() && !none.step(), "an unknown rule is left out");
    ValidationRun noStage(nullptr, allRules());
    CHECK(noStage.isDone(), "no stage, nothing to do");
}

USDCC_TEST(validation_visits) {
    const std::string rule = visitor({}, "usdccTest:Visitor");
    UsdStageRefPtr stage = goodStage();
    stage->DefinePrim(SdfPath("/World/Src/bad"));
    stage->DefinePrim(SdfPath("/World/Other/bad"));
    stage->OverridePrim(SdfPath("/Over"));
    stage->CreateClassPrim(SdfPath("/Class"));
    stage->DefinePrim(SdfPath("/World/Off")).SetActive(false);
    UsdPrim instance = stage->DefinePrim(SdfPath("/World/Inst"));
    instance.GetReferences().AddInternalReference(SdfPath("/World/Src"));
    instance.SetInstanceable(true);
    {
        ValidationRun run(stage, {rule});
        run.runAll();
        const std::multiset<std::string> expected = {"/World",           "/World/Src",  "/World/Src/bad", "/World/Other",
                                                     "/World/Other/bad", "/World/Inst", "/World/Inst/bad"};
        CHECK(visited == expected, "each defined, active prim once, instances' contents too (%zu)", visited.size());
        CHECK(run.findings().size() == 3, "a finding on each bad one (%zu)", run.findings().size());
    }
    {
        visitor({}, "usdccTest:Visitor");
        // One inside the other: each prim still once.
        ValidationRun run(stage, {rule}, {SdfPath("/World/Src/bad"), SdfPath("/World/Src"), SdfPath("/Gone")});
        run.runAll();
        const std::multiset<std::string> expected = {"/World/Src", "/World/Src/bad"};
        CHECK(visited == expected, "a selection and what's under it (%zu)", visited.size());
    }
    {
        ValidationRun run(UsdStage::CreateInMemory(), allRules(), {SdfPath::AbsoluteRootPath()});
        run.runAll();
        CHECK(run.findings().empty(), "a selection doesn't run the stage's own rules");
    }
    {
        const std::string twice = visitor({TfToken("UsdGeomImageable"), TfToken("UsdGeomXformable")}, "usdccTest:Twice");
        UsdGeomXform::Define(stage, SdfPath("/World/Twice/bad"));
        ValidationRun run(stage, {twice}, {SdfPath("/World/Twice/bad")});
        run.runAll();
        CHECK(visited.count("/World/Twice/bad") == 2, "a rule with two fitting types runs twice (%zu)",
              visited.count("/World/Twice/bad"));
        CHECK(run.findings().size() == 1, "but its finding is there once (%zu)", run.findings().size());
    }
}

USDCC_TEST(validation_steps) {
    const std::string rule = visitor({}, "usdccTest:Visitor");
    UsdStageRefPtr stage = goodStage();
    for (int i = 0; i < 2000; ++i) stage->DefinePrim(SdfPath("/World/p" + std::to_string(i)));
    ValidationRun run(stage, {rule});
    CHECK(!run.isDone() && run.progress() == 0.0, "nothing runs before the first step");
    int steps = 1;
    double last = 0.0;
    bool forward = true;
    while (run.step(0.5)) {
        ++steps;
        forward = forward && run.progress() >= last;
        last = run.progress();
    }
    CHECK(steps > 3 && forward, "it takes several steps, and its progress goes forward (%d)", steps);
    CHECK(visited.size() == 2001, "every prim once, however the steps fall (%zu)", visited.size());

    visitor({}, "usdccTest:Visitor");
    stage->DefinePrim(SdfPath("/World/bad"));
    ValidationRun stopped(stage, {rule});
    stopped.step();
    stopped.step();
    const size_t checked = stopped.checkedPrims();
    stopped.stop();
    CHECK(stopped.isDone() && !stopped.step() && checked > 0 && stopped.checkedPrims() == checked &&
              stopped.checkedPrims() < stopped.totalPrims(),
          "stopped: no more is checked (%zu of %zu)", stopped.checkedPrims(), stopped.totalPrims());

    // Prims removed between steps are left out, rather than used.
    ValidationRun edited(stage, {rule});
    edited.step();
    stage->RemovePrim(SdfPath("/World"));
    edited.runAll();
    CHECK(edited.isDone(), "a run outlives its prims");
}

USDCC_TEST(validation_failing_rule) {
    UsdValidationRegistry& registry = UsdValidationRegistry::GetInstance();
    const TfToken name("usdccTest:Fails");
    if (!registry.HasValidator(name)) {
        UsdValidationValidatorMetadata metadata;
        metadata.name = name;
        metadata.doc = "Test";
        registry.RegisterValidator(metadata, UsdValidatePrimTaskFn([](const UsdPrim&, const UsdValidationTimeRange&) {
                                       TF_RUNTIME_ERROR("the rule went wrong");
                                       return UsdValidationErrorVector();
                                   }));
    }
    const std::string good = visitor({}, "usdccTest:Visitor");
    UsdStageRefPtr stage = goodStage();
    stage->DefinePrim(SdfPath("/World/bad"));
    ValidationRun run(stage, {name.GetString(), good});
    run.runAll();
    const Finding* failed = find(run, "RuleFailed");
    CHECK(failed && failed->message.find("the rule went wrong") != std::string::npos, "a rule's failure is a finding");
    CHECK(find(run, "Bad"), "and the other rules' findings are there");
    CHECK(run.findings().size() == 2, "once, for both prims (%zu)", run.findings().size());
}
