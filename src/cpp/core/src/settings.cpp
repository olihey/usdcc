#include "usdcc/core/settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace usdcc::core {

namespace {

constexpr std::array<std::pair<SettingType, std::string_view>, 8> kTypeNames = {{
    {SettingType::Bool, "bool"},
    {SettingType::Int, "int"},
    {SettingType::Float, "float"},
    {SettingType::String, "string"},
    {SettingType::Choice, "choice"},
    {SettingType::Color, "color"},
    {SettingType::Shortcut, "shortcut"},
    {SettingType::Folder, "folder"},
}};

bool hasRange(const SettingDefinition& definition) { return definition.maximum > definition.minimum; }

bool inRange(const SettingDefinition& definition, double value) {
    return !hasRange(definition) || (value >= definition.minimum && value <= definition.maximum);
}

// "#rrggbb" in lower case, or nothing.
std::optional<std::string> normalizedColor(const std::string& text) {
    if (text.size() != 7 || text[0] != '#') {
        return std::nullopt;
    }
    std::string result = "#";
    for (std::size_t i = 1; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (!std::isxdigit(c)) {
            return std::nullopt;
        }
        result += static_cast<char>(std::tolower(c));
    }
    return result;
}

}  // namespace

std::string_view settingTypeName(SettingType type) {
    for (const auto& [candidate, name] : kTypeNames) {
        if (candidate == type) {
            return name;
        }
    }
    return "string";
}

bool parseSettingType(std::string_view text, SettingType* type) {
    for (const auto& [candidate, name] : kTypeNames) {
        if (name == text) {
            *type = candidate;
            return true;
        }
    }
    return false;
}

std::optional<SettingValue> MemorySettingsBackend::read(const SettingDefinition& definition) {
    const auto it = m_values.find(definition.key);
    if (it == m_values.end()) {
        return std::nullopt;
    }
    return it->second;
}

void MemorySettingsBackend::write(const SettingDefinition& definition, const SettingValue& value) {
    m_values[definition.key] = value;
}

void MemorySettingsBackend::remove(const std::string& key) { m_values.erase(key); }

Settings& Settings::instance() {
    static Settings settings;
    return settings;
}

Settings::Settings() : m_backend(std::make_unique<MemorySettingsBackend>()) {}

Settings::~Settings() = default;

