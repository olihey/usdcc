#pragma once

// Color management (docs/COLOR_MANAGEMENT.md): one OpenColorIO config for the
// process, the working space colors are in, and the display and view that
// images take on their way to the screen. Settings > Color Management holds
// the choices; the Hydra viewport passes them to Hydra's own OCIO step.
//
// The config is chosen once, at start-up: Hydra reads it from OCIO's current
// config and only uses it while $OCIO is set, so applyColorManagementSettings()
// loads it and sets the variable before anything draws. Display and view apply
// at once. No looks: Hydra's OCIO step mishandles them (it sets the display to
// the look's name), so a view's own looks are all there is.
//
// Qt-free; OpenColorIO stays inside the .cpp. Built without OpenColorIO,
// nothing loads and viewports fall back to Hydra's sRGB encode.

#include "usdcc/core/settings.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace usdcc::usd {

inline constexpr const char* kColorCategory = "Color Management";
inline constexpr const char* kColorConfigKey = "Color/config";
inline constexpr const char* kColorDisplayKey = "Color/display";
inline constexpr const char* kColorViewKey = "Color/view";
inline constexpr const char* kColorPickerSpaceKey = "Color/pickerSpace";

// What swatches and the color picker show (docs/COLOR_MANAGEMENT.md §7): the config's
// color picking space, which round-trips, or the view the viewport shows with.
inline constexpr const char* kPickerSpaceColorPicking = "Color picking";
inline constexpr const char* kPickerSpaceView = "Through the view";

// OCIO 2.2's built-in CG config: ACES views and the common displays, no file to ship.
inline constexpr const char* kDefaultColorConfig = "ocio://default";
// The view a display starts with when the config has it: the plain sRGB-style encode,
// no tone mapping (docs/COLOR_MANAGEMENT.md decision 1).
inline constexpr const char* kPreferredView = "Un-tone-mapped";
// Linear values as they are. A viewport can pick it without a config too: Hydra's
// correction off.
inline constexpr const char* kRawView = "Raw";

// Whether `key` is one of the settings above: viewports redraw when they change.
bool isColorManagementKey(const std::string& key);

// Where colors go to be seen: an OCIO display and one of its views.
struct DisplayView {
    std::string display;
    std::string view;
    bool operator==(const DisplayView&) const = default;
};

// Working-space pixels to display pixels, for images that don't go through Hydra (the
// render view panels). Made once per display and view; apply() is thread-safe.
class ImageTransform {
public:
    virtual ~ImageTransform() = default;
    // `pixels` RGBA floats, in place; alpha is left alone.
    virtual void apply(float* rgba, size_t pixels) const = 0;
};

// A 3D LUT of the transform, for the GPU (RTX's presentation). Its input is shaped:
// a working-space value v is looked up at (log2(v / 0.18) - kLutMinStops) /
// (kLutMaxStops - kLutMinStops), clamped to 0-1, per channel.
struct DisplayLut {
    int size = 0;
    std::vector<float> rgba;  // size³ texels, red fastest, then green, then blue
};
inline constexpr float kLutMinStops = -12.0f;  // 0.18 * 2^-12: black, near enough
inline constexpr float kLutMaxStops = 8.0f;    // 0.18 * 2^8 = 46: highlights to roll off

class ColorManagement {
public:
    // The process's one instance, which the viewports use.
    static ColorManagement& instance();

    ColorManagement();
    ~ColorManagement();
    ColorManagement(const ColorManagement&) = delete;
    ColorManagement& operator=(const ColorManagement&) = delete;

    // Loads a config: a .ocio file or a built-in "ocio://" URI. False, with error()
    // saying why, when it can't (nothing is loaded then). Makes it OCIO's current
    // config, which is the one Hydra uses.
    bool load(const std::string& source);
    void unload();

