#include "usdcc/usd/usd_runtime_info.h"

#include "usdcc/usd/usd_environment.h"

#include <pxr/base/arch/defines.h>
#include <pxr/base/arch/demangle.h>
#include <pxr/base/arch/env.h>
#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/arch/systemInfo.h>
#include <pxr/base/js/value.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/debug.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/pxr.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/usdMtlx/utils.h>

#if defined(ARCH_OS_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <set>
#include <typeinfo>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {

std::string join(const std::vector<std::string>& items, const char* separator = ", ") {
    return items.empty() ? std::string("none") : TfStringJoin(items, separator);
}

std::string pluginName(const TfType& type) {
    const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginForType(type);
    return plugin ? plugin->GetName() : std::string();
}

std::string metadataString(const TfType& type, const char* key) {
    const JsValue value = PlugRegistry::GetInstance().GetDataFromPluginMetaData(type, key);
    return value.IsString() ? value.GetString() : std::string();
}

bool metadataBool(const TfType& type, const char* key, bool fallback) {
    const JsValue value = PlugRegistry::GetInstance().GetDataFromPluginMetaData(type, key);
    return value.IsBool() ? value.GetBool() : fallback;
}

// The resolver's registered type name, which plugInfo.json and
// ArSetPreferredResolver use.
std::string typeName(const ArResolver& resolver) {
    const TfType type = TfType::Find(resolver);
    return type.IsUnknown() ? ArchGetDemangled(typeid(resolver)) : type.GetTypeName();
}

// "NAME=value" for the whole process environment, as UTF-8. On Windows from
// Windows' own copy, which has what ArchSetEnv() sets as well.
std::vector<std::string> processEnvironment() {
    std::vector<std::string> entries;
#if defined(ARCH_OS_WINDOWS)
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return entries;
    for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
        const int length = static_cast<int>(wcslen(entry));
        const int size = WideCharToMultiByte(CP_UTF8, 0, entry, length, nullptr, 0, nullptr, nullptr);
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, entry, length, utf8.data(), size, nullptr, nullptr);
        entries.push_back(std::move(utf8));
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** entry = ArchEnviron(); entry && *entry; ++entry) entries.emplace_back(*entry);
#endif
    return entries;
}

UsdRuntimeInfo::Section openUsdSection(const std::vector<UsdRuntimeInfo::Plugin>& plugins) {
    UsdRuntimeInfo::Section section{"OpenUSD", {}};
    section.rows.push_back({"Version", TfStringPrintf("%d.%d.%d", PXR_MAJOR_VERSION, PXR_MINOR_VERSION, PXR_PATCH_VERSION)});
#if defined(PXR_PYTHON_SUPPORT_ENABLED)
    section.rows.push_back({"Python support", "yes"});
#else
    section.rows.push_back({"Python support", "no"});
#endif
    if (const PlugPluginPtr usd = PlugRegistry::GetInstance().GetPluginWithName("usd")) {
        const std::string library = usd->GetPath();
        section.rows.push_back({"Libraries", library.substr(0, library.find_last_of("/\\"))});
    }
    section.rows.push_back({"Executable", ArchGetExecutablePath()});
    const auto loaded = std::count_if(plugins.begin(), plugins.end(), [](const auto& plugin) { return plugin.loaded; });
    section.rows.push_back({"Plugins", TfStringPrintf("%zu, %td loaded", plugins.size(), loaded)});
    return section;
}

UsdRuntimeInfo::Section threadsSection() {
    UsdRuntimeInfo::Section section{"Threads", {}};
    std::string threads = TfStringPrintf("%u of %u cores", WorkGetConcurrencyLimit(), WorkGetPhysicalConcurrencyLimit());
    if (const std::string source = usdEnvironmentSource("PXR_WORK_THREAD_LIMIT"); !source.empty()) {
        threads += " (" + source + ")";
    } else if (const std::string variable = ArchGetEnv("PXR_WORK_THREAD_LIMIT"); !variable.empty()) {
        threads += " (PXR_WORK_THREAD_LIMIT=" + variable + ")";
    }
    section.rows.push_back({"Worker threads", threads});
    return section;
}

