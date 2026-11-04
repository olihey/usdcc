#include "usdcc/usd/relationship_targets.h"

#include "usdcc/core/log.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/usd/schemaRegistry.h>

#include <algorithm>

namespace usdcc::usd {

namespace {

using Rule = RelationshipTargetRule;

// From the schemas' own documentation. Not material:binding:*: a collection
// binding (material:binding:collection:x) targets a collection as well.
std::vector<Rule> builtInRules() {
    auto rule = [](const char* relationship, const char* type, bool single, const char* reason,
                   Rule::Severity severity = Rule::Severity::Block) {
        Rule r;
        r.relationship = relationship;
        r.types = {type};
        r.single = single;
        r.reason = reason;
        r.severity = severity;
        return r;
    };
    return {
        rule("material:binding", "Material", true, "A material binding targets a Material"),
        rule("material:binding:full", "Material", true, "A material binding targets a Material"),
        rule("material:binding:preview", "Material", true, "A material binding targets a Material"),
        rule("skel:skeleton", "Skeleton", true, "Binds a Skeleton"),
        rule("skel:animationSource", "SkelAnimation", true, "An animation source is a SkelAnimation"),
        rule("skel:blendShapeTargets", "BlendShape", false, "Targets BlendShapes"),
        rule("light:filters", "LightFilter", false, "Targets LightFilters"),
        rule("camera", "Camera", true, "Must target a Camera"),
        rule("products", "RenderProduct", false, "Targets RenderProducts"),
        rule("orderedVars", "RenderVar", false, "Targets RenderVars"),
        rule("inputPasses", "RenderPass", false, "Targets RenderPasses"),
        rule("renderSource", "RenderSettings", true, "Usually a RenderSettings, unless a file configures the pass",
             Rule::Severity::Warn),
        rule("physics:body0", "Xformable", true, "A joint's body is Xformable"),
        rule("physics:body1", "Xformable", true, "A joint's body is Xformable"),
        rule("physics:filteredGroups", "PhysicsCollisionGroup", false, "Targets PhysicsCollisionGroups"),
        rule("physics:simulationOwner", "PhysicsScene", true, "A single PhysicsScene simulates this"),
        rule("boundingVolume", "Boundable", true, "The bounding volume is Boundable"),
        rule("proxyPrim", "Imageable", true, "A single Imageable proxy"),
    };
}

// How much of `name` the pattern pins down, or -1 if it doesn't match.
int matchScore(const std::string& pattern, const std::string& name) {
    const std::size_t star = pattern.find('*');
    if (star == std::string::npos) return pattern == name ? int(pattern.size()) + 1000000 : -1;  // exact beats any
    const std::size_t tail = pattern.size() - star - 1;
    if (name.size() < star + tail) return -1;
    if (name.compare(0, star, pattern, 0, star) != 0) return -1;
    if (name.compare(name.size() - tail, tail, pattern, star + 1, tail) != 0) return -1;
    return int(pattern.size()) - 1;
}

bool isType(const PXR_NS::UsdPrim& prim, const std::string& name) {
    const PXR_NS::TfType type = PXR_NS::UsdSchemaRegistry::GetTypeFromSchemaTypeName(PXR_NS::TfToken(name));
    return type ? prim.IsA(type) : prim.GetTypeName().GetString() == name;
}

bool hasSchema(const PXR_NS::UsdPrim& prim, const std::string& name) {
    for (const PXR_NS::TfToken& applied : prim.GetAppliedSchemas()) {
        const std::string& text = applied.GetString();
        if (text == name || text.rfind(name + ":", 0) == 0) return true;  // any instance
    }
    return false;
}

}  // namespace

RelationshipTargetRules& RelationshipTargetRules::instance() {
    static RelationshipTargetRules rules;
    return rules;
}

RelationshipTargetRules::RelationshipTargetRules() : m_rules(builtInRules()) {}

void RelationshipTargetRules::set(const RelationshipTargetRule& rule) {
    const PXR_NS::UsdSchemaRegistry& registry = PXR_NS::UsdSchemaRegistry::GetInstance();
    for (const std::string& type : rule.types) {
        if (!PXR_NS::UsdSchemaRegistry::GetTypeFromSchemaTypeName(PXR_NS::TfToken(type))) {
            USDCC_LOG_WARNING("usdcc.usd", "Relationship rule {}: unknown prim type {}, compared by name",
                              rule.relationship, type);
        }
    }
    for (const std::string& schema : rule.schemas) {
        if (!registry.FindAppliedAPIPrimDefinition(PXR_NS::TfToken(schema))) {
            USDCC_LOG_WARNING("usdcc.usd", "Relationship rule {}: unknown API schema {}, compared by name",
                              rule.relationship, schema);
        }
    }
    remove(rule.relationship);
    m_rules.push_back(rule);
}

bool RelationshipTargetRules::remove(const std::string& relationship) {
    const auto it = std::find_if(m_rules.begin(), m_rules.end(),
                                 [&](const Rule& rule) { return rule.relationship == relationship; });
    if (it == m_rules.end()) return false;
    m_rules.erase(it);
    return true;
}

void RelationshipTargetRules::reset() { m_rules = builtInRules(); }

std::optional<RelationshipTargetRule> RelationshipTargetRules::find(const std::string& relationship) const {
    const Rule* best = nullptr;
    int bestScore = -1;
    for (const Rule& rule : m_rules) {
        const int score = matchScore(rule.relationship, relationship);
        if (score >= 0 && score >= bestScore) {
            best = &rule;
            bestScore = score;
        }
    }
    return best ? std::optional<Rule>(*best) : std::nullopt;
}

RelationshipTargetVerdict RelationshipTargetRules::check(const std::string& relationship,
                                                         const PXR_NS::UsdPrim& target) const {
    RelationshipTargetVerdict verdict;
    const std::optional<Rule> rule = find(relationship);
    if (!rule || !target || (rule->types.empty() && rule->schemas.empty())) return verdict;
    const bool fine = std::any_of(rule->types.begin(), rule->types.end(), [&](const std::string& t) { return isType(target, t); }) ||
                      std::any_of(rule->schemas.begin(), rule->schemas.end(), [&](const std::string& s) { return hasSchema(target, s); });
    if (fine) return verdict;
    verdict.kind = rule->severity == Rule::Severity::Block ? RelationshipTargetVerdict::Kind::Blocked
                                                           : RelationshipTargetVerdict::Kind::Warn;
    verdict.reason = rule->reason;
    return verdict;
}

}  // namespace usdcc::usd
