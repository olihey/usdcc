// A render delegate's own settings (docs/VIEWPORT_DISPLAY.md §7): what Embree lists, the
// Settings > Renderers defaults, a viewport's overrides of them (kept when a default
// changes, followed otherwise), carried by a duplicate and a session, and the Renderer
// Settings dialog. The viewports are never shown, so there's no engine: what reaches
// one is checked in the app. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/settings.h"
#include "usdcc/tools/tool_set.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/hydra_viewport_window.h"
#include "usdcc/usd/renderer_settings.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/viewport_view_panel.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <QApplication>
#include <QDialog>
#include <QJsonObject>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>

#include <algorithm>
#include <memory>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

using usdcc::core::Settings;
using usdcc::core::SettingType;
using usdcc::usd::RendererSettingInfo;
using usdcc::usd::ViewportViewPanel;

namespace {
const TfToken kEmbree("HdEmbreeRendererPlugin");
const TfToken kStorm("HdStormRendererPlugin");
const TfToken kBounces("ty:maxBounces");
const std::string kBouncesKey = "Renderers/HdEmbreeRendererPlugin/ty:maxBounces";

const RendererSettingInfo* find(const std::vector<RendererSettingInfo>& settings, const TfToken& key) {
    const auto found = std::find_if(settings.begin(), settings.end(), [&key](const RendererSettingInfo& s) { return s.key == key; });
    return found == settings.end() ? nullptr : &*found;
}

bool overridden(const ViewportViewPanel& panel, const TfToken& pluginId, const TfToken& key) {
    const auto found = panel.rendererOverrides().find(pluginId);
    return found != panel.rendererOverrides().end() && found->second.contains(key);
}
}  // namespace

USDCC_TEST(renderer_settings_of_a_delegate) {
    const std::vector<RendererSettingInfo>& embree = usdcc::usd::rendererSettings(kEmbree);
    CHECK(!embree.empty(), "Embree lists settings");
    const RendererSettingInfo* bounces = find(embree, kBounces);
    CHECK(bounces && bounces->type == SettingType::Int && bounces->defaultValue.IsHolding<int>() && bounces->label == "Max Bounces",
          "its Max Bounces, an int with its label");
    const RendererSettingInfo* cubemap = find(usdcc::usd::rendererSettings(kStorm), TfToken("domeLightCubemapTargetMemory"));
    CHECK(cubemap && cubemap->type == SettingType::Int && cubemap->defaultValue.IsHolding<unsigned int>(),
          "Storm's unsigned cubemap memory, as an int");
    for (const TfToken& plugin : {kEmbree, kStorm}) {
        CHECK(!find(usdcc::usd::rendererSettings(plugin), HdRenderSettingsTokens->domeLightCameraVisibility),
              "%s: not the dome light's visibility, which Display sets", plugin.GetText());
    }
    CHECK(usdcc::usd::rendererSettings(TfToken("HdNoSuchRendererPlugin")).empty(), "nothing for an unknown plugin");

    // A delegate's value and a setting's, both ways.
    using usdcc::usd::toRendererValue;
    const auto is = [](const std::optional<VtValue>& value, const VtValue& expected) { return value && *value == expected; };
    CHECK(is(toRendererValue(3, VtValue(1.5f)), VtValue(3.0f)), "an int for a float");
    CHECK(is(toRendererValue(4.0, VtValue(1)), VtValue(4)), "a whole double for an int");
    CHECK(!toRendererValue(4.5, VtValue(1)), "not a fraction for an int");
    CHECK(!toRendererValue(-1, VtValue(1u)), "not a negative for an unsigned");
    CHECK(is(toRendererValue(std::string("a"), VtValue(TfToken("b"))), VtValue(TfToken("a"))), "a string for a token");
    CHECK(usdcc::usd::toSettingValue(VtValue(SdfAssetPath("x.exr"))) == usdcc::core::SettingValue(std::string("x.exr")),
          "an asset path as its path");
    CHECK(!usdcc::usd::toSettingValue(VtValue(GfVec3f(1.0f))), "not a vector");
}

