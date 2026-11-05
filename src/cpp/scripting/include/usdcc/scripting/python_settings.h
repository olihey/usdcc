#pragma once

#include <filesystem>
#include <vector>

namespace usdcc::scripting {

// The Python page of Edit > Settings (docs/PYTHON_PACKAGES.md §3a). The keys.
// kPythonUserSiteKey, in python_runtime.h, is one of them too.
inline constexpr auto kPackagesFolderKey = "Python/packagesFolder";
inline constexpr auto kExtraPackageFoldersKey = "Python/extraPackageFolders";
inline constexpr auto kUserPackagesKey = "Python/userPackages";
inline constexpr auto kPackageIndexKey = "Python/packageIndex";
inline constexpr auto kWheelsFolderKey = "Python/wheelsFolder";
inline constexpr auto kPackagesOfflineKey = "Python/offline";

// Defines them. Before PluginManager::defaultOptions() and PythonRuntime, which
// read them, and after usdcc::ui::installAppSettings(), so they load saved values.
// Needs the Qt application object, for the default folder.
void definePythonSettings();

// The Shared packages folder, or its default if it has been emptied.
std::filesystem::path packagesFolderSetting();
// The Additional package folders, split.
std::vector<std::filesystem::path> extraPackageFoldersSetting();

}  // namespace usdcc::scripting
