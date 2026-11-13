#pragma once

// A Hydra render delegate's own settings (docs/VIEWPORT_DISPLAY.md §7): what it lists
// (HdRenderDelegate::GetRenderSettingDescriptors()), their app-wide defaults in
// Settings > Renderers, and the values a viewport hands its engine.

#include "usdcc/core/settings.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// One setting of a render delegate.
struct RendererSettingInfo {
    PXR_NS::TfToken key;           // what the delegate takes it by: "ty:maxBounces"
    std::string label;             // what it calls it: "Max Bounces"
    core::SettingType type;        // Bool, Int, Float or String
    PXR_NS::VtValue defaultValue;  // in the delegate's own type (int, float, TfToken, ...)
};

// What the values a viewport hands its engine are: by setting key.
using RendererSettingValues = std::map<PXR_NS::TfToken, PXR_NS::VtValue>;

// The settings `pluginId` ("HdEmbreeRendererPlugin") lists, in its order. Read from a
// delegate made for the purpose (no GPU or GL context needed) and kept for later calls.
// Leaves out the settings of another type than a bool, a number or a string, and those
// the app sets itself (isAppOwnedRendererSetting()). Empty for an unknown plugin or one
// whose delegate can't be made.
const std::vector<RendererSettingInfo>& rendererSettings(const PXR_NS::TfToken& pluginId);

// Settings the viewport sets from its own options, never shown as the delegate's:
// domeLightCameraVisibility (the Display options' dome light textures).
bool isAppOwnedRendererSetting(const PXR_NS::TfToken& key);

// The Settings key of a delegate's setting: "Renderers/<pluginId>/<key>".
std::string rendererSettingKey(const PXR_NS::TfToken& pluginId, const PXR_NS::TfToken& key);
// Whether `settingsKey` is one of those (Settings::changed's key).
bool isRendererSettingKey(const std::string& settingsKey);

// Settings > Renderers: a group per render delegate, a setting each of its settings,
// defaulting to the delegate's own default. Defined when needed, not at start: listing a
// delegate's settings makes one, which for some (Gatling, loading its MDL runtime) takes
// most of a second. One delegate's: by rendererSettingDefaults(), so a viewport using it
// starts from the saved values. All of them: before the Settings dialog opens (main.cpp).
// Each is defined once; calling again does nothing.
void defineRendererSettings(const PXR_NS::TfToken& pluginId);
void defineRendererSettings();

// What a viewport of `pluginId` uses without overrides: each setting's Settings value,
// as the delegate's type. Defines the delegate's settings first.
RendererSettingValues rendererSettingDefaults(const PXR_NS::TfToken& pluginId);

// Between a delegate's value and a Settings value. toSettingValue() is nothing for
// a value of another type; toRendererValue() makes `value` the type of `like` (the
// delegate's default), or nothing if it can't be.
std::optional<core::SettingValue> toSettingValue(const PXR_NS::VtValue& value);
std::optional<PXR_NS::VtValue> toRendererValue(const core::SettingValue& value, const PXR_NS::VtValue& like);

}  // namespace usdcc::usd
