// RelationshipTargetRules (docs/RELATIONSHIPS.md): the built-in rules, derived
// types and API schemas, patterns and which one wins, replacing a rule, a type USD
// doesn't know, and that nothing is restricted without a rule.
#include "test_harness.h"

#include "usdcc/usd/relationship_targets.h"

#include <pxr/usd/usd/stage.h>

using usdcc::usd::RelationshipTargetRule;
using usdcc::usd::RelationshipTargetRules;
using usdcc::usd::RelationshipTargetVerdict;
using Kind = usdcc::usd::RelationshipTargetVerdict::Kind;
PXR_NAMESPACE_USING_DIRECTIVE

USDCC_TEST(relationship_target_rules) {
    RelationshipTargetRules rules;
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const UsdPrim material = stage->DefinePrim(SdfPath("/Mat"), TfToken("Material"));
    const UsdPrim xform = stage->DefinePrim(SdfPath("/X"), TfToken("Xform"));
    const UsdPrim camera = stage->DefinePrim(SdfPath("/Cam"), TfToken("Camera"));
    const UsdPrim mesh = stage->DefinePrim(SdfPath("/M"), TfToken("Mesh"));

    // Built in.
    CHECK(rules.check("material:binding", material).kind == Kind::Fine, "a Material can be bound");
    const RelationshipTargetVerdict verdict = rules.check("material:binding", xform);
    CHECK(verdict.kind == Kind::Blocked && !verdict.reason.empty(), "an Xform can't, with a reason");
    CHECK(rules.find("material:binding")->single, "a binding holds one target");
    CHECK(rules.check("camera", camera).kind == Kind::Fine && rules.check("camera", xform).kind == Kind::Blocked, "camera");
    CHECK(rules.check("renderSource", xform).kind == Kind::Warn, "a warning rule warns");
    CHECK(rules.check("material:binding:preview", xform).kind == Kind::Blocked, "a purpose's binding too");
    CHECK(rules.check("material:binding:collection:ferns", xform).kind == Kind::Fine,
          "a collection binding isn't restricted: it targets a collection as well");
    // A base type counts.
    CHECK(rules.check("physics:body0", mesh).kind == Kind::Fine && rules.check("physics:body0", material).kind == Kind::Blocked,
          "Xformable takes a Mesh, which derives from it");
    // No rule: anything goes.
    CHECK(!rules.find("myStudio:look") && rules.check("myStudio:look", xform).kind == Kind::Fine, "no rule, no limit");
    CHECK(rules.check("collection:ferns:includes", xform).kind == Kind::Fine, "a collection takes anything");

    // Added at runtime.
    RelationshipTargetRule rule;
    rule.relationship = "myStudio:look";
    rule.types = {"Material"};
    rule.reason = "Looks are Materials";
    rules.set(rule);
    CHECK(rules.check("myStudio:look", xform).kind == Kind::Blocked && rules.check("myStudio:look", material).kind == Kind::Fine,
          "a plugin's rule applies at once");
    // Setting it again replaces it, rather than piling up.
    const std::size_t count = rules.rules().size();
    rule.severity = RelationshipTargetRule::Severity::Warn;
    rules.set(rule);
    CHECK(rules.rules().size() == count && rules.check("myStudio:look", xform).kind == Kind::Warn, "the same name replaces");
    // And a built-in one can be overridden.
    RelationshipTargetRule loose;
    loose.relationship = "camera";
    rules.set(loose);
    CHECK(rules.check("camera", xform).kind == Kind::Fine, "a studio can override a built-in rule");

    // Patterns: the exact name, then the most literal text.
    RelationshipTargetRule wide;
    wide.relationship = "myStudio:*";
    wide.types = {"Camera"};
    wide.reason = "wide";
    rules.set(wide);
    RelationshipTargetRule narrow;
    narrow.relationship = "myStudio:rig:*";
    narrow.types = {"Mesh"};
    narrow.reason = "narrow";
    rules.set(narrow);
    CHECK(rules.check("myStudio:other", xform).reason == "wide", "a pattern matches");
    CHECK(rules.check("myStudio:rig:hand", xform).reason == "narrow", "the more literal one wins");
    CHECK(rules.check("myStudio:look", xform).reason == "Looks are Materials", "the exact one wins");
    RelationshipTargetRule middle;
    middle.relationship = "a:*:z";
    middle.types = {"Camera"};
    rules.set(middle);
    CHECK(rules.find("a:b:z") && !rules.find("a:b:y") && !rules.find("a:z"), "a * in the middle");

    // By API schema.
    RelationshipTargetRule bySchema;
    bySchema.relationship = "myStudio:bound";
    bySchema.schemas = {"CollectionAPI"};
    rules.set(bySchema);
    CHECK(rules.check("myStudio:bound", xform).kind == Kind::Blocked, "without the schema");
    xform.AddAppliedSchema(TfToken("CollectionAPI:ferns"));
    CHECK(rules.check("myStudio:bound", xform).kind == Kind::Fine, "with it, any instance");

    // A type whose plugin isn't loaded: by name, not blocking what has that name.
    RelationshipTargetRule unknown;
    unknown.relationship = "myStudio:custom";
    unknown.types = {"MyStudioThing"};
    rules.set(unknown);
    const UsdPrim thing = stage->DefinePrim(SdfPath("/Thing"), TfToken("MyStudioThing"));
    CHECK(rules.check("myStudio:custom", thing).kind == Kind::Fine, "an unknown type compares by name");
    CHECK(rules.check("myStudio:custom", camera).kind == Kind::Blocked, "and others don't match");

    CHECK(rules.remove("myStudio:look") && !rules.remove("myStudio:look"), "removing, once");
    rules.reset();
    CHECK(rules.check("camera", xform).kind == Kind::Blocked && !rules.find("myStudio:other"), "reset: the built-in ones");
    CHECK(rules.check("material:binding", UsdPrim()).kind == Kind::Fine, "no prim: nothing to object to");
}
