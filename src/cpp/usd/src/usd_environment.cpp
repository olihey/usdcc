#include "usdcc/usd/usd_environment.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/arnold_runtime.h"

#include <pxr/base/arch/defines.h>
#include <pxr/base/arch/env.h>
#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/work/threadLimits.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string_view>
#include <vector>

namespace usdcc::usd {

namespace {

constexpr const char* kCategory = "USD";
constexpr const char* kThreadLimitVariable = "PXR_WORK_THREAD_LIMIT";

struct PathVariable {
    const char* key;
    const char* variable;
};
constexpr PathVariable kPathVariables[] = {
    {kUsdPluginFoldersKey, "PXR_PLUGINPATH_NAME"},
    {kUsdAssetSearchPathsKey, "PXR_AR_DEFAULT_SEARCH_PATH"},
    {kUsdMaterialXFoldersKey, "PXR_MTLX_PLUGIN_SEARCH_PATHS"},
};

std::map<std::string, std::string, std::less<>>& sources() {
    static std::map<std::string, std::string, std::less<>> map;
    return map;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find(separator, start), text.size());
        if (end > start) parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

// In the C runtime's copy of the environment too, not only Windows' own
// (ArchSetEnv), so Python's os.environ and child processes see it.
void setVariable(const std::string& name, const std::string& value) {
#if defined(ARCH_OS_WINDOWS)
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

}  // namespace

void defineUsdEnvironmentSettings(core::Settings& settings) {
    using core::SettingType;
    settings.define({.key = kUsdPluginFoldersKey,
                     .category = kCategory,
                     .group = "Plugins",
                     .label = "USD plugin folders",
                     .description = "Folders with OpenUSD plugins (a plugInfo.json each, or in their subfolders): schemas, "
                                    "file formats, asset resolvers, render delegates. Added to PXR_PLUGINPATH_NAME.",
                     .type = SettingType::FolderList,
                     .defaultValue = std::string(),
                     .appliesAtNextStart = true});
    settings.define({.key = kUsdAssetSearchPathsKey,
                     .category = kCategory,
                     .group = "Assets",
                     .label = "Asset search paths",
                     .description = "Where an asset path that isn't relative or absolute, such as \"props/chair.usd\", is "
                                    "looked for, in order, when no layer next to it has it. Added to "
                                    "PXR_AR_DEFAULT_SEARCH_PATH.",
                     .type = SettingType::FolderList,
                     .defaultValue = std::string(),
                     .appliesAtNextStart = true});
    settings.define({.key = kUsdMaterialXFoldersKey,
                     .category = kCategory,
                     .group = "Shading",
                     .label = "MaterialX node folders",
                     .description = "Folders with your own MaterialX node definitions (.mtlx), for the material editor and "
                                    "the viewport. Added to PXR_MTLX_PLUGIN_SEARCH_PATHS.",
                     .type = SettingType::FolderList,
                     .defaultValue = std::string(),
                     .appliesAtNextStart = true});
    settings.define({.key = kUsdThreadLimitKey,
                     .category = kCategory,
                     .group = "Performance",
                     .label = "Thread limit",
                     .description = "How many threads OpenUSD works with: 0 for one per core, a negative number for all but "
                                    "that many. PXR_WORK_THREAD_LIMIT wins when it's set.",
                     .type = SettingType::Int,
                     .defaultValue = 0,
                     .minimum = -256,
                     .maximum = 1024,
                     .appliesAtNextStart = true});
    // On the Renderers page, where Arnold's own settings join it once it's on
    // (renderer_settings.cpp groups them by the delegate's name, "Arnold").
    settings.define({.key = kUsdArnoldFolderKey,
                     .category = "Renderers",
                     .group = "Arnold",
                     .label = "Arnold folder",
                     .description = "An Arnold SDK or install (the folder with bin/ai.dll), to render with Arnold in the "
                                    "viewport. Empty: Arnold is off. Needs usdcc's Arnold plugins and the Arnold they were "
                                    "built for or a later one of the same 7.x. Without an Arnold licence, renders have a "
                                    "watermark.",
                     .type = SettingType::Folder,
                     .defaultValue = std::string(),
                     .appliesAtNextStart = true});
}

std::string mergeSearchPaths(std::string_view environment, std::string_view setting) {
    std::vector<std::string> folders = split(environment, ARCH_PATH_LIST_SEP[0]);
    for (std::string& folder : split(setting, ';')) {
        if (std::find(folders.begin(), folders.end(), folder) == folders.end()) folders.push_back(std::move(folder));
    }
    std::string merged;
    for (const std::string& folder : folders) {
        if (!merged.empty()) merged += ARCH_PATH_LIST_SEP;
        merged += folder;
    }
    return merged;
}

namespace {

void applyThreadLimit(const core::Settings& settings) {
    const int threadLimit = settings.get<int>(kUsdThreadLimitKey);
    if (threadLimit == 0) return;
    if (!PXR_NS::ArchGetEnv(kThreadLimitVariable).empty()) {
        USDCC_LOG_INFO("usdcc.usd", "The thread limit in Settings > USD is ignored, as {} is set", kThreadLimitVariable);
        return;
    }
    // Not the variable: OpenUSD reads that one while its libraries load. With
    // the TBB before oneTBB this only works before anything has used TBB on
    // this thread, which is why it's applied so early.
    PXR_NS::WorkSetConcurrencyLimitArgument(threadLimit);
    sources()[kThreadLimitVariable] = "Settings";
    const int physical = static_cast<int>(PXR_NS::WorkGetPhysicalConcurrencyLimit());
    const unsigned wanted = static_cast<unsigned>(threadLimit > 0 ? threadLimit : std::max(1, threadLimit + physical));
    const unsigned threads = PXR_NS::WorkGetConcurrencyLimit();
    if (threads == wanted) {
        USDCC_LOG_INFO("usdcc.usd", "OpenUSD works with {} threads (Settings > USD)", threads);
    } else {
        USDCC_LOG_WARNING("usdcc.usd",
                          "OpenUSD works with {} threads, not the {} of Settings > USD: it had started its threads already",
                          threads, wanted);
    }
}

}  // namespace

void applyUsdEnvironmentSettings(const core::Settings& settings) {
    sources().clear();
    std::vector<std::string> newPluginFolders;
    for (const PathVariable& entry : kPathVariables) {
        const std::string setting = settings.get<std::string>(entry.key);
        if (setting.empty()) continue;
        const std::string inherited = PXR_NS::ArchGetEnv(entry.variable);
        const std::string value = mergeSearchPaths(inherited, setting);
        if (value == inherited) continue;
        setVariable(entry.variable, value);
        sources()[entry.variable] = inherited.empty() ? "Settings" : "environment and Settings";
        USDCC_LOG_INFO("usdcc.usd", "{}={} (Settings > USD)", entry.variable, value);
        if (entry.key == std::string_view(kUsdPluginFoldersKey)) {
            const std::vector<std::string> had = split(inherited, ARCH_PATH_LIST_SEP[0]);
            for (std::string& folder : split(setting, ';')) {
                if (std::find(had.begin(), had.end(), folder) == had.end()) newPluginFolders.push_back(std::move(folder));
            }
        }
    }

    applyThreadLimit(settings);

    // The variable alone does nothing in this process: OpenUSD read
    // PXR_PLUGINPATH_NAME in a static constructor of its plug library, before
    // main(). So the setting's folders it didn't have then are registered. After
    // the thread limit, as the registry reads plugInfo.json files on TBB's threads.
    if (!newPluginFolders.empty()) {
        const PXR_NS::PlugPluginPtrVector registered = PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(newPluginFolders);
        USDCC_LOG_INFO("usdcc.usd", "OpenUSD plugins registered from Settings > USD's plugin folders: {} (from {} folders)",
                       registered.size(), newPluginFolders.size());
    }

    // After the thread limit, which wants nothing to have started threads yet.
    const ArnoldStatus arnold = enableArnold(settings.get<std::string>(kUsdArnoldFolderKey));
    if (arnold.enabled) {
        USDCC_LOG_INFO("usdcc.usd", "{}", arnold.message);
    } else if (!arnold.message.empty()) {
        USDCC_LOG_WARNING("usdcc.usd", "{}", arnold.message);
    }
}

void setUsdEnvironmentVariable(const std::string& variable, const std::string& value, std::string source) {
    setVariable(variable, value);
    sources()[variable] = std::move(source);
}

std::string usdEnvironmentSource(std::string_view variable) {
    const auto found = sources().find(variable);
    return found == sources().end() ? std::string() : found->second;
}

}  // namespace usdcc::usd
