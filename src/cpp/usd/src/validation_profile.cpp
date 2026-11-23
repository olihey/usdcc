#include "usdcc/usd/validation_profile.h"

#include <pxr/base/js/json.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::validation {

namespace {
const TfToken kIgnoredRules("usdcc:ignoredRules");
constexpr auto kSuggested = "validationProfile";

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string stringIn(const JsObject& object, const std::string& key) {
    const auto it = object.find(key);
    return it != object.end() && it->second.IsString() ? it->second.GetString() : std::string();
}

RuleSettings ruleSettings(const JsObject& object) {
    RuleSettings settings;
    if (const auto it = object.find("enabled"); it != object.end() && it->second.IsBool()) settings.enabled = it->second.GetBool();
    settings.severity = severityFromId(stringIn(object, "severity"));
    if (const auto it = object.find("parameters"); it != object.end() && it->second.IsObject()) {
        settings.parameters = it->second.GetJsObject();
    }
    return settings;
}

// What `stronger` doesn't say, from `weaker`.
void fill(RuleSettings& stronger, const RuleSettings& weaker) {
    if (!stronger.enabled) stronger.enabled = weaker.enabled;
    if (!stronger.severity) stronger.severity = weaker.severity;
    stronger.parameters.insert(weaker.parameters.begin(), weaker.parameters.end());
}

std::vector<Profile> builtIn() {
    std::vector<Profile> result;
    const auto add = [&result](const char* name, const char* description) -> Profile& {
        Profile profile;
        profile.name = name;
        profile.description = description;
        profile.readOnly = true;
        result.push_back(std::move(profile));
        return result.back();
    };
    RuleSettings off, on;
    off.enabled = false;
    on.enabled = true;
    Profile& general = add(ProfileLibrary::kDefault, "All rules but those for USDZ packages and physics");
    general.rules["keyword:UsdzValidators"] = off;
    general.rules["keyword:UsdPhysicsValidators"] = off;
    add("Everything", "All rules");
    Profile& usdz = add("USDZ", "The rules for USDZ packages");
    usdz.rules["*"] = off;
    usdz.rules["keyword:UsdzValidators"] = on;
    Profile& physics = add("Physics", "The rules for physics");
    physics.rules["*"] = off;
    physics.rules["keyword:UsdPhysicsValidators"] = on;
    return result;
}
}  // namespace

const char* severityId(Severity severity) {
    return severity == Severity::Error ? "error" : severity == Severity::Warning ? "warning" : "info";
}

std::optional<Severity> severityFromId(const std::string& id) {
    if (id == "error") return Severity::Error;
    if (id == "warning") return Severity::Warning;
    if (id == "info") return Severity::Info;
    return std::nullopt;
}

// --- Profile -----------------------------------------------------------------------

std::optional<Profile> Profile::parse(const std::string& text, std::string* error) {
    JsParseError problem;
    const JsValue value = JsParseString(text, &problem);
    if (!value.IsObject()) {
        *error = problem.reason.empty() ? "it isn't a JSON object" : "line " + std::to_string(problem.line) + ": " + problem.reason;
        return std::nullopt;
    }
    Profile profile;
    profile.m_source = value.GetJsObject();
    profile.name = stringIn(profile.m_source, "name");
    profile.description = stringIn(profile.m_source, "description");
    if (profile.name.empty()) {
        *error = "it has no name";
        return std::nullopt;
    }
    if (const auto it = profile.m_source.find("rules"); it != profile.m_source.end()) {
        if (!it->second.IsObject()) {
            *error = "\"rules\" isn't an object";
            return std::nullopt;
        }
        for (const auto& [name, settings] : it->second.GetJsObject()) {
            if (settings.IsObject()) profile.rules[name] = ruleSettings(settings.GetJsObject());
        }
    }
    if (const auto it = profile.m_source.find("templates"); it != profile.m_source.end()) {
        if (!it->second.IsArray()) {
            *error = "\"templates\" isn't a list";
            return std::nullopt;
        }
        std::set<std::string> ids;
        for (const JsValue& entry : it->second.GetJsArray()) {
            if (!entry.IsObject()) continue;
            CheckSettings check;
            check.values = entry.GetJsObject();
            check.id = stringIn(check.values, "id");
            check.label = stringIn(check.values, "label");
            check.templateId = stringIn(check.values, "template");
            check.severity = severityFromId(stringIn(check.values, "severity")).value_or(Severity::Warning);
            const auto enabled = check.values.find("enabled");
            check.enabled = enabled == check.values.end() || !enabled->second.IsBool() || enabled->second.GetBool();
            for (const char* key : {"id", "label", "template", "severity", "enabled"}) check.values.erase(key);
            if (check.id.empty() || !ids.insert(check.id).second) {
                *error = check.id.empty() ? "a template has no id" : "two templates have the id " + check.id;
                return std::nullopt;
            }
            profile.checks.push_back(std::move(check));
        }
    }
    return profile;
}

