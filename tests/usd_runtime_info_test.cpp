// Help > USD Runtime and Settings > USD (docs/USD_RUNTIME.md): what's collected
// about the OpenUSD the app runs with, TF_DEBUG's flags, and how the settings
// become environment variables.
#include "test_harness.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/usd/arnold_runtime.h"
#include "usdcc/usd/usd_environment.h"
#include "usdcc/usd/usd_runtime_info.h"

#include <pxr/base/arch/env.h>
#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/pxr.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace usdcc::usd;

namespace {
const UsdRuntimeInfo::Row* row(const UsdRuntimeInfo& info, const std::string& section, const std::string& name) {
    for (const auto& s : info.overview) {
        if (s.title != section) continue;
        for (const auto& r : s.rows) {
            if (r.name == name) return &r;
        }
    }
    return nullptr;
}

// What was logged, to count the records that say something.
struct Collect : usdcc::core::LogSink {
    std::mutex mutex;
    std::vector<std::string> messages;
    void onRecord(const usdcc::core::LogRecord& record) override {
        std::lock_guard lock(mutex);
        messages.push_back(record.message);
    }
    int count(const std::string& text) {
        std::lock_guard lock(mutex);
        int n = 0;
        for (const std::string& message : messages) {
            if (message.find(text) != std::string::npos) ++n;
        }
        return n;
    }
};
}  // namespace

USDCC_TEST(usd_runtime_info) {
    const UsdRuntimeInfo info = collectUsdRuntimeInfo();
    const std::string version =
        std::to_string(PXR_MAJOR_VERSION) + "." + std::to_string(PXR_MINOR_VERSION) + "." + std::to_string(PXR_PATCH_VERSION);
    const auto* versionRow = row(info, "OpenUSD", "Version");
    CHECK(versionRow && versionRow->value == version, "the OpenUSD version: %s", versionRow ? versionRow->value.c_str() : "none");
    CHECK(row(info, "Asset resolution", "Primary resolver") && row(info, "Threads", "Worker threads") &&
              row(info, "Shading", "Shader nodes"),
          "the resolver, the threads and the shader registry");

    const auto usd = std::find_if(info.plugins.begin(), info.plugins.end(), [](const auto& p) { return p.name == "usd"; });
    CHECK(usd != info.plugins.end() && usd->type == "library" && !usd->path.empty(), "the usd plugin, a library");
    CHECK(std::is_sorted(info.plugins.begin(), info.plugins.end(), [](const auto& a, const auto& b) { return a.name < b.name; }),
          "plugins by name");

    const auto usda = std::find_if(info.fileFormats.begin(), info.fileFormats.end(),
                                   [](const auto& f) { return f.formatId == "usda"; });
    CHECK(usda != info.fileFormats.end() && usda->extensions == std::vector<std::string>{"usda"} && usda->reading &&
              usda->writing && usda->target == "usd" && !usda->plugin.empty(),
          "usda, from the usd plugin's metadata: %s", usda == info.fileFormats.end() ? "none" :
          (usda->formatId + " [" + (usda->extensions.empty() ? std::string() : usda->extensions[0]) + "] " + usda->target + " " +
           std::to_string(usda->reading) + std::to_string(usda->writing) + " " + usda->plugin).c_str());
    CHECK(std::any_of(info.renderers.begin(), info.renderers.end(),
                      [](const auto& r) { return r.id == "HdStormRendererPlugin" && r.displayName == "GL"; }),
          "Storm among the render delegates");

    for (const auto& variable : info.environment) {
        CHECK(isUsdEnvironmentVariable(variable.name), "only USD variables: %s", variable.name.c_str());
    }
    CHECK(isUsdEnvironmentVariable("PXR_PLUGINPATH_NAME") && isUsdEnvironmentVariable("HDST_ENABLE_X") &&
              isUsdEnvironmentVariable("USDCC_LOG") && isUsdEnvironmentVariable("TF_DEBUG"),
          "PXR_, HD, USD and TF_ variables are USD's");
    CHECK(!isUsdEnvironmentVariable("PATH") && !isUsdEnvironmentVariable("HOMEPATH") && !isUsdEnvironmentVariable(""),
          "PATH and HOMEPATH aren't");

    const std::string text = usdRuntimeInfoText(info);
    CHECK(text.find("OpenUSD\n  Version: " + version + "\n") == 0, "the text starts with the version: %s",
          text.substr(0, 40).c_str());
    CHECK(text.find("\nPlugins (") != std::string::npos && text.find("\nFile formats (") != std::string::npos &&
              text.find("\nRender delegates (") != std::string::npos && text.find("\nEnvironment (") != std::string::npos,
          "and has every section");
}