UsdRuntimeInfo::Section assetSection() {
    UsdRuntimeInfo::Section section{"Asset resolution", {}};
    section.rows.push_back({"Primary resolver", typeName(ArGetUnderlyingResolver())});
    std::vector<std::string> resolvers;
    for (const TfType& type : ArGetAvailableResolvers()) resolvers.push_back(type.GetTypeName());
    section.rows.push_back({"Available resolvers", join(resolvers)});
    section.rows.push_back({"URI schemes", join(ArGetRegisteredURISchemes())});
    const std::string searchPath = ArchGetEnv("PXR_AR_DEFAULT_SEARCH_PATH");
    section.rows.push_back(
        {"Search paths", join(TfStringSplit(searchPath, ARCH_PATH_LIST_SEP), "; ")});
    section.rows.push_back({"Layers in memory", std::to_string(SdfLayer::GetLoadedLayers().size())});
    return section;
}

UsdRuntimeInfo::Section shadingSection() {
    UsdRuntimeInfo::Section section{"Shading", {}};
    SdrRegistry& registry = SdrRegistry::GetInstance();
    std::vector<std::string> systems;
    for (const TfToken& system : registry.GetAllShaderNodeShadingSystems()) systems.push_back(system.GetString());
    section.rows.push_back({"Shading systems", join(systems)});
    std::vector<std::string> sourceTypes;
    for (const TfToken& type : registry.GetAllShaderNodeSourceTypes()) sourceTypes.push_back(type.GetString());
    section.rows.push_back({"Source types", join(sourceTypes)});
    section.rows.push_back({"Shader nodes", std::to_string(registry.GetShaderNodeIdentifiers().size())});
    section.rows.push_back({"Shader search paths", join(registry.GetSearchURIs(), "; ")});
    section.rows.push_back({"MaterialX search paths", join(UsdMtlxSearchPaths(), "; ")});
    return section;
}

UsdRuntimeInfo::Section debugSection() {
    UsdRuntimeInfo::Section section{"Debugging", {}};
    std::vector<std::string> enabled;
    const std::vector<std::string> names = TfDebug::GetDebugSymbolNames();
    for (const std::string& name : names) {
        if (TfDebug::IsDebugSymbolNameEnabled(name)) enabled.push_back(name);
    }
    section.rows.push_back({"TF_DEBUG flags on", TfStringPrintf("%zu of %zu", enabled.size(), names.size())});
    if (!enabled.empty()) section.rows.push_back({"On", join(enabled)});
    return section;
}

}  // namespace

bool isUsdEnvironmentVariable(std::string_view name) {
    static constexpr std::string_view kPrefixes[] = {
        "PXR_",  "PIXAR_", "TF_",  "ARCH_", "WORK_", "TRACE_", "PLUG_", "AR_",  "SDF_",      "SDR_",   "NDR_",
        "PCP_",  "USD",    "HD",   "HGI",   "HIO_",  "GLF_",   "GARCH_", "VDF_", "MATERIALX", "MTLX_", "OCIO",
    };
    // Windows' variables keep their case; OpenUSD's are upper case.
    return std::any_of(std::begin(kPrefixes), std::end(kPrefixes),
                       [name](std::string_view prefix) { return name.substr(0, prefix.size()) == prefix; });
}