std::string Profile::toJson() const {
    JsObject object = m_source;
    object["name"] = JsValue(name);
    if (description.empty()) {
        object.erase("description");
    } else {
        object["description"] = JsValue(description);
    }
    // Keys in an entry that this version doesn't know stay.
    JsObject before;
    if (const auto it = object.find("rules"); it != object.end() && it->second.IsObject()) before = it->second.GetJsObject();
    JsObject after;
    for (const auto& [key, settings] : rules) {
        JsObject entry;
        if (const auto it = before.find(key); it != before.end() && it->second.IsObject()) entry = it->second.GetJsObject();
        for (const char* known : {"enabled", "severity", "parameters"}) entry.erase(known);
        if (settings.enabled) entry["enabled"] = JsValue(*settings.enabled);
        if (settings.severity) entry["severity"] = JsValue(std::string(severityId(*settings.severity)));
        if (!settings.parameters.empty()) entry["parameters"] = JsValue(settings.parameters);
        if (!entry.empty()) after[key] = JsValue(entry);
    }
    object["rules"] = JsValue(after);
    JsArray templates;
    for (const CheckSettings& check : checks) {
        JsObject entry = check.values;
        entry["id"] = JsValue(check.id);
        entry["template"] = JsValue(check.templateId);
        entry["severity"] = JsValue(std::string(severityId(check.severity)));
        if (!check.label.empty()) entry["label"] = JsValue(check.label);
        if (!check.enabled) entry["enabled"] = JsValue(false);
        templates.push_back(JsValue(entry));
    }
    if (templates.empty()) {
        object.erase("templates");
    } else {
        object["templates"] = JsValue(templates);
    }
    return JsWriteToString(JsValue(object)) + "\n";
}

RuleSettings Profile::combine(const Rule& rule, bool own) const {
    RuleSettings result;
    const auto use = [this, &result](const std::string& key) {
        if (const auto it = rules.find(key); it != rules.end()) fill(result, it->second);
    };
    if (own) use(rule.name);
    use("group:" + rule.group());
    RuleSettings keywords;
    for (const std::string& keyword : rule.keywords) {
        const auto it = rules.find("keyword:" + keyword);
        if (it == rules.end()) continue;
        if (it->second.enabled) keywords.enabled = keywords.enabled.value_or(true) && *it->second.enabled;
        if (it->second.severity) keywords.severity = std::min(keywords.severity.value_or(Severity::Info), *it->second.severity);
        keywords.parameters.insert(it->second.parameters.begin(), it->second.parameters.end());
    }
    fill(result, keywords);
    use("*");
    return result;
}

RuleSettings Profile::settingsFor(const Rule& rule) const { return combine(rule, true); }
RuleSettings Profile::inheritedBy(const Rule& rule) const { return combine(rule, false); }

RunSettings Profile::resolve(const std::vector<Rule>& known) const {
    RunSettings result;
    for (const Rule& rule : known) {
        const RuleSettings settings = settingsFor(rule);
        if (!settings.enabled.value_or(true)) continue;
        result.rules.push_back(rule.name);
        if (settings.severity) result.severities[rule.name] = *settings.severity;
        if (!settings.parameters.empty()) result.parameters[rule.name] = settings.parameters;
    }
    for (const CheckSettings& check : checks) {
        if (check.enabled) result.checks.push_back(check);
    }
    return result;
}

std::vector<std::string> Profile::missing(const std::vector<Rule>& known) const {
    std::vector<std::string> result;
    for (const auto& [key, settings] : rules) {
        if (key == "*" || key.rfind("keyword:", 0) == 0 || key.rfind("group:", 0) == 0) continue;
        if (std::none_of(known.begin(), known.end(), [&key = key](const Rule& rule) { return rule.name == key; })) {
            result.push_back(key);
        }
    }
    return result;
}

// --- ProfileLibrary ----------------------------------------------------------------

ProfileLibrary::ProfileLibrary() { read(); }

ProfileLibrary& ProfileLibrary::instance() {
    static ProfileLibrary library;
    return library;
}

void ProfileLibrary::setFolders(std::filesystem::path user, std::vector<std::filesystem::path> others) {
    m_user = std::move(user);
    m_others = std::move(others);
    reload();
}

void ProfileLibrary::reload() {
    read();
    changed();
}

