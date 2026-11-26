#include "usdcc/usd/validation_rules.h"

#include "usdcc/usd/primvar_info.h"

#include <pxr/base/js/json.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>

#include <algorithm>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::validation {

namespace {
std::vector<std::string> strings(const TfTokenVector& tokens) {
    std::vector<std::string> result;
    for (const TfToken& token : tokens) result.push_back(token.GetString());
    return result;
}
}  // namespace

std::string Rule::group() const {
    const size_t colon = name.find(':');
    if (colon != std::string::npos) return name.substr(0, colon);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::vector<Rule> rules() {
    registerPrimvarRules();  // usdcc's own, which OpenUSD has none of
    std::vector<Rule> result = UserRules::instance().rules();
    for (const UsdValidationValidatorMetadata& metadata : UsdValidationRegistry::GetInstance().GetAllValidatorMetadata()) {
        // usdcc.validation's are above, those that are in use.
        if (metadata.isSuite || metadata.name.GetString().rfind("usdcc:", 0) == 0) continue;
        Rule rule;
        rule.name = metadata.name.GetString();
        rule.doc = metadata.doc;
        rule.keywords = strings(metadata.keywords);
        rule.schemaTypes = strings(metadata.schemaTypes);
        rule.registered = rule.name;
        result.push_back(std::move(rule));
    }
    std::sort(result.begin(), result.end(), [](const Rule& a, const Rule& b) { return a.name < b.name; });
    return result;
}

std::vector<std::string> rulesWithKeyword(const std::string& keyword) {
    std::vector<std::string> result;
    for (const Rule& rule : rules()) {
        if (std::find(rule.keywords.begin(), rule.keywords.end(), keyword) != rule.keywords.end()) {
            result.push_back(rule.name);
        }
    }
    return result;
}

UserRules& UserRules::instance() {
    static UserRules* const rules = new UserRules;  // outlives Python and everything else
    return *rules;
}

void UserRules::set(Rule rule) {
    std::erase_if(m_rules, [&rule](const Rule& other) { return other.name == rule.name; });
    m_rules.push_back(std::move(rule));
    changed();
}

void UserRules::remove(const std::string& name) {
    if (std::erase_if(m_rules, [&name](const Rule& rule) { return rule.name == name; }) > 0) changed();
}

void UserRules::beginStep(long run, const std::map<std::string, JsObject>* parameters) {
    m_parameters = parameters;
    m_run = run;
}

void UserRules::endStep() {
    m_run = 0;
    m_parameters = nullptr;
}

std::string UserRules::parameters(const std::string& rule) const {
    if (!m_parameters) return {};
    const auto it = m_parameters->find(rule);
    return it == m_parameters->end() ? std::string() : JsWriteToString(JsValue(it->second));
}

void UserRules::setProblems(std::vector<Problem> problems) {
    m_problems = std::move(problems);
    changed();
}

}  // namespace usdcc::usd::validation