USDCC_TEST(renderer_settings_defaults) {
    TestSettings testSettings;
    Settings& settings = Settings::instance();
    usdcc::usd::rendererSettingDefaults(kEmbree);  // what a viewport of it asks: defines them
    const usdcc::core::SettingDefinition* definition = settings.definition(kBouncesKey);
    CHECK(definition && definition->category == "Renderers" && !definition->group.empty() && definition->type == SettingType::Int,
          "Embree's Max Bounces on the Renderers page, in its renderer's group");
    usdcc::usd::defineRendererSettings();  // what the Settings dialog has done first
    CHECK(settings.contains("Renderers/HdStormRendererPlugin/maxLights"), "and every renderer's");
    CHECK(usdcc::usd::isRendererSettingKey(kBouncesKey) && !usdcc::usd::isRendererSettingKey("Viewport/showGrid"),
          "its key is a renderer's");
    const RendererSettingInfo* bounces = find(usdcc::usd::rendererSettings(kEmbree), kBounces);
    if (!bounces) return;  // said by renderer_settings_of_a_delegate
    const VtValue builtIn = bounces->defaultValue;
    CHECK(usdcc::usd::rendererSettingDefaults(kEmbree).at(kBounces) == builtIn, "the delegate's own default");
    CHECK(settings.setValue(kBouncesKey, 7), "a value");
    CHECK(usdcc::usd::rendererSettingDefaults(kEmbree).at(kBounces) == VtValue(7), "the default follows it");
    settings.reset(kBouncesKey);
}

USDCC_TEST(renderer_settings_overrides) {
    TestSettings testSettings;
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()), dir.filePath(QStringLiteral("ui.ini")));
    usdcc::usd::defineRendererSettings();
    Settings& settings = Settings::instance();
    settings.setValue(kBouncesKey, 9);

    usdcc::usd::StageManager stages;
    auto toolSet = std::make_shared<usdcc::tools::ToolSet>();
    auto panel = std::make_unique<ViewportViewPanel>(&stages, nullptr, toolSet, nullptr);
    panel->hydraWindow()->setRendererPlugin(kEmbree);  // what it starts with; no engine here
    CHECK(panel->rendererOverrides().empty(), "a new viewport overrides nothing");
    CHECK(panel->rendererSettingValues(kEmbree).at(kBounces) == VtValue(9), "and uses Settings > Renderers");

    int changes = 0;
    QObject::connect(panel.get(), &ViewportViewPanel::rendererSettingsChanged, [&changes]() { ++changes; });
    panel->setRendererOverride(kEmbree, kBounces, VtValue(32));
    CHECK(changes == 1, "rendererSettingsChanged() fired: %d", changes);
    settings.setValue(kBouncesKey, 4);
    CHECK(panel->rendererSettingValues(kEmbree).at(kBounces) == VtValue(32), "an override stays when the setting changes");

    // Duplicated, and through a session, with a renderer this installation hasn't.
    std::unique_ptr<usdcc::ui::ViewPanel> copy(panel->duplicate());
    CHECK(static_cast<ViewportViewPanel*>(copy.get())->rendererOverrides() == panel->rendererOverrides(), "a duplicate has them");
    QJsonObject state = panel->saveSessionState();
    const QJsonObject saved = state.value(QStringLiteral("rendererSettings")).toObject();
    CHECK(saved.value(QString::fromStdString(kEmbree.GetString())).toObject().value(QStringLiteral("ty:maxBounces")).toInt() == 32,
          "a session has the override");
    QJsonObject renderers = saved;
    renderers.insert(QStringLiteral("HdElsewhereRendererPlugin"), QJsonObject{{QStringLiteral("quality"), 3}});
    state.insert(QStringLiteral("rendererSettings"), renderers);
    auto restored = std::make_unique<ViewportViewPanel>(&stages, nullptr, toolSet, nullptr);
    restored->restoreSessionState(state);
    CHECK(restored->rendererOverrides().at(kEmbree).at(kBounces) == VtValue(32), "restored as the delegate's int");
    CHECK(restored->saveSessionState().value(QStringLiteral("rendererSettings")).toObject().contains(QStringLiteral("HdElsewhereRendererPlugin")),
          "an unknown renderer's are kept");

    // The dialog: the value shown, an edit overrides it, Default drops it.
    auto* dialog = panel->showRendererSettingsDialog();
    auto* spin = dialog->findChild<QSpinBox*>(QStringLiteral("rendererSetting_ty:maxBounces"));
    auto* reset = dialog->findChild<QToolButton*>(QStringLiteral("rendererSetting_ty:maxBounces_default"));
    CHECK(spin && reset, "a row for Max Bounces");
    if (!spin || !reset) return;
    CHECK(spin->value() == 32 && !reset->isHidden(), "shows the override, with its Default button");
    reset->click();
    CHECK(!overridden(*panel, kEmbree, kBounces) && spin->value() == 4 && reset->isHidden(),
          "Default drops it and shows the setting's 4 (%d)", spin->value());
    spin->setValue(16);
    CHECK(panel->rendererSettingValues(kEmbree).at(kBounces) == VtValue(16) && !reset->isHidden(), "an edit overrides it");
    CHECK(panel->showRendererSettingsDialog() == dialog, "one dialog at a time");
    dialog->close();
    settings.reset(kBouncesKey);
}
