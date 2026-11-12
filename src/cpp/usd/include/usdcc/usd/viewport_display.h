#pragma once

// The viewport's grid and background (docs/VIEWPORT_GRID.md), its selection style
// (docs/VIEWPORT_SELECTION.md), and how Hydra draws the stage: shading, complexity,
// purposes, materials, culling and lights (docs/VIEWPORT_DISPLAY.md). The app-wide
// defaults in Settings > Viewport, and what one viewport panel overrides of them.

#include "usdcc/usd/hydra_viewport_window.h"

#include <QColor>
#include <QJsonObject>

#include <optional>
#include <string>

namespace usdcc::usd {

// The defaults' Settings keys (docs/SETTINGS.md §4).
inline constexpr const char* kViewportShowGridKey = "Viewport/showGrid";
inline constexpr const char* kViewportGridSizeKey = "Viewport/gridSize";
inline constexpr const char* kViewportGridStepKey = "Viewport/gridStep";
inline constexpr const char* kViewportBackgroundKey = "Viewport/backgroundColor";
inline constexpr const char* kViewportSelectionStyleKey = "Viewport/selectionStyle";
inline constexpr const char* kViewportSelectionColorKey = "Viewport/selectionColor";
inline constexpr const char* kViewportSelectionWidthKey = "Viewport/selectionWidth";
inline constexpr const char* kViewportComplexityKey = "Viewport/complexity";
inline constexpr const char* kViewportShowGuidesKey = "Viewport/showGuides";
inline constexpr const char* kViewportShowProxyKey = "Viewport/showProxy";
inline constexpr const char* kViewportShowRenderKey = "Viewport/showRender";
inline constexpr const char* kViewportCullBackfacesKey = "Viewport/cullBackfaces";
inline constexpr const char* kViewportSceneLightsKey = "Viewport/sceneLights";
inline constexpr const char* kViewportCameraLightKey = "Viewport/cameraLight";
inline constexpr const char* kViewportDomeLightKey = "Viewport/domeLight";
// The HUD's (docs/VIEWPORT_HUD.md §5).
inline constexpr const char* kViewportHudKey = "Viewport/hud";
inline constexpr const char* kViewportHudCameraKey = "Viewport/hudCamera";
inline constexpr const char* kViewportHudAxesKey = "Viewport/hudAxes";
inline constexpr const char* kViewportHudStatsKey = "Viewport/hudStats";
inline constexpr const char* kViewportHudPerformanceKey = "Viewport/hudPerformance";
inline constexpr const char* kViewportHudToolReadoutKey = "Viewport/hudToolReadout";
inline constexpr const char* kViewportHudRolloverKey = "Viewport/hudRollover";

// Whether `key` is one of the settings above: a viewport follows them as they change.
bool isViewportDisplayKey(const std::string& key);

// The selection line width's range, in UI pixels.
inline constexpr int kMinSelectionWidth = 1;
inline constexpr int kMaxSelectionWidth = 8;

// A style's name: "solid", "outline", "wireframe" (sessions), and back; nothing for
// another name.
const char* selectionStyleName(SelectionStyle style);
std::optional<SelectionStyle> selectionStyleFromName(const QString& name);
// What a viewport's options and dialog call it: "Solid", "Outline", "Wireframe" (also
// the Settings choice's values).
QString selectionStyleLabel(SelectionStyle style);

// The same for the shading modes: "smooth", "flat", "wireframeOnShaded", "wireframe",
// "hiddenLine" (sessions); "Smooth Shaded", "Flat Shaded", "Wireframe on Shaded",
// "Wireframe", "Hidden Line" (the menu and dialog).
inline constexpr ShadingMode kShadingModes[] = {ShadingMode::SmoothShaded, ShadingMode::FlatShaded,
                                                ShadingMode::WireframeOnShaded, ShadingMode::Wireframe,
                                                ShadingMode::HiddenLine};
const char* shadingModeName(ShadingMode shading);
std::optional<ShadingMode> shadingModeFromName(const QString& name);
QString shadingModeLabel(ShadingMode shading);

// And the complexities: "low", "medium", "high", "veryHigh"; "Low", "Medium", "High",
// "Very High" (also the Settings choice's values).
inline constexpr Complexity kComplexities[] = {Complexity::Low, Complexity::Medium, Complexity::High,
                                               Complexity::VeryHigh};
const char* complexityName(Complexity complexity);
std::optional<Complexity> complexityFromName(const QString& name);
QString complexityLabel(Complexity complexity);

// Defines the settings; safe to call again (a definition again keeps the value).
// main.cpp calls it at start so the Settings dialog has them before a viewport opens.
void defineViewportDisplaySettings();

// What a viewport without overrides shows: the settings' values.
HydraViewportWindow::DisplayOptions defaultViewportDisplay();

// What one viewport sets for itself. Each value is either set, and kept whatever the
// default becomes, or not, and follows its setting as it changes.
struct ViewportDisplayOverrides {
    std::optional<bool> showGrid;
    std::optional<double> gridSize;
    std::optional<double> gridStep;
    std::optional<QColor> background;
    std::optional<SelectionStyle> selectionStyle;
    std::optional<QColor> selectionColor;
    std::optional<int> selectionWidth;
    // Shading, scene materials and dome light textures have no Settings default: without an
    // override a viewport shows the built-in one (Smooth Shaded, materials and maps on).
    std::optional<ShadingMode> shading;
    std::optional<Complexity> complexity;
    std::optional<bool> showGuides;
    std::optional<bool> showProxy;
    std::optional<bool> showRender;
    std::optional<bool> sceneMaterials;
    std::optional<bool> cullBackfaces;
    std::optional<bool> domeLightTextures;
    std::optional<bool> sceneLights;
    std::optional<bool> cameraLight;
    std::optional<bool> domeLight;
    std::optional<bool> hud;
    std::optional<bool> hudCamera;
    std::optional<bool> hudAxes;
    std::optional<bool> hudStats;
    std::optional<bool> hudPerformance;
    std::optional<bool> hudToolReadout;
    std::optional<bool> hudRollover;

    bool isEmpty() const { return *this == ViewportDisplayOverrides{}; }
    bool operator==(const ViewportDisplayOverrides&) const = default;

    // `defaults` with these values in place of theirs.
    HydraViewportWindow::DisplayOptions applyTo(HydraViewportWindow::DisplayOptions defaults) const;

    // For a session (docs/SESSIONS.md §2.4): only the values that are set, as
    // {"showGrid": bool, "gridSize": number, "gridStep": number, "background": "#rrggbb",
    //  "selectionStyle": "outline", "selectionColor": "#rrggbb", "selectionWidth": int,
    //  "shading": "hiddenLine", "complexity": "high", and the switches as bools:
    //  "showGuides", "showProxy", "showRender", "sceneMaterials", "cullBackfaces",
    //  "domeLightTextures", "sceneLights", "cameraLight", "domeLight", "hud", "hudCamera",
    //  "hudAxes", "hudStats", "hudPerformance", "hudToolReadout", "hudRollover"}.
    QJsonObject toJson() const;
    // Ignores what doesn't fit: a size or step that isn't positive, a color that isn't one,
    // a style, shading or complexity that isn't one, a width outside 1-8, a switch that
    // isn't a bool.
    static ViewportDisplayOverrides fromJson(const QJsonObject& json);
};

}  // namespace usdcc::usd