USDCC_TEST(usd_debug_flags) {
    const std::vector<UsdDebugFlag> flags = usdDebugFlags();
    const auto flag = std::find_if(flags.begin(), flags.end(), [](const auto& f) { return f.name == "AR_RESOLVER_INIT"; });
    CHECK(flag != flags.end() && !flag->description.empty(), "TfDebug's flags, with their descriptions");
    if (flag == flags.end()) return;
    const bool was = flag->enabled;
    CHECK(setUsdDebugFlag(flag->name, !was), "a flag turns on or off");
    const auto changed = usdDebugFlags();
    CHECK(std::find_if(changed.begin(), changed.end(), [&](const auto& f) { return f.name == flag->name; })->enabled == !was,
          "at once");
    setUsdDebugFlag(flag->name, was);
    CHECK(!setUsdDebugFlag("NOT_A_FLAG_USDCC", true), "an unknown flag isn't set");
    CHECK(!setUsdDebugFlag("AR_*", true), "nor a pattern");
}

USDCC_TEST(usd_environment_settings) {
    const std::string sep = ARCH_PATH_LIST_SEP;
    CHECK(mergeSearchPaths("", "") == "", "nothing from nothing");
    // No drive letters: the environment's list is split on the platform's
    // separator, which is ':' outside Windows.
    CHECK(mergeSearchPaths("", "/a;/b") == "/a" + sep + "/b", "the setting's folders");
    const std::string environment = "/x" + sep + "/y";
    CHECK(mergeSearchPaths(environment, "/a;/x") == environment + sep + "/a",
          "after the environment's, without the ones already there: %s",
          mergeSearchPaths(environment, "/a;/x").c_str());

    usdcc::core::Settings settings;
    defineUsdEnvironmentSettings(settings);
    std::vector<std::string> pages = settings.categories();
    std::sort(pages.begin(), pages.end());
    CHECK((pages == std::vector<std::string>{"Renderers", "USD"}), "on the USD page, and the Arnold folder on Renderers");
    CHECK(settings.definition(kUsdArnoldFolderKey) && settings.definition(kUsdArnoldFolderKey)->group == "Arnold",
          "in the Arnold group, where Arnold's own settings go");
    for (const auto* definition : settings.definitions()) {
        CHECK(definition->appliesAtNextStart, "%s applies at the next start", definition->key.c_str());
    }

    // The MaterialX one, put back afterwards: nothing else here reads it.
    const char* variable = "PXR_MTLX_PLUGIN_SEARCH_PATHS";
    const bool had = PXR_NS::ArchHasEnv(variable);
    const std::string before = PXR_NS::ArchGetEnv(variable);
    PXR_NS::ArchRemoveEnv(variable);
    settings.setValue(kUsdMaterialXFoldersKey, std::string("C:/nodes"));
    applyUsdEnvironmentSettings(settings);
    CHECK(PXR_NS::ArchGetEnv(variable) == "C:/nodes", "a folder setting sets its variable: %s",
          PXR_NS::ArchGetEnv(variable).c_str());
    CHECK(usdEnvironmentSource(variable) == "Settings", "from Settings");
    CHECK(usdEnvironmentSource("PXR_PLUGINPATH_NAME").empty(), "an empty setting leaves its variable alone");
    const auto info = collectUsdRuntimeInfo();
    const auto shown = std::find_if(info.environment.begin(), info.environment.end(), [&](const auto& v) { return v.name == variable; });
    CHECK(shown != info.environment.end() && shown->value == "C:/nodes" && shown->source == "Settings",
          "the runtime info shows it, and where it's from");
#if defined(_WIN32)
    _putenv_s(variable, had ? before.c_str() : "");  // "" removes it
#else
    if (had) {
        setenv(variable, before.c_str(), 1);
    } else {
        unsetenv(variable);
    }
#endif

    // The Cycles device: Metal for "GPU", Cycles' own CPU default otherwise.
    CHECK(cyclesDeviceVariable("GPU") == "METAL" && cyclesDeviceVariable("CPU").empty(), "Cycles device values");
#if defined(__APPLE__)
    {
        const auto* device = settings.definition(kUsdCyclesDeviceKey);
        CHECK(device && device->group == "Cycles" && std::get<std::string>(device->defaultValue) == "CPU",
              "the Cycles device, CPU by default, with Cycles' own settings");
        const char* cycles = "CYCLES_DEVICE";
        const bool hadDevice = PXR_NS::ArchHasEnv(cycles);
        const std::string deviceBefore = PXR_NS::ArchGetEnv(cycles);
        unsetenv(cycles);
        settings.setValue(kUsdCyclesDeviceKey, std::string("GPU"));
        applyUsdEnvironmentSettings(settings);
        CHECK(PXR_NS::ArchGetEnv(cycles) == "METAL" && usdEnvironmentSource(cycles) == "Settings",
              "GPU sets CYCLES_DEVICE=METAL: %s", PXR_NS::ArchGetEnv(cycles).c_str());
        setenv(cycles, "CPU", 1);
        applyUsdEnvironmentSettings(settings);
        CHECK(PXR_NS::ArchGetEnv(cycles) == "CPU" && usdEnvironmentSource(cycles).empty(),
              "the environment's CYCLES_DEVICE wins");
        unsetenv(cycles);
        settings.setValue(kUsdCyclesDeviceKey, std::string("CPU"));
        applyUsdEnvironmentSettings(settings);
        CHECK(!PXR_NS::ArchHasEnv(cycles), "CPU leaves it unset, Cycles' default");
        if (hadDevice) setenv(cycles, deviceBefore.c_str(), 1);
    }
#else
    CHECK(!settings.definition(kUsdCyclesDeviceKey), "no Cycles device setting where the pack has no GPU device");
#endif

    if (PXR_NS::ArchHasEnv("PXR_WORK_THREAD_LIMIT")) return;
    const unsigned threads = PXR_NS::WorkGetConcurrencyLimit();
    settings.setValue(kUsdMaterialXFoldersKey, std::string());
    settings.setValue(kUsdThreadLimitKey, 2);
    applyUsdEnvironmentSettings(settings);
    // Whether TBB takes it depends on whether it has started its threads, which
    // the tests above have done; the app applies it before. So only that it's asked for.
    CHECK(usdEnvironmentSource("PXR_WORK_THREAD_LIMIT") == "Settings" && usdEnvironmentSource(variable).empty(),
          "the thread limit, from Settings");
    PXR_NS::WorkSetConcurrencyLimit(threads);
}

