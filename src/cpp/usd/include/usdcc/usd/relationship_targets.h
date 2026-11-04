#pragma once

#include <pxr/usd/usd/prim.h>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// What a relationship expects to target (docs/RELATIONSHIPS.md). USD's schemas
// don't record it, and USD lets a relationship target anything, so this is ours:
// a runtime registry, from C++ and from Python (usdcc.usd), that the app fills
// with the same calls a plugin uses.
struct RelationshipTargetRule {
    enum class Severity {
        Block,  // greyed out in the target dialog
        Warn,   // can be picked, marked amber
    };
    // A relationship name, with at most one * for any text:
    // "material:binding", "myStudio:*:look". Identifies the rule.
    std::string relationship;
    // A target is fine if it is one of these prim types (or derives from one),
    // or has one of these API schemas. Neither: any prim is fine.
    std::vector<std::string> types;
    std::vector<std::string> schemas;
    Severity severity = Severity::Block;
    // Holds one target: picking another replaces it.
    bool single = false;
    std::string reason;  // for the tooltip: "A material binding targets a Material"
};

struct RelationshipTargetVerdict {
    enum class Kind { Fine, Warn, Blocked };
    Kind kind = Kind::Fine;
    std::string reason;
};

class RelationshipTargetRules {
public:
    // The app's own, which starts with the built-in rules.
    static RelationshipTargetRules& instance();
    RelationshipTargetRules();

    // Adds a rule, or replaces the one with the same `relationship`, so running a
    // script again doesn't pile up rules and a studio can override a built-in one.
    // A type or schema USD doesn't know (its plugin may load later) logs a warning,
    // and is compared by name until then.
    void set(const RelationshipTargetRule& rule);
    // False if there was no such rule.
    bool remove(const std::string& relationship);
    // Back to only the built-in rules.
    void reset();
    const std::vector<RelationshipTargetRule>& rules() const { return m_rules; }

    // The rule for a relationship: an exact name first, else the pattern with the
    // most literal text; later rules win ties. None: anything goes.
    std::optional<RelationshipTargetRule> find(const std::string& relationship) const;
    RelationshipTargetVerdict check(const std::string& relationship, const PXR_NS::UsdPrim& target) const;

private:
    std::vector<RelationshipTargetRule> m_rules;
};

}  // namespace usdcc::usd