    // Whether a config is loaded (and usdcc was built with OpenColorIO).
    bool available() const;
    // What load() was given; "" when nothing is loaded.
    const std::string& source() const;
    // Why the last load() failed; "" after one that worked.
    const std::string& error() const;
    // The config's own name, e.g. "cg-config-v1.0.0_aces-v1.3_ocio-v2.1".
    std::string configName() const;

    // The config's active displays, and a display's views, in the config's order.
    std::vector<std::string> displays() const;
    std::vector<std::string> views(const std::string& display) const;

    // The color space renderers compute in and USD's colors are in. Linear Rec.709
    // (sRGB primaries) when the config has it, which Storm, RTX and V-Ray assume;
    // else the config's scene_linear role. Not scene_linear first: the built-in CG
    // config's is ACEScg.
    std::string workingSpace() const;

    // The config's default display, and the view a display starts with: Un-tone-mapped
    // when it has one, else its first view.
    std::string defaultDisplay() const;
    std::string defaultView(const std::string& display) const;

    // What to show with: the display and view settings, each checked against the config
    // (its default when it isn't one of the config's). `viewOverride`, a viewport's own
    // view, wins when it is one of the display's views. Empty when nothing is loaded.
    DisplayView displayView(std::string_view viewOverride = {},
                            const core::Settings& settings = core::Settings::instance()) const;

    // One working-space color as `target` shows it, in 0-1 display values; `rgb` as it
    // is when nothing is loaded or the transform fails. For tests and swatches.
    std::array<float, 3> toDisplay(const std::array<float, 3>& rgb, const DisplayView& target) const;
    // The reverse: the working-space color `target` shows as the display color `rgb`.
    // Exact for an un-tone-mapped view; a tone-mapped one inverts within its range.
    std::array<float, 3> fromDisplay(const std::array<float, 3>& rgb, const DisplayView& target) const;

    // Images: Settings' display and view, or `viewOverride` as displayView() takes it.
    // Raw is an identity; without a config, the sRGB encode.
    std::shared_ptr<const ImageTransform> imageTransform(std::string_view viewOverride = {},
                                                         const core::Settings& settings = core::Settings::instance()) const;
    // The same as a LUT of `size`³ (docs/COLOR_MANAGEMENT.md §5).
    DisplayLut bakeLut(int size, std::string_view viewOverride = {},
                       const core::Settings& settings = core::Settings::instance()) const;

    // The config's color_picking role's space; "" when it has none.
    std::string pickingSpace() const;
    // Swatches and the picker (§7): a working-space color as the picker shows it, 0-1
    // for what a swatch can show, and back. The color picking space, else the display's
    // default view; the view itself when the picker space setting says so. Without a
    // config, the sRGB encode.
    std::array<float, 3> toPicker(const std::array<float, 3>& rgb,
                                  const core::Settings& settings = core::Settings::instance()) const;
    std::array<float, 3> fromPicker(const std::array<float, 3>& rgb,
                                    const core::Settings& settings = core::Settings::instance()) const;

    // A color in one of OpenUSD's color spaces (UsdColorSpaceAPI, GfColorSpaceNames: "lin_ap1_scene",
    // "srgb_rec709_scene", ...) in the working space, and back. "" and the ones that aren't
    // colors (data, raw, identity, unknown) or aren't known stay as they are.
    static std::array<float, 3> usdToWorking(const std::array<float, 3>& rgb, const std::string& usdColorSpace);
    static std::array<float, 3> workingToUsd(const std::array<float, 3>& rgb, const std::string& usdColorSpace);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Settings > Color Management (docs/SETTINGS.md §4): the config, then, once one is
// loaded, the display and view as choices from it.
void defineColorManagementSettings(core::Settings& settings = core::Settings::instance());

// At start-up, after defineColorManagementSettings() and before any viewport draws:
// loads $OCIO when it's set, else the config setting, sets OCIO for Hydra (and child
// processes), and defines the display and view choices. A display change re-lists the
// view's choices for it.
void applyColorManagementSettings(core::Settings& settings = core::Settings::instance());

}  // namespace usdcc::usd