// Settings > USD's plugin folders reach OpenUSD's plugin registry, not only the
// variable: OpenUSD reads PXR_PLUGINPATH_NAME when its libraries load, which in
// this program, as in the app, is long before the setting is applied.
USDCC_TEST(usd_plugin_folders_setting) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "usdcc_plugin_folder_setting_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "plugInfo.json")
        << R"({"Plugins": [{"Type": "resource", "Name": "usdccPluginFolderSettingTest", "Root": ".", "ResourcePath": ".", "Info": {}}]})";
    CHECK(!PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccPluginFolderSettingTest"), "not there before");

    const char* variable = "PXR_PLUGINPATH_NAME";
    const bool had = PXR_NS::ArchHasEnv(variable);
    const std::string before = PXR_NS::ArchGetEnv(variable);
    usdcc::core::Settings settings;
    defineUsdEnvironmentSettings(settings);
    settings.setValue(kUsdPluginFoldersKey, folder.generic_string());
    applyUsdEnvironmentSettings(settings);
    CHECK(PXR_NS::ArchGetEnv(variable).find(folder.generic_string()) != std::string::npos, "the variable has it");
    CHECK(PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("usdccPluginFolderSettingTest"),
          "and OpenUSD's plugin registry has its plugin");
    applyUsdEnvironmentSettings(settings);  // again: nothing registered twice, no error

#if defined(_WIN32)
    _putenv_s(variable, had ? before.c_str() : "");
#else
    if (had) {
        setenv(variable, before.c_str(), 1);
    } else {
        unsetenv(variable);
    }
#endif
    std::filesystem::remove_all(folder);
}