void ProfileLibrary::read() {
    m_profiles.clear();
    m_problems.clear();
    std::set<std::string> names;
    const auto readFolder = [&](const std::filesystem::path& folder, bool readOnly) {
        std::error_code error;
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            if (entry.is_regular_file(error) && entry.path().extension() == ".json") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& file : files) {
            std::ifstream stream(file, std::ios::binary);
            std::stringstream text;
            text << stream.rdbuf();
            std::string why;
            std::optional<Profile> profile = Profile::parse(text.str(), &why);
            if (!profile) {
                m_problems.push_back({toUtf8(file), why});
            } else if (!names.insert(profile->name).second) {
                m_problems.push_back({toUtf8(file), "there's another profile called " + profile->name});
            } else {
                profile->file = file;
                profile->readOnly = readOnly;
                m_profiles.push_back(std::move(*profile));
            }
        }
    };
    if (!m_user.empty()) readFolder(m_user, false);
    for (const std::filesystem::path& folder : m_others) readFolder(folder, true);
    for (Profile& profile : builtIn()) {
        // The user's may stand in for a built-in one.
        if (names.insert(profile.name).second) m_profiles.push_back(std::move(profile));
    }
}

const Profile* ProfileLibrary::find(const std::string& name) const {
    for (const Profile& profile : m_profiles) {
        if (profile.name == name) return &profile;
    }
    return nullptr;
}

const Profile& ProfileLibrary::current() const {
    const Profile* profile = find(m_current);
    return profile ? *profile : *find(kDefault);
}

void ProfileLibrary::setCurrent(const std::string& name) {
    if (name == m_current) return;
    m_current = name;
    changed();
}

bool ProfileLibrary::save(Profile profile, std::string* error) {
    if (profile.readOnly) {
        *error = "it's read-only";
        return false;
    }
    if (profile.name.empty()) {
        *error = "it needs a name";
        return false;
    }
    if (m_user.empty()) {
        *error = "there's no folder for profiles";
        return false;
    }
    const Profile* other = find(profile.name);
    if (other && other->file != profile.file) {
        *error = "there's another profile called " + profile.name;
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(m_user, code);
    if (profile.file.empty()) {
        std::string slug;
        for (const char c : profile.name) {
            slug += std::isalnum(static_cast<unsigned char>(c)) ? char(std::tolower(static_cast<unsigned char>(c))) : '_';
        }
        profile.file = m_user / (slug + ".json");
        for (int i = 2; std::filesystem::exists(profile.file, code); ++i) {
            profile.file = m_user / (slug + "_" + std::to_string(i) + ".json");
        }
    }
    // Beside it, then in its place: what's there survives a write that fails.
    std::filesystem::path temporary = profile.file;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << profile.toJson();
        if (!stream.flush()) {
            *error = "it can't be written to " + toUtf8(profile.file);
            return false;
        }
    }
    std::filesystem::rename(temporary, profile.file, code);
    if (code) {
        *error = "it can't be written to " + toUtf8(profile.file) + ": " + code.message();
        std::filesystem::remove(temporary, code);
        return false;
    }
    reload();
    return true;
}

bool ProfileLibrary::remove(const std::string& name, std::string* error) {
    const Profile* profile = find(name);
    if (!profile || profile->readOnly || profile->file.empty()) {
        *error = "it's read-only";
        return false;
    }
    std::error_code code;
    if (!std::filesystem::remove(profile->file, code)) {
        *error = "its file can't be deleted: " + code.message();
        return false;
    }
    reload();
    return true;
}

std::string ProfileLibrary::unusedName(const std::string& base) const {
    if (!find(base)) return base;
    for (int i = 2;; ++i) {
        const std::string name = base + " " + std::to_string(i);
        if (!find(name)) return name;
    }
}

// --- On the stage ------------------------------------------------------------------

std::vector<std::string> ignoredRules(const UsdPrim& prim) {
    std::vector<std::string> result;
    const VtValue value = prim ? prim.GetCustomDataByKey(kIgnoredRules) : VtValue();
    if (value.IsHolding<VtArray<std::string>>()) {
        const auto& array = value.UncheckedGet<VtArray<std::string>>();
        result.assign(array.cbegin(), array.cend());
    }
    return result;
}

bool setIgnored(const UsdPrim& prim, const std::string& rule, bool ignored) {
    if (!prim) return false;
    std::vector<std::string> list = ignoredRules(prim);
    const auto it = std::find(list.begin(), list.end(), rule);
    if ((it != list.end()) == ignored) return false;
    if (ignored) {
        list.push_back(rule);
    } else {
        list.erase(it);
    }
    if (list.empty()) {
        prim.ClearCustomDataByKey(kIgnoredRules);
        // A weaker layer's is still there, so say that there are none.
        if (ignoredRules(prim).empty()) return true;
    }
    prim.SetCustomDataByKey(kIgnoredRules, VtValue(VtArray<std::string>(list.begin(), list.end())));
    return true;
}

std::string suggestedProfile(const UsdStageRefPtr& stage) {
    if (!stage) return {};
    const VtDictionary data = stage->GetRootLayer()->GetCustomLayerData();
    const VtValue* value = data.GetValueAtPath("usdcc:" + std::string(kSuggested));
    return value && value->IsHolding<std::string>() ? value->UncheckedGet<std::string>() : std::string();
}

}  // namespace usdcc::usd::validation
