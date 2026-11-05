// usdcc::core::Settings (docs/SETTINGS.md): defining settings, typed and
// checked values, defaults and reset, change notifications, loading from a
// backend (including values that no longer fit) and the category order the
// Settings dialog lists. Each test uses its own registry, not the app's.
#include "test_harness.h"

#include "usdcc/core/settings.h"

#include <memory>
#include <string>
#include <vector>

using usdcc::core::MemorySettingsBackend;
using usdcc::core::SettingDefinition;
using usdcc::core::Settings;
using usdcc::core::SettingType;
using usdcc::core::SettingValue;

namespace {

SettingDefinition color(const std::string& key, const std::string& category = "Appearance") {
    return {.key = key, .category = category, .label = "Accent", .type = SettingType::Color,
            .defaultValue = std::string("#3d8bfd")};
}

SettingDefinition percent(const std::string& key) {
    return {.key = key, .category = "Appearance", .group = "Interface", .type = SettingType::Int,
            .defaultValue = 100, .minimum = 80, .maximum = 200};
}

}  // namespace

USDCC_TEST(settings_values) {
    Settings settings;
    CHECK(settings.define(color("Appearance/accent")), "a color setting is defined");
    CHECK(settings.get<std::string>("Appearance/accent") == "#3d8bfd", "starts at its default");
    CHECK(settings.isDefault("Appearance/accent"), "and is the default");

    std::vector<std::string> changes;
    auto connection = settings.changed.connect([&changes](const std::string& key) { changes.push_back(key); });

    CHECK(settings.setValue("Appearance/accent", std::string("#E5484D")), "a valid color is accepted");
    CHECK(settings.get<std::string>("Appearance/accent") == "#e5484d", "colors are stored in lower case");
    CHECK(!settings.isDefault("Appearance/accent"), "no longer the default");
    CHECK(changes.size() == 1 && changes[0] == "Appearance/accent", "changed() fired once (%zu)", changes.size());
    CHECK(settings.setValue("Appearance/accent", std::string("#e5484d")), "setting the same value succeeds");
    CHECK(changes.size() == 1, "but announces nothing (%zu)", changes.size());

    for (const SettingValue& bad : {SettingValue(std::string("red")), SettingValue(std::string("#12345")),
                                    SettingValue(std::string("#gggggg")), SettingValue(42), SettingValue(true)}) {
        CHECK(!settings.setValue("Appearance/accent", bad), "an invalid color is rejected");
    }
    CHECK(settings.get<std::string>("Appearance/accent") == "#e5484d", "rejected values change nothing");
    CHECK(!settings.setValue("Appearance/unknown", std::string("#000000")), "an unknown key is rejected");

    settings.define(percent("Appearance/scale"));
    CHECK(settings.setValue("Appearance/scale", 150), "an int in range is accepted");
    CHECK(settings.setValue("Appearance/scale", 120.0), "a whole double is accepted for an int");
    CHECK(settings.get<int>("Appearance/scale") == 120, "and stored as an int (got %d)", settings.get<int>("Appearance/scale"));
    CHECK(!settings.setValue("Appearance/scale", 250), "out of range is rejected");
    CHECK(!settings.setValue("Appearance/scale", 120.5), "a fractional double is rejected for an int");
    CHECK(settings.get<int>("Appearance/scale") == 120, "rejected values change nothing");

    settings.define({.key = "Viewport/fov", .category = "Viewport", .type = SettingType::Float, .defaultValue = 50.0});
    CHECK(settings.setValue("Viewport/fov", 35), "an int is accepted for a float");
    CHECK(settings.get<double>("Viewport/fov") == 35.0, "and stored as a double");

    settings.define({.key = "Viewport/mode", .category = "Viewport", .type = SettingType::Choice,
                     .defaultValue = std::string("Lit"), .choices = {"Lit", "Wireframe"}});
    CHECK(settings.setValue("Viewport/mode", std::string("Wireframe")), "one of the choices is accepted");
    CHECK(!settings.setValue("Viewport/mode", std::string("Unlit")), "anything else is rejected");

    settings.define({.key = "Viewport/grid", .category = "Viewport", .type = SettingType::Bool, .defaultValue = true});
    CHECK(!settings.setValue("Viewport/grid", 1), "an int is not a bool");
    CHECK(settings.setValue("Viewport/grid", false) && !settings.get<bool>("Viewport/grid", true), "a bool is");

    changes.clear();
    settings.reset("Appearance/accent");
    CHECK(settings.isDefault("Appearance/accent") && changes.size() == 1, "reset() restores the default and announces it");
    settings.resetCategory("Viewport");
    CHECK(settings.isDefault("Viewport/fov") && settings.isDefault("Viewport/mode") && settings.isDefault("Viewport/grid"),
          "resetCategory() resets the whole category");
    CHECK(!settings.isDefault("Appearance/scale"), "and only that category");

    CHECK(settings.get<int>("Appearance/accent", -1) == -1, "get<T>() of another type gives the fallback");
    CHECK(settings.get<std::string>("Nothing/here", "x") == "x", "get<T>() of an unknown key gives the fallback");
}

