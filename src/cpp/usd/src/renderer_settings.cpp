#include "usdcc/usd/renderer_settings.h"

#include "usdcc/core/log.h"

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
constexpr auto kCategory = "Renderers";
constexpr auto kKeyPrefix = "Renderers/";

std::optional<core::SettingType> settingType(const VtValue& value) {
    if (value.IsHolding<bool>()) return core::SettingType::Bool;
    if (value.IsHolding<int>() || value.IsHolding<unsigned int>()) return core::SettingType::Int;
    if (value.IsHolding<float>() || value.IsHolding<double>()) return core::SettingType::Float;
    if (value.IsHolding<std::string>() || value.IsHolding<TfToken>() || value.IsHolding<SdfAssetPath>()) {
        return core::SettingType::String;
    }
    return std::nullopt;
}

std::vector<RendererSettingInfo> readSettings(const TfToken& pluginId) {
    std::vector<RendererSettingInfo> settings;
    // From the plugin itself, as renderDelegateShading() does: the registry's own
    // CreateRenderDelegate() first asks whether the plugin can render here, which takes
    // a GPU, and a list doesn't.
    HdRendererPluginHandle plugin = HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(pluginId);
    if (!plugin) return settings;
    const auto started = std::chrono::steady_clock::now();
    HdRenderDelegate* delegate = plugin->CreateRenderDelegate();
    if (!delegate) return settings;
    for (const HdRenderSettingDescriptor& descriptor : delegate->GetRenderSettingDescriptors()) {
        if (isAppOwnedRendererSetting(descriptor.key)) continue;
        const std::optional<core::SettingType> type = settingType(descriptor.defaultValue);
        if (!type) {
            USDCC_LOG_DEBUG("usdcc.viewport", "Renderer setting {} of {} is a {}: not shown", descriptor.key.GetString(),
                            pluginId.GetString(), descriptor.defaultValue.GetTypeName());
            continue;
        }
        settings.push_back({descriptor.key, descriptor.name.empty() ? descriptor.key.GetString() : descriptor.name, *type,
                            descriptor.defaultValue});
    }
    plugin->DeleteRenderDelegate(delegate);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    USDCC_LOG_DEBUG("usdcc.viewport", "Read {} settings of {} in {} ms", settings.size(), pluginId.GetString(), took.count());
    return settings;
}
}  // namespace

bool isAppOwnedRendererSetting(const TfToken& key) { return key == HdRenderSettingsTokens->domeLightCameraVisibility; }

const std::vector<RendererSettingInfo>& rendererSettings(const TfToken& pluginId) {
    static std::map<TfToken, std::vector<RendererSettingInfo>> cache;
    auto found = cache.find(pluginId);
    if (found == cache.end()) found = cache.emplace(pluginId, readSettings(pluginId)).first;
    return found->second;
}

std::string rendererSettingKey(const TfToken& pluginId, const TfToken& key) {
    return kKeyPrefix + pluginId.GetString() + "/" + key.GetString();
}

bool isRendererSettingKey(const std::string& settingsKey) { return settingsKey.starts_with(kKeyPrefix); }

void defineRendererSettings(const TfToken& pluginId) {
    core::Settings& registry = core::Settings::instance();
    // Settings::instance() lasts as long as the process, and so do its definitions.
    static std::set<TfToken> defined;
    if (pluginId.IsEmpty() || !defined.insert(pluginId).second) return;
    HfPluginDesc plugin;
    if (!HdRendererPluginRegistry::GetInstance().GetPluginDesc(pluginId, &plugin)) return;
    for (const RendererSettingInfo& setting : rendererSettings(pluginId)) {
        const std::optional<core::SettingValue> defaultValue = toSettingValue(setting.defaultValue);
        if (!defaultValue) continue;
        registry.define({
            .key = rendererSettingKey(pluginId, setting.key),
            .category = kCategory,
            .group = plugin.displayName,
            .label = setting.label,
            .description = "The " + plugin.displayName + " render delegate's \"" + setting.key.GetString() +
                           "\". A viewport can override it (Renderer > Renderer Settings...).",
            .type = setting.type,
            .defaultValue = *defaultValue,
        });
    }
}

