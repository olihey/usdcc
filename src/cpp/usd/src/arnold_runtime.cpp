#include "usdcc/usd/arnold_runtime.h"

#include <pxr/base/arch/defines.h>
#include <pxr/base/arch/env.h>
#include <pxr/base/arch/symbols.h>
#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(ARCH_OS_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace usdcc::usd {

namespace {

std::vector<int> versionParts(std::string_view text) {
    std::vector<int> parts;
    int value = 0;
    bool digits = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '.' && digits) {
            parts.push_back(value);
            value = 0;
            digits = false;
        } else {
            break;
        }
    }
    if (digits) parts.push_back(value);
    return parts;
}

// The "version" the pack's usd-plugins/arnold.json gives (tools/build_usd_plugin.py).
std::string packVersion(const std::filesystem::path& manifest) {
    std::ifstream stream(manifest);
    if (!stream) return {};
    std::stringstream text;
    text << stream.rdbuf();
    const PXR_NS::JsValue json = PXR_NS::JsParseString(text.str());
    if (!json.IsObject()) return {};
    const PXR_NS::JsObject& object = json.GetJsObject();
    const auto found = object.find("version");
    return found != object.end() && found->second.IsString() ? found->second.GetString() : std::string();
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace

bool arnoldVersionFits(std::string_view runtime, std::string_view builtWith) {
    const std::vector<int> have = versionParts(runtime);
    const std::vector<int> need = versionParts(builtWith);
    if (have.size() < 2 || need.size() < 2) return false;
    if (have[0] != need[0] || have[1] != need[1]) return false;
    return have >= need;  // element by element, a shorter one first where they agree
}

std::string usdRootFolder() {
    std::string library;
    // Any OpenUSD function does: their libraries are all in one folder. Only the
    // path: on Windows, asking for more looks for debug symbols, and fails without.
    void* address = reinterpret_cast<void*>(static_cast<std::string (*)(const std::string&)>(&PXR_NS::ArchGetEnv));
    if (!PXR_NS::ArchGetAddressInfo(address, &library, nullptr, nullptr, nullptr) || library.empty()) return {};
    return utf8(std::filesystem::path(std::filesystem::u8path(library)).parent_path().parent_path());
}

ArnoldStatus enableArnold(const std::string& arnoldFolder, const std::string& usdRoot) {
    ArnoldStatus status;
    if (arnoldFolder.empty()) return status;
#if !defined(ARCH_OS_WINDOWS)
    status.message = "Settings > Renderers names an Arnold folder, but Arnold in usdcc is Windows-only for now";
    return status;
#else
    const std::filesystem::path root = std::filesystem::u8path(usdRoot.empty() ? usdRootFolder() : usdRoot);
    const std::filesystem::path plugins = root / "plugin" / "arnold";
    if (!std::filesystem::is_regular_file(plugins / "plugInfo.json")) {
        status.message = "Settings > Renderers names an Arnold folder, but this usdcc has no Arnold plugins (no " +
                         utf8(plugins / "plugInfo.json") +
                         "). Build them with tools/build_usd_plugin.py arnold (docs/USD_PLUGINS.md)";
        return status;
    }
    status.pluginFolder = utf8(plugins);
    status.packVersion = packVersion(root / "usd-plugins" / "arnold.json");
    if (status.packVersion.empty()) {
        status.message = "The Arnold plugins in " + status.pluginFolder + " don't say which Arnold they were built for (no " +
                         utf8(root / "usd-plugins" / "arnold.json") + "); Arnold stays off";
        return status;
    }

    const std::filesystem::path core = std::filesystem::u8path(arnoldFolder) / "bin" / "ai.dll";
    if (!std::filesystem::is_regular_file(core)) {
        status.message = "The Arnold folder in Settings > Renderers, " + arnoldFolder +
                         ", has no bin/ai.dll: name an Arnold SDK or install. Arnold stays off";
        return status;
    }
    // Its own DLLs (licensing, denoisers) from its own folder, and no message box
    // if one is missing: a failure is logged instead.
    DWORD previousMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previousMode);
    HMODULE module = LoadLibraryExW(core.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD error = module ? 0 : GetLastError();
    SetThreadErrorMode(previousMode, nullptr);
    if (!module) {
        status.message = "Arnold's core, " + utf8(core) + ", didn't load (Windows error " + std::to_string(error) +
                         "). Arnold stays off";
        return status;
    }
    using GetVersion = const char* (*)(char*, char*, char*, char*);
    const auto getVersion = reinterpret_cast<GetVersion>(GetProcAddress(module, "AiGetVersion"));
    const char* version = getVersion ? getVersion(nullptr, nullptr, nullptr, nullptr) : nullptr;
    status.arnoldVersion = version ? version : "";
    if (!arnoldVersionFits(status.arnoldVersion, status.packVersion)) {
        FreeLibrary(module);
        const std::vector<int> built = versionParts(status.packVersion);
        status.message = "The Arnold in " + arnoldFolder + " is " +
                         (status.arnoldVersion.empty() ? std::string("of an unknown version") : status.arnoldVersion) +
                         ", but usdcc's Arnold plugins were built for " + status.packVersion +
                         ": they need that Arnold or a later " + std::to_string(built[0]) + "." +
                         std::to_string(built.size() > 1 ? built[1] : 0) + ". Arnold stays off";
        status.arnoldVersion.clear();
        return status;
    }

    // Registered, not put on PXR_PLUGINPATH_NAME: OpenUSD reads that when its
    // libraries load, before main(), and a process usdcc starts would get the
    // plugins without the core.
    const PXR_NS::PlugPluginPtrVector registered = PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(status.pluginFolder);
    if (registered.empty()) {
        FreeLibrary(module);
        status.message = "OpenUSD registered no plugins from " + status.pluginFolder + ". Arnold stays off";
        status.arnoldVersion.clear();
        return status;
    }
    status.enabled = true;
    status.message = "Arnold " + status.arnoldVersion + " from " + utf8(core) + ", with " +
                     std::to_string(registered.size()) + " plugins from " + status.pluginFolder + " (Settings > Renderers)";
    return status;
#endif
}

}  // namespace usdcc::usd