USDCC_TEST(settings_definitions) {
    Settings settings;
    CHECK(!settings.define({.key = "", .category = "A", .type = SettingType::Bool, .defaultValue = false}),
          "a definition without a key is refused");
    CHECK(!settings.define({.key = "A/b", .category = "", .type = SettingType::Bool, .defaultValue = false}),
          "a definition without a category is refused");
    CHECK(!settings.define({.key = "A/b", .category = "A", .type = SettingType::Int, .defaultValue = 500,
                            .minimum = 0, .maximum = 10}),
          "a default out of its own range is refused");
    CHECK(!settings.define({.key = "A/b", .category = "A", .type = SettingType::Choice,
                            .defaultValue = std::string("x"), .choices = {"y"}}),
          "a default that isn't one of its choices is refused");
    CHECK(!settings.contains("A/b"), "nothing was defined");

    int defined = 0;
    auto connection = settings.defined.connect([&defined](const std::string&) { ++defined; });
    settings.define(color("Shortcuts/x", "Shortcuts"));
    settings.define(percent("Appearance/scale"));
    settings.define(color("Appearance/accent"));
    CHECK(defined == 3, "defined() fired for each (%d)", defined);
    const std::vector<std::string> categories = settings.categories();
    CHECK(categories == std::vector<std::string>({"Shortcuts", "Appearance"}), "categories in first-defined order");
    CHECK(settings.definitions("Appearance").size() == 2 && settings.definitions().size() == 3,
          "definitions() per category and in all");
    CHECK(settings.definitions("Appearance")[0]->key == "Appearance/scale", "in definition order");

    settings.setValue("Appearance/scale", 150);
    SettingDefinition redefined = percent("Appearance/scale");
    redefined.label = "UI scale";
    CHECK(settings.define(redefined), "a key can be defined again");
    CHECK(settings.definition("Appearance/scale")->label == "UI scale", "which replaces its description");
    CHECK(settings.get<int>("Appearance/scale") == 150, "and keeps a value that still fits");
    CHECK(settings.definitions().size() == 3, "without adding a setting");
    redefined.maximum = 120;
    settings.define(redefined);
    CHECK(settings.get<int>("Appearance/scale") == 100, "a value that no longer fits falls back to the default");

    SettingType type{};
    CHECK(usdcc::core::parseSettingType("shortcut", &type) && type == SettingType::Shortcut, "type names parse");
    CHECK(!usdcc::core::parseSettingType("colour", &type), "unknown type names don't");
    CHECK(usdcc::core::settingTypeName(SettingType::Color) == "color", "and print");
    CHECK(usdcc::core::parseSettingType("folder", &type) && type == SettingType::Folder, "folder too");
}

USDCC_TEST(settings_folder_and_next_start) {
    Settings settings;
    CHECK(settings.define({.key = "A/folder", .category = "A", .type = SettingType::Folder, .defaultValue = std::string("C:/x")}),
          "a folder setting");
    CHECK(settings.setValue("A/folder", std::string("C:/y")) && settings.get<std::string>("A/folder") == "C:/y", "holds a path");
    CHECK(!settings.setValue("A/folder", 3), "and only text");
    CHECK(!settings.isPendingNextStart("A/folder"), "without the flag, a change is never pending");

    settings.define({.key = "A/later", .category = "A", .type = SettingType::Bool, .defaultValue = false, .appliesAtNextStart = true});
    CHECK(!settings.isPendingNextStart("A/later"), "nothing is pending to begin with");
    settings.setValue("A/later", true);
    CHECK(settings.isPendingNextStart("A/later"), "a change is");
    settings.setValue("A/later", false);
    CHECK(!settings.isPendingNextStart("A/later"), "and isn't once it's back");
    CHECK(!settings.isPendingNextStart("A/none"), "an unknown key isn't");
}

USDCC_TEST(settings_backend) {
    auto backend = std::make_unique<MemorySettingsBackend>();
    MemorySettingsBackend* stored = backend.get();
    SettingDefinition accent = color("Appearance/accent");
    SettingDefinition scale = percent("Appearance/scale");
    stored->write(accent, std::string("#00FF00"));
    stored->write(scale, std::string("not a number"));

    Settings settings;
    settings.setBackend(std::move(backend));
    settings.define(accent);
    settings.define(scale);
    CHECK(settings.get<std::string>("Appearance/accent") == "#00ff00", "a stored value loads when defined");
    CHECK(settings.get<int>("Appearance/scale") == 100, "a stored value that doesn't fit falls back to the default");

    settings.setValue("Appearance/scale", 130);
    CHECK(std::get<int>(*stored->read(scale)) == 130, "changes are written through");
    settings.reset("Appearance/accent");
    CHECK(!stored->read(accent).has_value(), "reset() removes the stored value");

    std::vector<std::string> changes;
    auto connection = settings.changed.connect([&changes](const std::string& key) { changes.push_back(key); });
    settings.setBackend(nullptr);
    CHECK(settings.get<int>("Appearance/scale") == 100, "a new (empty) backend reloads every setting");
    CHECK(changes == std::vector<std::string>({"Appearance/scale"}), "announcing only those that changed");
}
