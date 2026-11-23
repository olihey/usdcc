#pragma once

// Checks made by filling in a form rather than writing code (docs/VALIDATION.md §5.3). Qt-free.

#include "usdcc/usd/validation_finding.h"

#include <pxr/base/js/value.h>
#include <pxr/usd/usd/prim.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd::validation {

// What a check asks for.
struct TemplateField {
    enum class Type { Text, TextList, Bool, Number };
    std::string key;    // in the profile's JSON
    std::string label;
    std::string tip;
    Type type = Type::Text;
    bool required = false;
};

// A kind of check: "name_pattern".
struct CheckTemplate {
    std::string id;
    std::string label;
    std::string doc;
    std::vector<TemplateField> fields;
};
const std::vector<CheckTemplate>& checkTemplates();
const CheckTemplate* findCheckTemplate(const std::string& id);

// One of them filled in, as a profile holds it.
struct CheckSettings {
    std::string id;        // unique in its profile; its findings' rule is "check.<id>"
    std::string label;     // empty: the template's
    std::string templateId;
    Severity severity = Severity::Warning;
    bool enabled = true;
    PXR_NS::JsObject values;  // by field; whatever else is there is kept

    std::string ruleName() const { return "check." + id; }
    std::string displayName() const;
};

// Ready to run, on any thread.
class Check {
public:
    // Null with `error` saying what's wrong with the settings.
    static std::shared_ptr<const Check> make(const CheckSettings& settings, std::string* error);

    // The finding on `prim`, if there is one (or several, for a check on its attributes).
    std::vector<Finding> run(const PXR_NS::UsdPrim& prim) const;

private:
    using Report = std::function<void(const PXR_NS::SdfPath& site, const std::string& message)>;
    std::string m_rule;
    std::string m_name;
    Severity m_severity = Severity::Warning;
    std::vector<PXR_NS::TfType> m_types;
    std::function<void(const PXR_NS::UsdPrim&, const Report&)> m_check;
};

// The same check as a rules file in Python, to start a rule from.
std::string checkAsPython(const CheckSettings& settings);

}  // namespace usdcc::usd::validation
