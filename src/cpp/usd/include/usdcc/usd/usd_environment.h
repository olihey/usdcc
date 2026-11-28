#pragma once

// Settings > USD (docs/USD_RUNTIME.md §3): the few things OpenUSD only takes
// from environment variables, as preferences. Each applies from the next
// start, since OpenUSD reads them once: applyUsdEnvironmentSettings() sets
// them early in main(), before anything uses OpenUSD's plugin registry, asset
// resolver or shader registry. Qt-free.
//
// The environment the app was started with stays in control: a folder list
// adds the setting's folders after the variable's own, and the thread limit
// is ignored when PXR_WORK_THREAD_LIMIT is set.

#include "usdcc/core/settings.h"

#include <string>
#include <string_view>

namespace usdcc::usd {

inline constexpr const char* kUsdPluginFoldersKey = "USD/pluginFolders";        // PXR_PLUGINPATH_NAME
inline constexpr const char* kUsdAssetSearchPathsKey = "USD/assetSearchPaths";  // PXR_AR_DEFAULT_SEARCH_PATH
inline constexpr const char* kUsdMaterialXFoldersKey = "USD/materialXFolders";  // PXR_MTLX_PLUGIN_SEARCH_PATHS
inline constexpr const char* kUsdThreadLimitKey = "USD/threadLimit";            // WorkSetConcurrencyLimitArgument()
inline constexpr const char* kUsdArnoldFolderKey = "USD/arnoldFolder";          // enableArnold() (arnold_runtime.h)
// CYCLES_DEVICE, which the Cycles pack's render delegate reads when it's created:
// "CPU" (the default) or "GPU" (Metal). macOS only, the one platform whose pack has a GPU device
// (docs/USD_PLUGINS.md section 7 phase 5).
inline constexpr const char* kUsdCyclesDeviceKey = "USD/cyclesDevice";

void defineUsdEnvironmentSettings(core::Settings& settings = core::Settings::instance());

// Sets the variables and the thread limit from the settings, logging what it
// changes. Registers the plugin folders' plugins too, as OpenUSD read
// PXR_PLUGINPATH_NAME before main(), and turns Arnold on (arnold_runtime.h).
// Once, at start-up, after defineUsdEnvironmentSettings().
void applyUsdEnvironmentSettings(const core::Settings& settings = core::Settings::instance());

// Arnold from the pack usdcc downloaded (docs/USD_PLUGINS.md §10), whose folder is
// `packFolder`: used when Settings > Renderers names an Arnold folder and
// applyUsdEnvironmentSettings() found no Arnold plugins beside OpenUSD's own. Called
// once at start-up after it, when the downloaded packs are made loadable and before
// anything makes a stage; with "" where there is no such pack, as it is also what
// says, then, that the Arnold folder has no plugins to go with. True when Arnold is on
// from the pack.
bool enableArnoldPack(const std::string& packFolder);

// A path list variable's value with the setting's folders (separated by ';')
// after the environment's own (separated by the platform's list separator),
// leaving out folders already there. Empty when both are.
std::string mergeSearchPaths(std::string_view environment, std::string_view setting);

// Sets `variable` for this process and the processes it starts (the C runtime's
// copy of the environment too), and records `source` for usdEnvironmentSource().
// For the variables other features set from their settings, such as OCIO
// (color_management.h).
void setUsdEnvironmentVariable(const std::string& variable, const std::string& value, std::string source);

// CYCLES_DEVICE's value for the Cycles device setting's: "METAL" for "GPU", ""
// (Cycles' own default, the CPU) for anything else.
std::string cyclesDeviceVariable(std::string_view setting);

// Where applyUsdEnvironmentSettings() got `variable`'s value from: "Settings",
// "environment and Settings", or "" when it didn't set it. For the thread
// limit, ask for PXR_WORK_THREAD_LIMIT.
std::string usdEnvironmentSource(std::string_view variable);

}  // namespace usdcc::usd
