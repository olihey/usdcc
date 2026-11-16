// Color management (docs/COLOR_MANAGEMENT.md): OCIO's built-in config, the working
// space, the display and view settings, and what the viewport hands Hydra.
#include "test_harness.h"

#include "usdcc/core/settings.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/viewport_display.h"
#include "usdcc/usd/viewport_render_params.h"

#include <pxr/base/arch/env.h>
#include <pxr/imaging/hdx/tokens.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace usdcc::usd;

namespace {

bool has(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool closeTo(float a, float b) { return std::abs(a - b) < 0.002f; }

// The OCIO variable as it was, and nothing loaded: applyColorManagementSettings() sets both.
struct RestoreColor {
    bool had = PXR_NS::ArchHasEnv("OCIO");
    std::string before = PXR_NS::ArchGetEnv("OCIO");
    ~RestoreColor() {
        ColorManagement::instance().unload();
        if (had) PXR_NS::ArchSetEnv("OCIO", before, true);
        else PXR_NS::ArchRemoveEnv("OCIO");
    }
};

}  // namespace

USDCC_TEST(color_management_builtin_config) {
    ColorManagement color;
    if (!color.load(kDefaultColorConfig)) {
        CHECK(false, "the built-in config loads: %s", color.error().c_str());
        return;
    }
    CHECK(color.available() && color.source() == kDefaultColorConfig && color.error().empty(), "loaded");
    CHECK(color.configName().find("cg-config") == 0, "OCIO's CG config: %s", color.configName().c_str());
    CHECK(has(color.displays(), "sRGB - Display") && color.defaultDisplay() == "sRGB - Display", "the sRGB display");
    const auto views = color.views("sRGB - Display");
    CHECK(has(views, "Un-tone-mapped") && has(views, "Raw") && has(views, "ACES 1.0 - SDR Video"), "its views");
    CHECK(color.views("No Such Display").empty(), "no views for a display that isn't there");
    // The config's own default view is Raw, its first: usdcc's is Un-tone-mapped.
    CHECK(color.defaultView("sRGB - Display") == kPreferredView, "Un-tone-mapped first: %s",
          color.defaultView("sRGB - Display").c_str());
    // Not the config's scene_linear role, which is ACEScg.
    CHECK(color.workingSpace() == "Linear Rec.709 (sRGB)", "linear Rec.709: %s", color.workingSpace().c_str());

    // A 0.2 grey card: 124/255 through the sRGB encode, as it is through Raw, and
    // rolled off by ACES.
    const std::array<float, 3> grey{0.2f, 0.2f, 0.2f};
    const auto encoded = color.toDisplay(grey, {"sRGB - Display", "Un-tone-mapped"});
    CHECK(closeTo(encoded[0], 0.4845f) && closeTo(encoded[1], 0.4845f) && closeTo(encoded[2], 0.4845f),
          "Un-tone-mapped is the sRGB encode: %f", encoded[0]);
    const auto raw = color.toDisplay(grey, {"sRGB - Display", "Raw"});
    CHECK(closeTo(raw[0], 0.2f), "Raw leaves it: %f", raw[0]);
    const auto aces = color.toDisplay({1.0f, 1.0f, 1.0f}, {"sRGB - Display", "ACES 1.0 - SDR Video"});
    CHECK(aces[0] > 0.6f && aces[0] < 0.95f, "ACES rolls 1.0 off below white: %f", aces[0]);
}

USDCC_TEST(color_management_missing_config) {
    ColorManagement color;
    CHECK(!color.load("C:/no/such/config.ocio"), "a missing file doesn't load");
    CHECK(!color.available() && !color.error().empty() && color.source().empty(), "why: %s", color.error().c_str());
    CHECK(color.displays().empty() && color.workingSpace().empty(), "nothing to list");
    CHECK(color.displayView() == DisplayView{}, "nothing to show with");
    const std::array<float, 3> grey{0.2f, 0.2f, 0.2f};
    CHECK(color.toDisplay(grey, {"sRGB - Display", "Un-tone-mapped"}) == grey, "colors stay as they are");
    CHECK(!color.load(""), "nor does nothing");
}

USDCC_TEST(color_management_settings) {
    const RestoreColor restore;
    PXR_NS::ArchRemoveEnv("OCIO");
    usdcc::core::Settings settings;
    defineColorManagementSettings(settings);
    CHECK(settings.definitions(kColorCategory).size() == 1, "the config first, on its own page");
    CHECK(settings.definition(kColorConfigKey)->appliesAtNextStart, "the config applies at the next start");
    applyColorManagementSettings(settings);
    ColorManagement& color = ColorManagement::instance();
    if (!color.available()) {
        CHECK(false, "the default config loads: %s", color.error().c_str());
        return;
    }
    CHECK(PXR_NS::ArchGetEnv("OCIO") == kDefaultColorConfig, "OCIO is set for Hydra: %s",
          PXR_NS::ArchGetEnv("OCIO").c_str());
    CHECK(settings.contains(kColorDisplayKey) && settings.contains(kColorViewKey), "display and view defined");
    CHECK(settings.get<std::string>(kColorViewKey) == kPreferredView, "Un-tone-mapped by default");
    CHECK(settings.definition(kColorConfigKey)->description.find("Linear Rec.709") != std::string::npos,
          "the config's row says what's in use");

    CHECK(color.displayView({}, settings) == (DisplayView{"sRGB - Display", "Un-tone-mapped"}), "the settings'");
    CHECK(color.displayView("ACES 1.0 - SDR Video", settings).view == "ACES 1.0 - SDR Video", "an override wins");
    CHECK(color.displayView("Nonsense", settings).view == kPreferredView, "an override the display lacks doesn't");

    // Each display has its own views: P3 has SDR Cinema, sRGB doesn't.
    CHECK(settings.setValue(kColorDisplayKey, std::string("P3-D65 - Display")), "P3");
    CHECK(settings.setValue(kColorViewKey, std::string("ACES 1.0 - SDR Cinema")), "a P3 view");
    CHECK(settings.setValue(kColorDisplayKey, std::string("sRGB - Display")), "back to sRGB");
    CHECK(color.displayView({}, settings).view == kPreferredView, "a view sRGB lacks gives way: %s",
          color.displayView({}, settings).view.c_str());
    CHECK(!settings.setValue(kColorViewKey, std::string("ACES 1.0 - SDR Cinema")), "and isn't a choice any more");
}

USDCC_TEST(color_management_render_params) {
    const RestoreColor restore;
    ColorManagement& color = ColorManagement::instance();
    color.unload();
    HydraViewportWindow::DisplayOptions display;
    auto params = viewportRenderParams(display, display.shading);
    CHECK(params.colorCorrectionMode == PXR_NS::HdxColorCorrectionTokens->sRGB, "no config: Hydra's sRGB encode");
    CHECK(params.clearColor[3] == 0.0f, "cleared to nothing, over the viewport's own background");
    display.colorView = kRawView;
    CHECK(viewportRenderParams(display, display.shading).colorCorrectionMode ==
              PXR_NS::HdxColorCorrectionTokens->disabled,
          "Raw: none");

    if (!color.load(kDefaultColorConfig)) {
        CHECK(false, "the built-in config loads: %s", color.error().c_str());
        return;
    }
    display.colorView.clear();
    params = viewportRenderParams(display, display.shading);
    CHECK(params.colorCorrectionMode == PXR_NS::HdxColorCorrectionTokens->openColorIO, "OCIO with a config");
    CHECK(params.ocioDisplay == PXR_NS::TfToken("sRGB - Display") && params.ocioView == PXR_NS::TfToken(kPreferredView),
          "the display and view: %s / %s", params.ocioDisplay.GetText(), params.ocioView.GetText());
    CHECK(params.ocioColorSpace == PXR_NS::TfToken("Linear Rec.709 (sRGB)"), "from the working space");
    CHECK(params.ocioLook.IsEmpty(), "no look");
    display.colorView = "ACES 1.0 - SDR Video";
    CHECK(viewportRenderParams(display, display.shading).ocioView == PXR_NS::TfToken("ACES 1.0 - SDR Video"),
          "the viewport's own view");
    display.colorView = kRawView;
    CHECK(viewportRenderParams(display, display.shading).colorCorrectionMode ==
              PXR_NS::HdxColorCorrectionTokens->disabled,
          "Raw: none, rather than an identity OCIO pass");
}

USDCC_TEST(color_management_view_override) {
    ViewportDisplayOverrides overrides;
    overrides.colorView = "ACES 1.0 - SDR Video";
    const QJsonObject json = overrides.toJson();
    CHECK(json.value("colorView").toString() == "ACES 1.0 - SDR Video", "saved by name");
    CHECK(ViewportDisplayOverrides::fromJson(json) == overrides, "round trip");
    CHECK(overrides.applyTo({}).colorView == "ACES 1.0 - SDR Video", "applied");
    CHECK(ViewportDisplayOverrides::fromJson(QJsonObject{{"colorView", ""}}).isEmpty() &&
              ViewportDisplayOverrides::fromJson(QJsonObject{{"colorView", 3}}).isEmpty(),
          "an empty or non-string view is ignored");
}

USDCC_TEST(color_management_opaque_clear) {
    const RestoreColor restore;
    ColorManagement& color = ColorManagement::instance();
    color.unload();
    const QColor background(0x1c, 0x1c, 0x1e);
    PXR_NS::UsdImagingGLRenderParams params;
    params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->disabled;
    CHECK(closeTo(usdcc::usd::opaqueClearColor(background, params)[0], 28.0f / 255.0f), "Raw: the background as it is");
    params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->sRGB;
    const auto decoded = usdcc::usd::opaqueClearColor(background, params);
    CHECK(closeTo(decoded[0], 0.0110f) && decoded[3] == 1.0f, "sRGB: decoded, opaque: %f", decoded[0]);

    if (!color.load(kDefaultColorConfig)) {
        CHECK(false, "the built-in config loads: %s", color.error().c_str());
        return;
    }
    const DisplayView untoned{"sRGB - Display", "Un-tone-mapped"};
    const auto linear = color.fromDisplay({0.4845f, 0.4845f, 0.4845f}, untoned);
    CHECK(closeTo(linear[0], 0.2f), "the inverse of the sRGB encode: %f", linear[0]);
    for (const char* view : {"Un-tone-mapped", "ACES 1.0 - SDR Video"}) {
        params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->openColorIO;
        params.ocioDisplay = PXR_NS::TfToken("sRGB - Display");
        params.ocioView = PXR_NS::TfToken(view);
        const auto clear = usdcc::usd::opaqueClearColor(background, params);
        const auto shown = color.toDisplay({clear[0], clear[1], clear[2]}, {"sRGB - Display", view});
        CHECK(std::abs(shown[0] * 255.0f - 28.0f) < 0.6f && std::abs(shown[2] * 255.0f - 30.0f) < 0.6f,
              "%s shows the clear color as the background: %f %f", view, shown[0] * 255.0f, shown[2] * 255.0f);
    }
}
