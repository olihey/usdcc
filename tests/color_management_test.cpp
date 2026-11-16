// Color management (docs/COLOR_MANAGEMENT.md): OCIO's built-in config, the working
// space, the display and view settings, and what the viewport hands Hydra.
#include "test_harness.h"

#include "usdcc/core/settings.h"
#include "usdcc/usd/color_management.h"
#include "usdcc/usd/linear_exr.h"
#include "usdcc/usd/properties/property_editor.h"
#include "usdcc/usd/viewport_display.h"
#include "usdcc/usd/viewport_render_params.h"

#include <pxr/base/arch/env.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/colorSpaceAPI.h>
#include <pxr/usd/usd/stage.h>

#include <QTemporaryDir>

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

USDCC_TEST(color_management_pickers) {
    usdcc::core::Settings settings;
    ColorManagement color;
    const std::array<float, 3> grey{0.2f, 0.2f, 0.2f};
    // No config: the sRGB transfer, both ways.
    CHECK(closeTo(color.toPicker(grey, settings)[0], 0.4845f), "sRGB encode without a config");
    CHECK(closeTo(color.fromPicker({0.4845f, 0.4845f, 0.4845f}, settings)[1], 0.2f), "and back");
    if (!color.load(kDefaultColorConfig)) {
        CHECK(false, "the built-in config loads: %s", color.error().c_str());
        return;
    }
    CHECK(color.pickingSpace() == "sRGB - Texture", "the CG config's color_picking role: %s", color.pickingSpace().c_str());
    CHECK(closeTo(color.toPicker(grey, settings)[0], 0.4845f), "through the color picking space");
    const auto back = color.fromPicker(color.toPicker({0.7f, 0.05f, 0.3f}, settings), settings);
    CHECK(closeTo(back[0], 0.7f) && closeTo(back[1], 0.05f) && closeTo(back[2], 0.3f), "a round trip");
    // Through the view: ACES rolls 1.0 off, and picking back undoes it.
    settings.define({.key = kColorPickerSpaceKey, .category = kColorCategory, .type = usdcc::core::SettingType::Choice,
                     .defaultValue = std::string(kPickerSpaceColorPicking),
                     .choices = {kPickerSpaceColorPicking, kPickerSpaceView}});
    settings.define({.key = kColorViewKey, .category = kColorCategory, .type = usdcc::core::SettingType::String,
                     .defaultValue = std::string("ACES 1.0 - SDR Video")});
    settings.setValue(kColorPickerSpaceKey, std::string(kPickerSpaceView));
    const auto viewed = color.toPicker({1.0f, 1.0f, 1.0f}, settings);
    CHECK(viewed[0] < 0.95f, "through the ACES view white is below 1: %f", viewed[0]);
    CHECK(closeTo(color.fromPicker(viewed, settings)[0], 1.0f), "and comes back as 1: %f", color.fromPicker(viewed, settings)[0]);
}

USDCC_TEST(color_management_usd_color_spaces) {
    const std::array<float, 3> encoded{0.4845f, 0.4845f, 0.4845f};
    const auto linear = ColorManagement::usdToWorking(encoded, "srgb_rec709_scene");
    CHECK(closeTo(linear[0], 0.2f), "sRGB-encoded Rec.709 is decoded: %f", linear[0]);
    CHECK(closeTo(ColorManagement::workingToUsd(linear, "srgb_rec709_scene")[0], 0.4845f), "and encoded again");
    const auto red = ColorManagement::usdToWorking({1.0f, 0.0f, 0.0f}, "lin_ap1_scene");
    CHECK(red[0] > 1.6f && red[1] < 0.0f, "ACEScg red is outside Rec.709: %f %f %f", red[0], red[1], red[2]);
    const auto white = ColorManagement::usdToWorking({1.0f, 1.0f, 1.0f}, "lin_ap1_scene");
    CHECK(std::abs(white[0] - 1.0f) < 0.01f && std::abs(white[2] - 1.0f) < 0.01f, "white stays white: %f %f", white[0], white[2]);
    const auto back = ColorManagement::workingToUsd(red, "lin_ap1_scene");
    CHECK(std::abs(back[0] - 1.0f) < 1e-3f && std::abs(back[1]) < 1e-3f, "a round trip: %f %f", back[0], back[1]);
    const std::array<float, 3> data{0.3f, 2.0f, -1.0f};
    for (const char* asIs : {"", "data", "raw", "identity", "unknown", "lin_rec709_scene", "no_such_space"}) {
        CHECK(ColorManagement::usdToWorking(data, asIs) == data, "%s stays as it is", asIs);
    }

    // The attribute editor takes the space from UsdColorSpaceAPI, inherited down the namespace.
    auto stage = PXR_NS::UsdStage::CreateInMemory();
    auto root = stage->DefinePrim(PXR_NS::SdfPath("/Root"));
    PXR_NS::UsdColorSpaceAPI::Apply(root).CreateColorSpaceNameAttr().Set(PXR_NS::TfToken("lin_ap1_scene"));
    auto sphere = stage->DefinePrim(PXR_NS::SdfPath("/Root/Ball"), PXR_NS::TfToken("Sphere"));
    auto colorAttr = sphere.CreateAttribute(PXR_NS::TfToken("primvars:displayColor"), PXR_NS::SdfValueTypeNames->Color3f);
    auto other = sphere.CreateAttribute(PXR_NS::TfToken("radius2"), PXR_NS::SdfValueTypeNames->Double);
    CHECK(usdcc::usd::properties::EditorHints::forAttribute(colorAttr).colorSpace == PXR_NS::TfToken("lin_ap1_scene"),
          "a color attribute's hint has its prim's color space");
    CHECK(usdcc::usd::properties::EditorHints::forAttribute(other).colorSpace.IsEmpty(), "a number's has none");
}

