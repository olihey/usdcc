#include "usdcc/usd/color_management.h"

#include "usdcc/core/log.h"
#include "usdcc/usd/usd_environment.h"

#include <pxr/base/arch/env.h>
#include <pxr/base/gf/color.h>
#include <pxr/base/gf/colorSpace.h>
#include <pxr/base/gf/vec3f.h>

#include <algorithm>
#include <cmath>
#include <optional>

#ifdef USDCC_HAS_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

namespace usdcc::usd {

namespace {

constexpr const char* kLog = "usdcc.color";
constexpr const char* kOcioVariable = "OCIO";
// What linear Rec.709 is called: the ACES configs' alias, their full name, and older
// configs' names.
constexpr const char* kLinearRec709Names[] = {"lin_rec709", "Linear Rec.709 (sRGB)", "lin_srgb", "linear"};

bool contains(const std::vector<std::string>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

#ifdef USDCC_HAS_OCIO
// OCIO's own messages into usdcc's log, instead of stderr.
void routeOcioLogging() {
    static bool routed = false;
    if (routed) return;
    routed = true;
    OCIO::SetLoggingFunction([](const char* message) {
        std::string text = message ? message : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (text.find("Error") != std::string::npos) {
            USDCC_LOG_ERROR(kLog, "{}", text);
        } else if (text.find("Warning") != std::string::npos) {
            USDCC_LOG_WARNING(kLog, "{}", text);
        } else {
            USDCC_LOG_INFO(kLog, "{}", text);
        }
    });
}
#endif

}  // namespace

bool isColorManagementKey(const std::string& key) {
    return key == kColorConfigKey || key == kColorDisplayKey || key == kColorViewKey || key == kColorPickerSpaceKey;
}

namespace {

// The sRGB transfer, for when there's no config: what Hydra's own sRGB mode does.
float srgbEncode(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
float srgbDecode(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

class IdentityTransform : public ImageTransform {
public:
    void apply(float*, size_t) const override {}
};

class SrgbTransform : public ImageTransform {
public:
    void apply(float* rgba, size_t pixels) const override {
        for (size_t i = 0; i < pixels; ++i) {
            for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = srgbEncode(rgba[i * 4 + c]);
        }
    }
};

#ifdef USDCC_HAS_OCIO
class OcioTransform : public ImageTransform {
public:
    explicit OcioTransform(OCIO::ConstCPUProcessorRcPtr processor) : m_processor(std::move(processor)) {}
    void apply(float* rgba, size_t pixels) const override {
        if (!pixels) return;
        OCIO::PackedImageDesc image(rgba, static_cast<long>(pixels), 1, 4);
        m_processor->apply(image);
    }

private:
    OCIO::ConstCPUProcessorRcPtr m_processor;
};
#endif

}  // namespace

struct ColorManagement::Impl {
    std::string source;
    std::string error;
#ifdef USDCC_HAS_OCIO
    OCIO::ConstConfigRcPtr config;
#endif
};

ColorManagement& ColorManagement::instance() {
    static ColorManagement management;
    return management;
}

ColorManagement::ColorManagement() : m_impl(std::make_unique<Impl>()) {}
ColorManagement::~ColorManagement() = default;

bool ColorManagement::load(const std::string& source) {
    unload();
#ifdef USDCC_HAS_OCIO
    routeOcioLogging();
    if (source.empty()) {
        m_impl->error = "no config given";
        return false;
    }
    try {
        OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromFile(source.c_str());
        if (!config || config->getNumDisplays() == 0) {
            m_impl->error = "it has no displays";
            return false;
        }
        OCIO::SetCurrentConfig(config);
        m_impl->config = config;
        m_impl->source = source;
        return true;
    } catch (const std::exception& error) {
        m_impl->error = error.what();
        return false;
    }
#else
    m_impl->error = "usdcc was built without OpenColorIO";
    return false;
#endif
}

void ColorManagement::unload() {
    m_impl->source.clear();
    m_impl->error.clear();
#ifdef USDCC_HAS_OCIO
    m_impl->config.reset();
#endif
}

bool ColorManagement::available() const {
#ifdef USDCC_HAS_OCIO
    return static_cast<bool>(m_impl->config);
#else
    return false;
#endif
}

const std::string& ColorManagement::source() const { return m_impl->source; }
const std::string& ColorManagement::error() const { return m_impl->error; }

std::string ColorManagement::configName() const {
#ifdef USDCC_HAS_OCIO
    if (m_impl->config) return m_impl->config->getName();
#endif
    return {};
}

std::vector<std::string> ColorManagement::displays() const {
    std::vector<std::string> result;
#ifdef USDCC_HAS_OCIO
    if (!m_impl->config) return result;
    for (int i = 0; i < m_impl->config->getNumDisplays(); ++i) result.emplace_back(m_impl->config->getDisplay(i));
#endif
    return result;
}

std::vector<std::string> ColorManagement::views(const std::string& display) const {
    std::vector<std::string> result;
#ifdef USDCC_HAS_OCIO
    if (!m_impl->config || !contains(displays(), display)) return result;
    for (int i = 0; i < m_impl->config->getNumViews(display.c_str()); ++i) {
        result.emplace_back(m_impl->config->getView(display.c_str(), i));
    }
#else
    (void)display;
#endif
    return result;
}

std::string ColorManagement::workingSpace() const {
#ifdef USDCC_HAS_OCIO
    if (!m_impl->config) return {};
    for (const char* name : kLinearRec709Names) {
        if (OCIO::ConstColorSpaceRcPtr space = m_impl->config->getColorSpace(name)) return space->getName();
    }
    if (OCIO::ConstColorSpaceRcPtr space = m_impl->config->getColorSpace(OCIO::ROLE_SCENE_LINEAR)) {
        return space->getName();
    }
#endif
    return {};
}

std::string ColorManagement::defaultDisplay() const {
#ifdef USDCC_HAS_OCIO
    if (m_impl->config) return m_impl->config->getDefaultDisplay();
#endif
    return {};
}

std::string ColorManagement::defaultView(const std::string& display) const {
    const std::vector<std::string> list = views(display);
    if (list.empty()) return {};
    if (contains(list, kPreferredView)) return kPreferredView;
#ifdef USDCC_HAS_OCIO
    const std::string configDefault = m_impl->config->getDefaultView(display.c_str());
    if (contains(list, configDefault)) return configDefault;
#endif
    return list.front();
}

DisplayView ColorManagement::displayView(std::string_view viewOverride, const core::Settings& settings) const {
    if (!available()) return {};
    DisplayView result;
    result.display = settings.get<std::string>(kColorDisplayKey);
    if (!contains(displays(), result.display)) result.display = defaultDisplay();
    const std::vector<std::string> list = views(result.display);
    if (!viewOverride.empty() && contains(list, viewOverride)) {
        result.view = viewOverride;
    } else {
        result.view = settings.get<std::string>(kColorViewKey);
        if (!contains(list, result.view)) result.view = defaultView(result.display);
    }
    return result;
}

namespace {
#ifdef USDCC_HAS_OCIO
// One color through working space -> display/view, either way.
std::array<float, 3> applyDisplayView(const OCIO::ConstConfigRcPtr& config, const std::string& workingSpace,
                                      const std::array<float, 3>& rgb, const DisplayView& target, bool inverse) {
    if (!config) return rgb;
    try {
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(workingSpace.c_str());
        transform->setDisplay(target.display.c_str());
        transform->setView(target.view.c_str());
        // As Hydra's OCIO step does: the view's own looks apply.
        transform->setLooksBypass(false);
        OCIO::ConstCPUProcessorRcPtr processor =
            config->getProcessor(transform, inverse ? OCIO::TRANSFORM_DIR_INVERSE : OCIO::TRANSFORM_DIR_FORWARD)
                ->getDefaultCPUProcessor();
        std::array<float, 3> result = rgb;
        processor->applyRGB(result.data());
        return result;
    } catch (const std::exception& error) {
        USDCC_LOG_WARNING(kLog, "No transform {} {} / {}: {}", inverse ? "from" : "to", target.display, target.view,
                          error.what());
    }
    return rgb;
}
#endif
}  // namespace

std::array<float, 3> ColorManagement::toDisplay(const std::array<float, 3>& rgb, const DisplayView& target) const {
#ifdef USDCC_HAS_OCIO
    return applyDisplayView(m_impl->config, workingSpace(), rgb, target, false);
#else
    (void)target;
    return rgb;
#endif
}

std::array<float, 3> ColorManagement::fromDisplay(const std::array<float, 3>& rgb, const DisplayView& target) const {
#ifdef USDCC_HAS_OCIO
    return applyDisplayView(m_impl->config, workingSpace(), rgb, target, true);
#else
    (void)target;
    return rgb;
#endif
}

std::shared_ptr<const ImageTransform> ColorManagement::imageTransform(std::string_view viewOverride,
                                                                      const core::Settings& settings) const {
    if (viewOverride == kRawView) return std::make_shared<IdentityTransform>();
    if (!available()) return std::make_shared<SrgbTransform>();
    const DisplayView target = displayView(viewOverride, settings);
    if (target.view == kRawView) return std::make_shared<IdentityTransform>();
#ifdef USDCC_HAS_OCIO
    try {
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(workingSpace().c_str());
        transform->setDisplay(target.display.c_str());
        transform->setView(target.view.c_str());
        transform->setLooksBypass(false);
        return std::make_shared<OcioTransform>(m_impl->config->getProcessor(transform)->getDefaultCPUProcessor());
    } catch (const std::exception& error) {
        USDCC_LOG_WARNING(kLog, "No transform to {} / {}, showing sRGB: {}", target.display, target.view, error.what());
    }
#endif
    return std::make_shared<SrgbTransform>();
}

DisplayLut ColorManagement::bakeLut(int size, std::string_view viewOverride, const core::Settings& settings) const {
    DisplayLut lut;
    if (size < 2) return lut;
    lut.size = size;
    const size_t texels = static_cast<size_t>(size) * size * size;
    lut.rgba.resize(texels * 4);
    std::vector<float> lattice(static_cast<size_t>(size));
    for (int i = 0; i < size; ++i) {
        const float stops = kLutMinStops + (kLutMaxStops - kLutMinStops) * static_cast<float>(i) / (size - 1);
        lattice[static_cast<size_t>(i)] = 0.18f * std::exp2(stops);
    }
    size_t texel = 0;
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r, ++texel) {
                float* out = &lut.rgba[texel * 4];
                out[0] = lattice[static_cast<size_t>(r)];
                out[1] = lattice[static_cast<size_t>(g)];
                out[2] = lattice[static_cast<size_t>(b)];
                out[3] = 1.0f;
            }
        }
    }
    imageTransform(viewOverride, settings)->apply(lut.rgba.data(), texels);
    return lut;
}

std::string ColorManagement::pickingSpace() const {
#ifdef USDCC_HAS_OCIO
    if (m_impl->config && m_impl->config->hasRole(OCIO::ROLE_COLOR_PICKING)) {
        if (OCIO::ConstColorSpaceRcPtr space = m_impl->config->getColorSpace(OCIO::ROLE_COLOR_PICKING)) {
            return space->getName();
        }
    }
#endif
    return {};
}

namespace {
#ifdef USDCC_HAS_OCIO
std::array<float, 3> applyColorSpaces(const OCIO::ConstConfigRcPtr& config, const std::string& from,
                                      const std::string& to, const std::array<float, 3>& rgb) {
    try {
        std::array<float, 3> result = rgb;
        config->getProcessor(from.c_str(), to.c_str())->getDefaultCPUProcessor()->applyRGB(result.data());
        return result;
    } catch (const std::exception& error) {
        USDCC_LOG_WARNING(kLog, "No transform from {} to {}: {}", from, to, error.what());
    }
    return rgb;
}
#endif
}  // namespace

std::array<float, 3> ColorManagement::toPicker(const std::array<float, 3>& rgb, const core::Settings& settings) const {
#ifdef USDCC_HAS_OCIO
    if (available()) {
        const DisplayView target = displayView({}, settings);
        if (settings.get<std::string>(kColorPickerSpaceKey) == kPickerSpaceView) return toDisplay(rgb, target);
        if (const std::string picking = pickingSpace(); !picking.empty()) {
            return applyColorSpaces(m_impl->config, workingSpace(), picking, rgb);
        }
        return toDisplay(rgb, {target.display, defaultView(target.display)});
    }
#else
    (void)settings;
#endif
    return {srgbEncode(rgb[0]), srgbEncode(rgb[1]), srgbEncode(rgb[2])};
}

std::array<float, 3> ColorManagement::fromPicker(const std::array<float, 3>& rgb, const core::Settings& settings) const {
#ifdef USDCC_HAS_OCIO
    if (available()) {
        const DisplayView target = displayView({}, settings);
        if (settings.get<std::string>(kColorPickerSpaceKey) == kPickerSpaceView) return fromDisplay(rgb, target);
        if (const std::string picking = pickingSpace(); !picking.empty()) {
            return applyColorSpaces(m_impl->config, picking, workingSpace(), rgb);
        }
        return fromDisplay(rgb, {target.display, defaultView(target.display)});
    }
#else
    (void)settings;
#endif
    return {srgbDecode(rgb[0]), srgbDecode(rgb[1]), srgbDecode(rgb[2])};
}

namespace {
// The OpenUSD color space a color is converted from, or nothing for one that stays as it is.
std::optional<PXR_NS::GfColorSpace> gfColorSpace(const std::string& name) {
    if (name.empty()) return std::nullopt;
    const PXR_NS::TfToken token(name);
    for (const PXR_NS::TfToken& asIs : {PXR_NS::GfColorSpaceNames->Data, PXR_NS::GfColorSpaceNames->Raw,
                                        PXR_NS::GfColorSpaceNames->Identity, PXR_NS::GfColorSpaceNames->Unknown,
                                        PXR_NS::GfColorSpaceNames->LinearRec709}) {
        if (token == asIs) return std::nullopt;
    }
    if (!PXR_NS::GfColorSpace::IsValid(token)) return std::nullopt;
    return PXR_NS::GfColorSpace(token);
}

std::array<float, 3> convert(const std::array<float, 3>& rgb, const PXR_NS::GfColorSpace& from,
                             const PXR_NS::GfColorSpace& to) {
    const PXR_NS::GfColor converted(PXR_NS::GfColor(PXR_NS::GfVec3f(rgb[0], rgb[1], rgb[2]), from), to);
    const PXR_NS::GfVec3f out = converted.GetRGB();
    return {out[0], out[1], out[2]};
}
}  // namespace

std::array<float, 3> ColorManagement::usdToWorking(const std::array<float, 3>& rgb, const std::string& usdColorSpace) {
    const auto space = gfColorSpace(usdColorSpace);
    if (!space) return rgb;
    return convert(rgb, *space, PXR_NS::GfColorSpace(PXR_NS::GfColorSpaceNames->LinearRec709));
}

std::array<float, 3> ColorManagement::workingToUsd(const std::array<float, 3>& rgb, const std::string& usdColorSpace) {
    const auto space = gfColorSpace(usdColorSpace);
    if (!space) return rgb;
    return convert(rgb, PXR_NS::GfColorSpace(PXR_NS::GfColorSpaceNames->LinearRec709), *space);
}

namespace {

void defineConfigSetting(core::Settings& settings, const std::string& status) {
    std::string description =
        "The OpenColorIO config: a .ocio file, or a built-in one such as ocio://default (OCIO's CG config, ACES "
        "views and the common displays). The OCIO environment variable wins when it's set.";
    if (!status.empty()) description += " " + status;
    settings.define({
        .key = kColorConfigKey,
        .category = kColorCategory,
        .group = "Config",
        .label = "OCIO config",
        .description = description,
        .type = core::SettingType::String,
        .defaultValue = std::string(kDefaultColorConfig),
        .appliesAtNextStart = true,
    });
}

void defineViewSetting(core::Settings& settings) {
    const ColorManagement& color = ColorManagement::instance();
    std::string display = settings.get<std::string>(kColorDisplayKey);
    const std::vector<std::string> displays = color.displays();
    if (std::find(displays.begin(), displays.end(), display) == displays.end()) display = color.defaultDisplay();
    const std::vector<std::string> views = color.views(display);
    if (views.empty()) return;
    settings.define({
        .key = kColorViewKey,
        .category = kColorCategory,
        .group = "Display",
        .label = "View",
        .description = "How the working space's linear values are turned into the display's: Un-tone-mapped is a "
                       "plain encode for the display, an ACES view rolls highlights off as film does, Raw shows the "
                       "linear values as they are. A viewport can pick its own (its options menu, View Transform).",
        .type = core::SettingType::Choice,
        .defaultValue = color.defaultView(display),
        .choices = views,
    });
}

std::optional<core::Signal<const std::string&>::Connection>& displayConnection() {
    static std::optional<core::Signal<const std::string&>::Connection> connection;
    return connection;
}

}  // namespace

void defineColorManagementSettings(core::Settings& settings) { defineConfigSetting(settings, {}); }

void applyColorManagementSettings(core::Settings& settings) {
    ColorManagement& color = ColorManagement::instance();
    const std::string inherited = PXR_NS::ArchGetEnv(kOcioVariable);
    const bool fromEnvironment = !inherited.empty();
    const std::string source = fromEnvironment ? inherited : settings.get<std::string>(kColorConfigKey);

    std::string status;
    if (color.load(source)) {
        // Hydra only uses OCIO while the variable is set; child processes (V-Ray's helper) get it too.
        if (!fromEnvironment) setUsdEnvironmentVariable(kOcioVariable, source, "Settings");
        status = "In use: " + color.configName() + (fromEnvironment ? ", from the OCIO environment variable" : "") +
                 ". Working space: " + color.workingSpace() + ".";
        USDCC_LOG_INFO(kLog, "OCIO config {} ({}{}), working space {}", color.configName(), source,
                       fromEnvironment ? ", from OCIO" : "", color.workingSpace());
    } else {
        status = "Not in use: " + source + " doesn't load (" + color.error() +
                 "). The viewport shows a plain sRGB encode instead.";
        USDCC_LOG_WARNING(kLog, "Color management is off: the OCIO config {}{} doesn't load: {}. The viewport shows "
                                "a plain sRGB encode instead.",
                          source, fromEnvironment ? " (from OCIO)" : "", color.error());
    }
    defineConfigSetting(settings, status);

    displayConnection().reset();
    if (!color.available()) return;
    settings.define({
        .key = kColorDisplayKey,
        .category = kColorCategory,
        .group = "Display",
        .label = "Display",
        .description = "The monitor the images are shown on, as the config names it.",
        .type = core::SettingType::Choice,
        .defaultValue = color.defaultDisplay(),
        .choices = color.displays(),
    });
    defineViewSetting(settings);
    settings.define({
        .key = kColorPickerSpaceKey,
        .category = kColorCategory,
        .group = "Pickers",
        .label = "Swatches show",
        .description = "What the attribute editor's color swatches and the color picker show a color as. Color "
                       "picking is the config's space for it, which picks back exactly what was shown. Through "
                       "the view shows the color as the viewport does, tone mapping included.",
        .type = core::SettingType::Choice,
        .defaultValue = std::string(kPickerSpaceColorPicking),
        .choices = {kPickerSpaceColorPicking, kPickerSpaceView},
    });
    // Each display has its own views.
    displayConnection() = settings.changed.connect([&settings](const std::string& key) {
        if (key == kColorDisplayKey) defineViewSetting(settings);
    });
}

}  // namespace usdcc::usd