void defineRendererSettings() {
    HfPluginDescVector plugins;
    HdRendererPluginRegistry::GetInstance().GetPluginDescs(&plugins);
    for (const HfPluginDesc& plugin : plugins) defineRendererSettings(TfToken(plugin.id));
}

RendererSettingValues rendererSettingDefaults(const TfToken& pluginId) {
    defineRendererSettings(pluginId);
    const core::Settings& registry = core::Settings::instance();
    RendererSettingValues values;
    for (const RendererSettingInfo& setting : rendererSettings(pluginId)) {
        const std::string key = rendererSettingKey(pluginId, setting.key);
        std::optional<VtValue> value;
        if (registry.contains(key)) value = toRendererValue(registry.value(key), setting.defaultValue);
        values[setting.key] = value ? *value : setting.defaultValue;
    }
    return values;
}

std::optional<core::SettingValue> toSettingValue(const VtValue& value) {
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>();
    if (value.IsHolding<int>()) return value.UncheckedGet<int>();
    if (value.IsHolding<unsigned int>()) {
        const unsigned int number = value.UncheckedGet<unsigned int>();
        if (number > static_cast<unsigned int>(std::numeric_limits<int>::max())) return std::nullopt;
        return static_cast<int>(number);
    }
    if (value.IsHolding<float>()) return static_cast<double>(value.UncheckedGet<float>());
    if (value.IsHolding<double>()) return value.UncheckedGet<double>();
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    if (value.IsHolding<SdfAssetPath>()) return value.UncheckedGet<SdfAssetPath>().GetAssetPath();
    return std::nullopt;
}

std::optional<VtValue> toRendererValue(const core::SettingValue& value, const VtValue& like) {
    // A whole number, from an int or a double without a fraction.
    const auto whole = [&value]() -> std::optional<long long> {
        if (const int* number = std::get_if<int>(&value)) return *number;
        if (const double* number = std::get_if<double>(&value); number && std::isfinite(*number) && std::trunc(*number) == *number) {
            return static_cast<long long>(*number);
        }
        return std::nullopt;
    };
    const auto real = [&value]() -> std::optional<double> {
        if (const double* number = std::get_if<double>(&value)) return *number;
        if (const int* number = std::get_if<int>(&value)) return *number;
        return std::nullopt;
    };
    const std::string* text = std::get_if<std::string>(&value);
    if (like.IsHolding<bool>()) {
        if (const bool* on = std::get_if<bool>(&value)) return VtValue(*on);
        return std::nullopt;
    }
    if (like.IsHolding<int>()) {
        const std::optional<long long> number = whole();
        if (!number || *number < std::numeric_limits<int>::min() || *number > std::numeric_limits<int>::max()) return std::nullopt;
        return VtValue(static_cast<int>(*number));
    }
    if (like.IsHolding<unsigned int>()) {
        const std::optional<long long> number = whole();
        if (!number || *number < 0 || *number > std::numeric_limits<unsigned int>::max()) return std::nullopt;
        return VtValue(static_cast<unsigned int>(*number));
    }
    if (like.IsHolding<float>()) {
        if (const std::optional<double> number = real()) return VtValue(static_cast<float>(*number));
        return std::nullopt;
    }
    if (like.IsHolding<double>()) {
        if (const std::optional<double> number = real()) return VtValue(*number);
        return std::nullopt;
    }
    if (!text) return std::nullopt;
    if (like.IsHolding<std::string>()) return VtValue(*text);
    if (like.IsHolding<TfToken>()) return VtValue(TfToken(*text));
    if (like.IsHolding<SdfAssetPath>()) return VtValue(SdfAssetPath(*text));
    return std::nullopt;
}

}  // namespace usdcc::usd