UsdRuntimeInfo collectUsdRuntimeInfo() {
    UsdRuntimeInfo info;
    PlugRegistry& registry = PlugRegistry::GetInstance();

    for (const PlugPluginPtr& plugin : registry.GetAllPlugins()) {
        if (!plugin) continue;
        info.plugins.push_back({plugin->GetName(),
                                plugin->IsPythonModule() ? "python" : plugin->IsResource() ? "resource" : "library",
                                plugin->IsLoaded(), plugin->GetPath()});
    }
    std::sort(info.plugins.begin(), info.plugins.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

    // From the plugins' metadata, as SdfFileFormat's registry does, so no
    // format's library is loaded. Abstract bases (SdfTextFileFormat) have none.
    std::set<TfType> formatTypes;
    TfType::Find<SdfFileFormat>().GetAllDerivedTypes(&formatTypes);
    for (const TfType& type : formatTypes) {
        const std::string formatId = metadataString(type, "formatId");
        if (formatId.empty()) continue;
        UsdRuntimeInfo::FileFormat format;
        format.formatId = formatId;
        const JsValue extensions = registry.GetDataFromPluginMetaData(type, "extensions");
        if (extensions.IsArrayOf<std::string>()) format.extensions = extensions.GetArrayOf<std::string>();
        format.target = metadataString(type, "target");
        format.reading = metadataBool(type, "supportsReading", true);
        format.writing = metadataBool(type, "supportsWriting", true);
        format.plugin = pluginName(type);
        info.fileFormats.push_back(std::move(format));
    }
    std::sort(info.fileFormats.begin(), info.fileFormats.end(),
              [](const auto& a, const auto& b) { return a.formatId < b.formatId; });

    HfPluginDescVector renderers;
    HdRendererPluginRegistry::GetInstance().GetPluginDescs(&renderers);
    for (const HfPluginDesc& renderer : renderers) {
        info.renderers.push_back({renderer.id.GetString(), renderer.displayName, renderer.priority,
                                  pluginName(TfType::FindByName(renderer.id.GetString()))});
    }

    info.overview = {openUsdSection(info.plugins), threadsSection(), assetSection(), shadingSection(), debugSection()};

    for (const std::string& entry : processEnvironment()) {
        // Windows lists its per-drive folders as "=C:=C:\...".
        const size_t equals = entry.find('=', 1);
        if (equals == std::string::npos) continue;
        std::string name = entry.substr(0, equals);
        if (!isUsdEnvironmentVariable(name)) continue;
        const std::string source = usdEnvironmentSource(name);
        info.environment.push_back({std::move(name), entry.substr(equals + 1), source});
    }
    std::sort(info.environment.begin(), info.environment.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return info;
}

std::string usdRuntimeInfoText(const UsdRuntimeInfo& info) {
    std::string text;
    const auto line = [&text](const std::string& content) { text += content + "\n"; };
    for (const UsdRuntimeInfo::Section& section : info.overview) {
        line(section.title);
        for (const UsdRuntimeInfo::Row& row : section.rows) line("  " + row.name + ": " + row.value);
        line("");
    }
    line(TfStringPrintf("Plugins (%zu)", info.plugins.size()));
    for (const auto& plugin : info.plugins) {
        line("  " + plugin.name + " (" + plugin.type + (plugin.loaded ? ", loaded" : "") + "): " + plugin.path);
    }
    line("");
    line(TfStringPrintf("File formats (%zu)", info.fileFormats.size()));
    for (const auto& format : info.fileFormats) {
        std::string modes = format.reading && format.writing ? "read/write" : format.reading ? "read" : "write";
        line("  " + format.formatId + " [" + join(format.extensions) + "] target " + format.target + ", " + modes +
             ", from " + format.plugin);
    }
    line("");
    line(TfStringPrintf("Render delegates (%zu)", info.renderers.size()));
    for (const auto& renderer : info.renderers) {
        line("  " + renderer.displayName + " (" + renderer.id + ", priority " + std::to_string(renderer.priority) +
             ", from " + renderer.plugin + ")");
    }
    line("");
    line(TfStringPrintf("Environment (%zu)", info.environment.size()));
    for (const auto& variable : info.environment) {
        line("  " + variable.name + "=" + variable.value + (variable.source.empty() ? "" : " (" + variable.source + ")"));
    }
    return text;
}

std::vector<UsdDebugFlag> usdDebugFlags() {
    std::vector<UsdDebugFlag> flags;
    for (const std::string& name : TfDebug::GetDebugSymbolNames()) {
        flags.push_back({name, TfDebug::GetDebugSymbolDescription(name), TfDebug::IsDebugSymbolNameEnabled(name)});
    }
    std::sort(flags.begin(), flags.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return flags;
}

bool setUsdDebugFlag(const std::string& name, bool enabled) {
    // A name with a '*' would set every flag it matches.
    if (name.empty() || name.find('*') != std::string::npos) return false;
    return !TfDebug::SetDebugSymbolsByName(name, enabled).empty();
}

}  // namespace usdcc::usd
