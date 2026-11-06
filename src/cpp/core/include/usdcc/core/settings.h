#pragma once

// The application's preferences (docs/SETTINGS.md): one registry of typed,
// described settings that every consumer — C++ and Python — reads and writes
// through, and that the Settings dialog builds its pages from.
//
//   auto& settings = usdcc::core::Settings::instance();
//   settings.define({.key = "Appearance/accentColor", .category = "Appearance", .group = "Theme",
//                    .label = "Accent color", .type = SettingType::Color, .defaultValue = "#3d8bfd"});
//   std::string accent = settings.get<std::string>("Appearance/accentColor");
//
// Qt-free, and the instance lives in usdcc_core's shared library so that the
// exe and the Python extension modules all see the same one (like LogHub).
// Where values are stored is a SettingsBackend; the app installs one on
// usdcc.ini (usdcc::ui::installAppSettings()), until then — and in tests —
// values only live in memory.
//
// Not thread-safe: use it from the UI thread. changed() fires synchronously.

#include "usdcc/core/export.h"
#include "usdcc/core/signal.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace usdcc::core {

enum class SettingType {
    Bool,      // bool
    Int,       // int, within [minimum, maximum] when maximum > minimum
    Float,     // double, within [minimum, maximum] when maximum > minimum
    String,    // std::string
    Choice,    // std::string, one of `choices`
    Color,     // std::string "#rrggbb" (lower case)
    Shortcut,  // std::string in QKeySequence's portable text ("W", "Ctrl+Shift+S"), "" = none
    Folder,    // std::string, a folder's path; "" = none
    // std::string, folders' paths separated by ';' ("" = none), in order; the
    // definition's fixedFolders are listed with them but aren't part of the value
    FolderList,
};

// "bool", "int", "float", "string", "choice", "color", "shortcut", "folder", "folders".
USDCC_CORE_API std::string_view settingTypeName(SettingType type);
// The names above; false if `text` is none of them.
USDCC_CORE_API bool parseSettingType(std::string_view text, SettingType* type);

using SettingValue = std::variant<bool, int, double, std::string>;

struct SettingDefinition {
    // "Group/name": also where it is stored in usdcc.ini. The group is
    // usually the category ("Appearance/accentColor", "Shortcuts/tools.move").
    std::string key;
    std::string category;  // the Settings dialog page: "Appearance", "Shortcuts", "Viewport", "Python", …
    std::string group;     // a section heading on that page ("Theme", "Tools"); "" = none
    std::string label;
    std::string description;
    SettingType type = SettingType::String;
    SettingValue defaultValue;
    double minimum = 0;  // Int/Float range, used when maximum > minimum
    double maximum = 0;
    std::vector<std::string> choices;  // Choice
    // FolderList: folders shown with the value, greyed, and not part of it: ones that
    // are always used, such as the user's and the shipped plugins.
    struct FixedFolder {
        std::string label;   // "Your plugins"
        std::string folder;
        bool after = false;  // listed after the value's folders, not before
    };
    std::vector<FixedFolder> fixedFolders;
    // A change is saved at once but only used from the next start. The dialog
    // says so while the value differs from the one this session started with.
    bool appliesAtNextStart = false;
};

// Where setting values are stored. read() returns nothing for a value that
// was never written (or was removed); Settings checks what it does return.
class SettingsBackend {
public:
    virtual ~SettingsBackend() = default;
    virtual std::optional<SettingValue> read(const SettingDefinition& definition) = 0;
    virtual void write(const SettingDefinition& definition, const SettingValue& value) = 0;
    virtual void remove(const std::string& key) = 0;
};

// Keeps values in memory: the default backend, and the one tests install.
class MemorySettingsBackend : public SettingsBackend {
public:
    USDCC_CORE_API std::optional<SettingValue> read(const SettingDefinition& definition) override;
    USDCC_CORE_API void write(const SettingDefinition& definition, const SettingValue& value) override;
    USDCC_CORE_API void remove(const std::string& key) override;

private:
    std::map<std::string, SettingValue> m_values;
};

class Settings {
public:
    // The process-wide registry every consumer uses.
    USDCC_CORE_API static Settings& instance();

    // Separate registries are for tests; the app only uses instance().
    USDCC_CORE_API Settings();
    USDCC_CORE_API ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // Replaces where values are stored and reloads every defined setting
    // from it (firing changed() for each one whose value differs). Null
    // installs a fresh MemorySettingsBackend.
    USDCC_CORE_API void setBackend(std::unique_ptr<SettingsBackend> backend);

    // Adds a setting, loading its stored value (the default if there is none
    // or it doesn't fit the definition). Defining a key again replaces its
    // description and keeps the current value if it still fits. False, and
    // nothing changes, if the definition itself is invalid: an empty key or
    // category, or a default value that doesn't fit its type/range/choices.
    USDCC_CORE_API bool define(SettingDefinition definition);

    USDCC_CORE_API bool contains(std::string_view key) const;
    // Null for an unknown key. Valid until that key is defined again.
    USDCC_CORE_API const SettingDefinition* definition(std::string_view key) const;
    // In the order they were first defined; all of them for an empty category.
    USDCC_CORE_API std::vector<const SettingDefinition*> definitions(std::string_view category = {}) const;
    // Every category that has a setting, in the order each first appeared.
    USDCC_CORE_API std::vector<std::string> categories() const;

    // The current value; an empty string for an unknown key.
    USDCC_CORE_API SettingValue value(std::string_view key) const;
    USDCC_CORE_API bool isDefault(std::string_view key) const;
    // An appliesAtNextStart setting whose value isn't the one this session
    // started with.
    USDCC_CORE_API bool isPendingNextStart(std::string_view key) const;

    // The value as T, or `fallback` for an unknown key or another type.
    template <typename T>
    T get(std::string_view key, T fallback = T{}) const {
        if (!contains(key)) {
            return fallback;
        }
        const SettingValue current = value(key);
        if (const T* typed = std::get_if<T>(&current)) {
            return *typed;
        }
        return fallback;
    }

    // Stores and announces a new value. Accepts an int for a Float setting and
    // a whole double for an Int one; a Color is stored in lower case. False,
    // and nothing changes, for an unknown key or a value that doesn't fit
    // (wrong type, out of range, not one of the choices, not "#rrggbb").
    USDCC_CORE_API bool setValue(std::string_view key, const SettingValue& value);
    // Back to the default, and removed from the backend so a later change of
    // the default in code applies.
    USDCC_CORE_API void reset(std::string_view key);
    USDCC_CORE_API void resetCategory(std::string_view category);

    // Converts `value` to what a setting of `definition` stores, or nothing if
    // it doesn't fit (the checks setValue() makes).
    USDCC_CORE_API static std::optional<SettingValue> normalized(const SettingDefinition& definition,
                                                                 const SettingValue& value);

    // A setting's value changed (its key).
    Signal<const std::string&> changed;
    // A setting was defined (its key), e.g. by a Python plugin.
    Signal<const std::string&> defined;

private:
    struct Entry {
        SettingDefinition definition;
        SettingValue value;
        SettingValue sessionValue;  // as it was when loaded
    };
    Entry* find(std::string_view key);
    const Entry* find(std::string_view key) const;
    SettingValue load(const SettingDefinition& definition);

    std::unique_ptr<SettingsBackend> m_backend;
    std::vector<std::unique_ptr<Entry>> m_entries;  // definition order
};

}  // namespace usdcc::core