// Settings > Renderers > Arnold's folder (docs/USD_PLUGINS.md §3), without an Arnold:
// the version rule, where the pack is looked for, and what stops it early.
USDCC_TEST(arnold_runtime) {
    CHECK(arnoldVersionFits("7.5.4.0", "7.5.4.0"), "the Arnold it was built for");
    CHECK(arnoldVersionFits("7.5.4.1", "7.5.4.0") && arnoldVersionFits("7.5.10.0", "7.5.4.0"),
          "a later 7.5, compared as numbers");
    CHECK(!arnoldVersionFits("7.5.2.0", "7.5.4.0"), "not an earlier one: its core lacks what the plugins call");
    CHECK(!arnoldVersionFits("7.6.0.0", "7.5.4.0") && !arnoldVersionFits("8.0.0.0", "7.5.4.0"), "not another 7.x");
    CHECK(!arnoldVersionFits("", "7.5.4.0") && !arnoldVersionFits("7.5.4.0", ""), "not an unknown version");

    const std::filesystem::path root = std::filesystem::u8path(usdRootFolder());
    CHECK(std::filesystem::is_regular_file(root / "plugin" / "usd" / "plugInfo.json"),
          "OpenUSD's root, with its plugin/usd: %s", root.generic_string().c_str());

    const std::string before = PXR_NS::ArchGetEnv("PXR_PLUGINPATH_NAME");
    const ArnoldStatus off = enableArnold("");
    CHECK(!off.enabled && off.message.empty(), "no folder, nothing done and nothing said");

    const std::filesystem::path fake = std::filesystem::temp_directory_path() / "usdcc_arnold_runtime_test";
    std::filesystem::remove_all(fake);
    std::filesystem::create_directories(fake / "arnold");
    const std::string fakeRoot = (fake / "usd").generic_string();
    const ArnoldStatus noPack = enableArnold((fake / "arnold").generic_string(), fakeRoot);
#if defined(_WIN32)
    const char* const coreName = "bin/ai.dll";
#elif defined(__APPLE__)
    const char* const coreName = "bin/libai.dylib";
#else
    const char* const coreName = "bin/libai.so";
#endif
    CHECK(!noPack.enabled && noPack.noPlugins && noPack.message.find("no Arnold plugins") != std::string::npos,
          "no pack: %s", noPack.message.c_str());
    CHECK(noPack.message.find("View > Plugins > Packs") != std::string::npos, "and where to get one: %s", noPack.message.c_str());
    std::filesystem::create_directories(fake / "usd" / "plugin" / "arnold");
    std::filesystem::create_directories(fake / "usd" / "usd-plugins");
    std::ofstream(fake / "usd" / "plugin" / "arnold" / "plugInfo.json") << R"({"Includes": ["*/resources/"]})";
    std::ofstream(fake / "usd" / "usd-plugins" / "arnold.json") << R"({"pack": "arnold", "version": "7.5.4.0"})";
    const ArnoldStatus noCore = enableArnold((fake / "arnold").generic_string(), fakeRoot);
    CHECK(!noCore.enabled && !noCore.noPlugins && noCore.packVersion == "7.5.4.0" &&
              noCore.message.find(coreName) != std::string::npos,
          "the pack's version read, and no Arnold core: %s", noCore.message.c_str());

    // A pack usdcc downloaded is laid out the same, in its own folder (docs/USD_PLUGINS.md §10):
    // it gets its turn only when the settings named an Arnold folder and OpenUSD's own
    // root had no plugins for it.
    const auto sink = std::make_shared<Collect>();
    Collect& captured = *sink;
    usdcc::core::LogHub::instance().addSink(sink, false);
    usdcc::core::Settings packSettings;
    defineUsdEnvironmentSettings(packSettings);
    applyUsdEnvironmentSettings(packSettings);
    CHECK(!enableArnoldPack(""), "no Arnold folder and no pack: nothing to do");
    CHECK(captured.count("Arnold") == 0, "and nothing to say");
    applyUsdEnvironmentSettings(packSettings);
    CHECK(!enableArnoldPack(fakeRoot), "no Arnold folder: the pack isn't tried");
    CHECK(captured.count("The Arnold pack is installed, and Arnold is off") == 1, "but the log says what it waits for");
    packSettings.setValue(kUsdArnoldFolderKey, (fake / "arnold").generic_string());
    applyUsdEnvironmentSettings(packSettings);
    if (std::filesystem::is_regular_file(root / "plugin" / "arnold" / "plugInfo.json")) {
        // This OpenUSD has an Arnold pack of its own (a local build): that one is used.
        CHECK(!enableArnoldPack(fakeRoot), "the pack beside OpenUSD comes first");
    } else {
        CHECK(captured.count("no Arnold plugins") == 0, "nothing said yet: a downloaded pack may have them");
        CHECK(!enableArnoldPack(fakeRoot), "the pack is tried, and Arnold stays off without a core");
        CHECK(captured.count(coreName) == 1, "which is what the log says, for the pack's folder");
        CHECK(!enableArnoldPack(fakeRoot), "once: a second call does nothing");
        CHECK(captured.count(coreName) == 1, "and says nothing");
        applyUsdEnvironmentSettings(packSettings);
        CHECK(!enableArnoldPack(""), "no pack either");
        CHECK(captured.count("no Arnold plugins") == 1, "then the folder is said to have no plugins to go with");
    }
    usdcc::core::LogHub::instance().removeSink(sink.get());
    CHECK(PXR_NS::ArchGetEnv("PXR_PLUGINPATH_NAME") == before, "the plugin path left alone when Arnold stays off");
    std::filesystem::remove_all(fake);
}
