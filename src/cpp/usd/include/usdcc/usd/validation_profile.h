#pragma once

// Which rules run, how severe they are and with which parameters (docs/VALIDATION.md §6). Qt-free.

#include "usdcc/core/signal.h"
#include "usdcc/usd/validation_rules.h"
#include "usdcc/usd/validation_templates.h"

#include <pxr/base/js/value.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd::validation {

// What a run is to do: a profile applied to the rules there are.
struct RunSettings {
    std::vector<std::string> rules;
    std::map<std::string, Severity> severities;          // by rule; none: its own
    std::map<std::string, PXR_NS::JsObject> parameters;  // by rule made with usdcc.validation
    std::vector<CheckSettings> checks;
};

struct RuleSettings {
    std::optional<bool> enabled;
    std::optional<Severity> severity;
    PXR_NS::JsObject parameters;
    bool empty() const { return !enabled && !severity && parameters.empty(); }
};

class Profile {
public:
    std::string name;
    std::string description;
    std::filesystem::path file;  // empty for one that's built in or not saved yet
    bool readOnly = false;

    // By "studio.mesh_names", "group:studio", "keyword:UsdzValidators" or "*", each winning over
    // those after it, setting by setting. Names no rule has are kept.
    std::map<std::string, RuleSettings> rules;
    std::vector<CheckSettings> checks;

    // Nothing with `error` saying why, for text that isn't a profile.
    static std::optional<Profile> parse(const std::string& text, std::string* error);
    // What this version doesn't know in what was parsed is kept.
    std::string toJson() const;

    // `rule`'s settings: of several keywords that say something, off and the most severe win.
    RuleSettings settingsFor(const Rule& rule) const;
    // As it would be without its own entry.
    RuleSettings inheritedBy(const Rule& rule) const;
    bool runs(const Rule& rule) const { return settingsFor(rule).enabled.value_or(true); }

    RunSettings resolve(const std::vector<Rule>& rules) const;
    // Entries naming a rule that isn't there.
    std::vector<std::string> missing(const std::vector<Rule>& rules) const;

private:
    RuleSettings combine(const Rule& rule, bool own) const;
    PXR_NS::JsObject m_source;  // what was parsed
};

const char* severityId(Severity severity);  // "error", "warning", "info"
std::optional<Severity> severityFromId(const std::string& id);

// The profiles there are. Main thread.
class ProfileLibrary {
public:
    static ProfileLibrary& instance();

    // Where they are: `user` is the one that's written to, the rest are read-only. Reads them.
    void setFolders(std::filesystem::path user, std::vector<std::filesystem::path> others);
    // Reads the folders again.
    void reload();

    // The user's, the other folders', then the built-in ones. The first of a name wins.
    const std::vector<Profile>& profiles() const { return m_profiles; }
    // Null for a name there isn't.
    const Profile* find(const std::string& name) const;
    // The chosen one; the built-in General when its name isn't there.
    const Profile& current() const;
    const std::string& currentName() const { return m_current; }
    void setCurrent(const std::string& name);
    static constexpr const char* kDefault = "General";

    // Writes it to the user's folder, and makes it the one there. False with `error` saying
    // why; another of that name is an error unless it's the same file.
    bool save(Profile profile, std::string* error);
    bool remove(const std::string& name, std::string* error);
    // "Film 2" for a "Film" that's taken.
    std::string unusedName(const std::string& base) const;

    // Files that aren't profiles, and why.
    const std::vector<UserRules::Problem>& problems() const { return m_problems; }

    // The profiles or the chosen one changed.
    usdcc::core::Signal<> changed;

private:
    ProfileLibrary();
    void read();

    std::filesystem::path m_user;
    std::vector<std::filesystem::path> m_others;
    std::vector<Profile> m_profiles;
    std::vector<UserRules::Problem> m_problems;
    std::string m_current = kDefault;
};

// A prim's list of rules not to bother with there: customData["usdcc:ignoredRules"]. A check's
// is "check.<id>".
std::vector<std::string> ignoredRules(const PXR_NS::UsdPrim& prim);
// In the stage's edit target. Nothing ignored clears it.
bool setIgnored(const PXR_NS::UsdPrim& prim, const std::string& rule, bool ignored);
// The profile a stage asks for, in its root layer's customLayerData["usdcc:validationProfile"].
std::string suggestedProfile(const PXR_NS::UsdStageRefPtr& stage);

}  // namespace usdcc::usd::validation