std::optional<SettingValue> Settings::normalized(const SettingDefinition& definition, const SettingValue& value) {
    switch (definition.type) {
    case SettingType::Bool:
        if (const bool* b = std::get_if<bool>(&value)) {
            return *b;
        }
        return std::nullopt;
    case SettingType::Int: {
        double number = 0;
        if (const int* i = std::get_if<int>(&value)) {
            number = *i;
        } else if (const double* d = std::get_if<double>(&value)) {
            if (!std::isfinite(*d) || std::trunc(*d) != *d || *d < std::numeric_limits<int>::min() ||
                *d > std::numeric_limits<int>::max()) {
                return std::nullopt;
            }
            number = *d;
        } else {
            return std::nullopt;
        }
        if (!inRange(definition, number)) {
            return std::nullopt;
        }
        return static_cast<int>(number);
    }
    case SettingType::Float: {
        double number = 0;
        if (const double* d = std::get_if<double>(&value)) {
            number = *d;
        } else if (const int* i = std::get_if<int>(&value)) {
            number = *i;
        } else {
            return std::nullopt;
        }
        if (!std::isfinite(number) || !inRange(definition, number)) {
            return std::nullopt;
        }
        return number;
    }
    case SettingType::String:
    case SettingType::Shortcut:
    case SettingType::Folder:
        if (const std::string* s = std::get_if<std::string>(&value)) {
            return *s;
        }
        return std::nullopt;
    case SettingType::Choice:
        if (const std::string* s = std::get_if<std::string>(&value)) {
            if (std::find(definition.choices.begin(), definition.choices.end(), *s) != definition.choices.end()) {
                return *s;
            }
        }
        return std::nullopt;
    case SettingType::Color:
        if (const std::string* s = std::get_if<std::string>(&value)) {
            if (auto color = normalizedColor(*s)) {
                return std::move(*color);
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

Settings::Entry* Settings::find(std::string_view key) {
    for (auto& entry : m_entries) {
        if (entry->definition.key == key) {
            return entry.get();
        }
    }
    return nullptr;
}

const Settings::Entry* Settings::find(std::string_view key) const {
    return const_cast<Settings*>(this)->find(key);
}

SettingValue Settings::load(const SettingDefinition& definition) {
    if (auto stored = m_backend->read(definition)) {
        if (auto value = normalized(definition, *stored)) {
            return std::move(*value);
        }
    }
    return definition.defaultValue;
}

void Settings::setBackend(std::unique_ptr<SettingsBackend> backend) {
    m_backend = backend ? std::move(backend) : std::make_unique<MemorySettingsBackend>();
    std::vector<std::string> changedKeys;
    for (auto& entry : m_entries) {
        SettingValue value = load(entry->definition);
        entry->sessionValue = value;
        if (value != entry->value) {
            entry->value = std::move(value);
            changedKeys.push_back(entry->definition.key);
        }
    }
    for (const auto& key : changedKeys) {
        changed(key);
    }
}

bool Settings::define(SettingDefinition definition) {
    if (definition.key.empty() || definition.category.empty()) {
        return false;
    }
    auto defaultValue = normalized(definition, definition.defaultValue);
    if (!defaultValue) {
        return false;
    }
    definition.defaultValue = std::move(*defaultValue);
    const std::string key = definition.key;

    if (Entry* entry = find(key)) {
        auto kept = normalized(definition, entry->value);
        entry->definition = std::move(definition);
        if (kept) {
            entry->value = std::move(*kept);
        } else {
            entry->value = load(entry->definition);
            changed(key);
        }
    } else {
        auto added = std::make_unique<Entry>();
        added->value = load(definition);
        added->sessionValue = added->value;
        added->definition = std::move(definition);
        m_entries.push_back(std::move(added));
    }
    defined(key);
    return true;
}

bool Settings::contains(std::string_view key) const { return find(key) != nullptr; }

const SettingDefinition* Settings::definition(std::string_view key) const {
    const Entry* entry = find(key);
    return entry ? &entry->definition : nullptr;
}

std::vector<const SettingDefinition*> Settings::definitions(std::string_view category) const {
    std::vector<const SettingDefinition*> result;
    for (const auto& entry : m_entries) {
        if (category.empty() || entry->definition.category == category) {
            result.push_back(&entry->definition);
        }
    }
    return result;
}

std::vector<std::string> Settings::categories() const {
    std::vector<std::string> result;
    for (const auto& entry : m_entries) {
        if (std::find(result.begin(), result.end(), entry->definition.category) == result.end()) {
            result.push_back(entry->definition.category);
        }
    }
    return result;
}

SettingValue Settings::value(std::string_view key) const {
    const Entry* entry = find(key);
    return entry ? entry->value : SettingValue(std::string());
}

bool Settings::isDefault(std::string_view key) const {
    const Entry* entry = find(key);
    return !entry || entry->value == entry->definition.defaultValue;
}

bool Settings::isPendingNextStart(std::string_view key) const {
    const Entry* entry = find(key);
    return entry && entry->definition.appliesAtNextStart && entry->value != entry->sessionValue;
}

bool Settings::setValue(std::string_view key, const SettingValue& value) {
    Entry* entry = find(key);
    if (!entry) {
        return false;
    }
    auto checked = normalized(entry->definition, value);
    if (!checked) {
        return false;
    }
    if (*checked == entry->value) {
        return true;
    }
    entry->value = std::move(*checked);
    m_backend->write(entry->definition, entry->value);
    const std::string changedKey = entry->definition.key;
    changed(changedKey);
    return true;
}

void Settings::reset(std::string_view key) {
    Entry* entry = find(key);
    if (!entry) {
        return;
    }
    m_backend->remove(entry->definition.key);
    if (entry->value != entry->definition.defaultValue) {
        entry->value = entry->definition.defaultValue;
        const std::string changedKey = entry->definition.key;
        changed(changedKey);
    }
}

void Settings::resetCategory(std::string_view category) {
    for (const SettingDefinition* definition : definitions(category)) {
        reset(definition->key);
    }
}

}  // namespace usdcc::core