USDCC_TEST(color_management_images) {
    usdcc::core::Settings settings;
    ColorManagement color;
    std::vector<float> pixels{0.2f, 0.2f, 0.2f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f};
    std::vector<float> image = pixels;
    color.imageTransform({}, settings)->apply(image.data(), 2);
    CHECK(closeTo(image[0], 0.4845f) && image[3] == 0.5f, "no config: the sRGB encode, alpha kept: %f", image[0]);
    image = pixels;
    color.imageTransform(kRawView, settings)->apply(image.data(), 2);
    CHECK(image == pixels, "Raw leaves them");
    if (!color.load(kDefaultColorConfig)) {
        CHECK(false, "the built-in config loads: %s", color.error().c_str());
        return;
    }
    image = pixels;
    color.imageTransform({}, settings)->apply(image.data(), 2);
    CHECK(closeTo(image[0], 0.4845f) && closeTo(image[4], 1.0f) && image[3] == 0.5f && image[7] == 1.0f,
          "Un-tone-mapped through OCIO: %f %f", image[0], image[4]);
    image = pixels;
    color.imageTransform("ACES 1.0 - SDR Video", settings)->apply(image.data(), 2);
    CHECK(image[4] < 0.95f, "an override's view: ACES rolls red off: %f", image[4]);

    // The LUT: 0.18 sits on a lattice point when the stops divide evenly.
    const int size = 21;  // a point every stop, from -12 to +8
    const DisplayLut lut = color.bakeLut(size, {}, settings);
    CHECK(lut.size == size && lut.rgba.size() == size_t(size) * size * size * 4, "size³ RGBA texels");
    const size_t grey = (static_cast<size_t>(12) * size * size + 12 * size + 12) * 4;
    const float expected = color.toDisplay({0.18f, 0.18f, 0.18f}, color.displayView({}, settings))[0];
    CHECK(closeTo(lut.rgba[grey], expected) && closeTo(lut.rgba[grey + 2], expected) && lut.rgba[grey + 3] == 1.0f,
          "middle grey's texel is its transform: %f vs %f", lut.rgba[grey], expected);
    const size_t redCorner = static_cast<size_t>(size - 1) * 4;  // r at the top, g and b at the bottom
    CHECK(lut.rgba[redCorner] > lut.rgba[redCorner + 1], "red fastest");
    CHECK(color.bakeLut(1, {}, settings).size == 0, "too small: none");
}

USDCC_TEST(color_management_linear_exr) {
    if (!usdcc::usd::linearExrAvailable()) {
        CHECK(false, "built with OpenEXR");
        return;
    }
    QTemporaryDir dir;
    // Two rows of two, top row first: values above 1 and below 0 survive, as linear data does.
    const std::vector<float> rgba{0.2f, 0.5f, 4.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f,
                                  -0.1f, 0.0f, 1.0f, 1.0f, 10.0f, 10.0f, 10.0f, 0.0f};
    const std::string path = dir.filePath(QStringLiteral("linear.exr")).toStdString();
    std::string error;
    CHECK(usdcc::usd::writeLinearExr(path, rgba.data(), 2, 2, &error), "writes: %s", error.c_str());

    // Read back through OpenUSD's own reader (OpenImageIO): float RGBA, as written.
    const PXR_NS::HioImageSharedPtr image = PXR_NS::HioImage::OpenForReading(path);
    CHECK(image && image->GetWidth() == 2 && image->GetHeight() == 2, "a 2 × 2 EXR");
    if (image) {
        CHECK(image->GetFormat() == PXR_NS::HioFormatFloat32Vec4, "float RGBA: format %d", int(image->GetFormat()));
        std::vector<float> back(16, -9.0f);
        PXR_NS::HioImage::StorageSpec storage;
        storage.width = storage.height = 2;
        storage.depth = 1;
        storage.format = PXR_NS::HioFormatFloat32Vec4;
        storage.data = back.data();
        CHECK(image->Read(storage) && back == rgba, "the values, unchanged: %f %f %f", back[0], back[2], back[12]);
    }
    CHECK(!usdcc::usd::writeLinearExr(dir.filePath(QStringLiteral("no/such/folder/x.exr")).toStdString(), rgba.data(), 2,
                                      2, &error) &&
              !error.empty(),
          "a path it can't write fails, saying why: %s", error.c_str());
    CHECK(!usdcc::usd::writeLinearExr(path, nullptr, 2, 2, &error), "no pixels fails");
}
