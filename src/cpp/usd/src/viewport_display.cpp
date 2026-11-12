#include "usdcc/usd/viewport_display.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace usdcc::usd {

namespace {
// Each value's range: the Settings dialog's spin boxes keep to it, and a session's value
// outside it is ignored.
constexpr double kMinGridSize = 0.01;
constexpr double kMaxGridSize = 1000000.0;
constexpr double kMinGridStep = 0.001;
constexpr double kMaxGridStep = 100000.0;

bool positive(double value, double minimum, double maximum) { return value >= minimum && value <= maximum; }

constexpr SelectionStyle kSelectionStyles[] = {SelectionStyle::Solid, SelectionStyle::Outline, SelectionStyle::Wireframe};

using Options = HydraViewportWindow::DisplayOptions;
using Overrides = ViewportDisplayOverrides;

// The on/off options: their session name, override and value, and their setting, for
// those that have one.
struct Switch {
    const char* name;
    std::optional<bool> Overrides::*override;
    bool Options::*option;
    const char* key;  // nullptr: no Settings default
};
constexpr Switch kSwitches[] = {
    {"showGuides", &Overrides::showGuides, &Options::showGuides, kViewportShowGuidesKey},
    {"showProxy", &Overrides::showProxy, &Options::showProxy, kViewportShowProxyKey},
    {"showRender", &Overrides::showRender, &Options::showRender, kViewportShowRenderKey},
    {"sceneMaterials", &Overrides::sceneMaterials, &Options::sceneMaterials, nullptr},
    {"cullBackfaces", &Overrides::cullBackfaces, &Options::cullBackfaces, kViewportCullBackfacesKey},
    {"domeLightTextures", &Overrides::domeLightTextures, &Options::domeLightTextures, nullptr},
    {"sceneLights", &Overrides::sceneLights, &Options::sceneLights, kViewportSceneLightsKey},
    {"cameraLight", &Overrides::cameraLight, &Options::cameraLight, kViewportCameraLightKey},
    {"domeLight", &Overrides::domeLight, &Options::domeLight, kViewportDomeLightKey},
};

// A name or label back to its value, ignoring case.
template <typename T, size_t N>
std::optional<T> fromName(const QString& name, const T (&values)[N], const char* (*nameOf)(T), QString (*labelOf)(T)) {
    for (T value : values) {
        if (name.compare(QLatin1String(nameOf(value)), Qt::CaseInsensitive) == 0 ||
            name.compare(labelOf(value), Qt::CaseInsensitive) == 0) {
            return value;
        }
    }
    return std::nullopt;
}
}  // namespace

bool isViewportDisplayKey(const std::string& key) {
    for (const char* known : {kViewportShowGridKey, kViewportGridSizeKey, kViewportGridStepKey, kViewportBackgroundKey,
                              kViewportSelectionStyleKey, kViewportSelectionColorKey, kViewportSelectionWidthKey,
                              kViewportComplexityKey}) {
        if (key == known) return true;
    }
    for (const Switch& toggle : kSwitches) {
        if (toggle.key && key == toggle.key) return true;
    }
    return false;
}

const char* shadingModeName(ShadingMode shading) {
    switch (shading) {
        case ShadingMode::FlatShaded: return "flat";
        case ShadingMode::WireframeOnShaded: return "wireframeOnShaded";
        case ShadingMode::Wireframe: return "wireframe";
        case ShadingMode::HiddenLine: return "hiddenLine";
        case ShadingMode::SmoothShaded: break;
    }
    return "smooth";
}

QString shadingModeLabel(ShadingMode shading) {
    switch (shading) {
        case ShadingMode::FlatShaded: return QStringLiteral("Flat Shaded");
        case ShadingMode::WireframeOnShaded: return QStringLiteral("Wireframe on Shaded");
        case ShadingMode::Wireframe: return QStringLiteral("Wireframe");
        case ShadingMode::HiddenLine: return QStringLiteral("Hidden Line");
        case ShadingMode::SmoothShaded: break;
    }
    return QStringLiteral("Smooth Shaded");
}

std::optional<ShadingMode> shadingModeFromName(const QString& name) {
    return fromName(name, kShadingModes, &shadingModeName, &shadingModeLabel);
}

const char* complexityName(Complexity complexity) {
    switch (complexity) {
        case Complexity::Medium: return "medium";
        case Complexity::High: return "high";
        case Complexity::VeryHigh: return "veryHigh";
        case Complexity::Low: break;
    }
    return "low";
}

QString complexityLabel(Complexity complexity) {
    switch (complexity) {
        case Complexity::Medium: return QStringLiteral("Medium");
        case Complexity::High: return QStringLiteral("High");
        case Complexity::VeryHigh: return QStringLiteral("Very High");
        case Complexity::Low: break;
    }
    return QStringLiteral("Low");
}

std::optional<Complexity> complexityFromName(const QString& name) {
    return fromName(name, kComplexities, &complexityName, &complexityLabel);
}

const char* selectionStyleName(SelectionStyle style) {
    switch (style) {
        case SelectionStyle::Outline: return "outline";
        case SelectionStyle::Wireframe: return "wireframe";
        case SelectionStyle::Solid: break;
    }
    return "solid";
}

std::optional<SelectionStyle> selectionStyleFromName(const QString& name) {
    for (SelectionStyle style : kSelectionStyles) {
        if (name.compare(QLatin1String(selectionStyleName(style)), Qt::CaseInsensitive) == 0) return style;
    }
    return std::nullopt;
}

QString selectionStyleLabel(SelectionStyle style) {
    switch (style) {
        case SelectionStyle::Outline: return QStringLiteral("Outline");
        case SelectionStyle::Wireframe: return QStringLiteral("Wireframe");
        case SelectionStyle::Solid: break;
    }
    return QStringLiteral("Solid");
}

void defineViewportDisplaySettings() {
    using usdcc::core::SettingType;
    auto& settings = usdcc::core::Settings::instance();
    const HydraViewportWindow::DisplayOptions builtIn;
    settings.define({
        .key = kViewportShowGridKey,
        .category = "Viewport",
        .group = "Grid",
        .label = "Show grid",
        .description = "Draw a grid on the ground plane in new viewports. A viewport can turn it on or off for itself "
                       "(its options menu, Display...).",
        .type = SettingType::Bool,
        .defaultValue = builtIn.showGrid,
    });
    settings.define({
        .key = kViewportGridSizeKey,
        .category = "Viewport",
        .group = "Grid",
        .label = "Grid size",
        .description = "How wide the grid is, in scene units, centred on the origin.",
        .type = SettingType::Float,
        .defaultValue = builtIn.gridSize,
        .minimum = kMinGridSize,
        .maximum = kMaxGridSize,
    });
    settings.define({
        .key = kViewportGridStepKey,
        .category = "Viewport",
        .group = "Grid",
        .label = "Grid step",
        .description = "The distance between grid lines, in scene units. Every tenth line is drawn brighter. A step "
                       "too fine for the size is coarsened so that at most 1001 lines are drawn each way.",
        .type = SettingType::Float,
        .defaultValue = builtIn.gridStep,
        .minimum = kMinGridStep,
        .maximum = kMaxGridStep,
    });
    settings.define({
        .key = kViewportBackgroundKey,
        .category = "Viewport",
        .group = "Background",
        .label = "Background color",
        .description = "The color behind the stage in the Hydra viewport.",
        .type = SettingType::Color,
        .defaultValue = builtIn.background.name().toStdString(),
    });
    std::vector<std::string> styles;
    for (SelectionStyle style : kSelectionStyles) styles.push_back(selectionStyleLabel(style).toStdString());
    settings.define({
        .key = kViewportSelectionStyleKey,
        .category = "Viewport",
        .group = "Selection",
        .label = "Selection style",
        .description = "How the viewport shows what's selected: filled with the selection color (Solid), a line "
                       "around it (Outline), or its edges (Wireframe). A viewport can choose its own (its options "
                       "menu, Selection).",
        .type = SettingType::Choice,
        .defaultValue = selectionStyleLabel(builtIn.selectionStyle).toStdString(),
        .choices = styles,
    });
    settings.define({
        .key = kViewportSelectionColorKey,
        .category = "Viewport",
        .group = "Selection",
        .label = "Selection color",
        .description = "The color the selection is drawn in, in every style.",
        .type = SettingType::Color,
        .defaultValue = builtIn.selectionColor.name().toStdString(),
    });
    settings.define({
        .key = kViewportSelectionWidthKey,
        .category = "Viewport",
        .group = "Selection",
        .label = "Selection line width",
        .description = "How wide the Outline and Wireframe styles draw their lines, in pixels at 100% UI scale.",
        .type = SettingType::Int,
        .defaultValue = builtIn.selectionWidth,
        .minimum = kMinSelectionWidth,
        .maximum = kMaxSelectionWidth,
    });

    // How Hydra draws the stage (docs/VIEWPORT_DISPLAY.md).
    std::vector<std::string> complexities;
    for (Complexity complexity : kComplexities) complexities.push_back(complexityLabel(complexity).toStdString());
    settings.define({
        .key = kViewportComplexityKey,
        .category = "Viewport",
        .group = "Display",
        .label = "Complexity",
        .description = "How finely subdivision surfaces are refined. Higher is smoother and slower. A viewport can "
                       "choose its own (its options menu, Complexity).",
        .type = SettingType::Choice,
        .defaultValue = complexityLabel(builtIn.complexity).toStdString(),
        .choices = complexities,
    });
    settings.define({
        .key = kViewportShowGuidesKey,
        .category = "Viewport",
        .group = "Display",
        .label = "Show guides",
        .description = "Draw geometry whose purpose is guide, such as a rig's controls.",
        .type = SettingType::Bool,
        .defaultValue = builtIn.showGuides,
    });
    settings.define({
        .key = kViewportShowProxyKey,
        .category = "Viewport",
        .group = "Display",
        .label = "Show proxy",
        .description = "Draw geometry whose purpose is proxy: the light stand-ins for render geometry.",
        .type = SettingType::Bool,
        .defaultValue = builtIn.showProxy,
    });
    settings.define({
        .key = kViewportShowRenderKey,
        .category = "Viewport",
        .group = "Display",
        .label = "Show render",
        .description = "Draw geometry whose purpose is render: the full geometry a proxy stands in for.",
        .type = SettingType::Bool,
        .defaultValue = builtIn.showRender,
    });
    settings.define({
        .key = kViewportCullBackfacesKey,
        .category = "Viewport",
        .group = "Display",
        .label = "Cull backfaces",
        .description = "Leave out the back of faces, except on double-sided geometry. Shows flipped normals.",
        .type = SettingType::Bool,
        .defaultValue = builtIn.cullBackfaces,
    });
    settings.define({
        .key = kViewportSceneLightsKey,
        .category = "Viewport",
        .group = "Lights",
        .label = "Scene lights",
        .description = "Light the stage with its own lights.",
        .type = SettingType::Bool,
        .defaultValue = builtIn.sceneLights,
    });
    settings.define({
        .key = kViewportCameraLightKey,
        .category = "Viewport",
        .group = "Lights",
        .label = "Camera light",
        .description = "A light at the camera, besides the stage's own. Storm only.",
        .type = SettingType::Bool,
        .defaultValue = builtIn.cameraLight,
    });
    settings.define({
        .key = kViewportDomeLightKey,
        .category = "Viewport",
        .group = "Lights",
        .label = "Dome light",
        .description = "Hydra's default dome light, besides the stage's own lights.",
        .type = SettingType::Bool,
        .defaultValue = builtIn.domeLight,
    });
}

HydraViewportWindow::DisplayOptions defaultViewportDisplay() {
    // So a viewport made without main.cpp's call (tests) has them.
    if (!usdcc::core::Settings::instance().contains(kViewportDomeLightKey)) defineViewportDisplaySettings();
    const auto& settings = usdcc::core::Settings::instance();
    HydraViewportWindow::DisplayOptions options;
    options.showGrid = settings.get<bool>(kViewportShowGridKey);
    options.gridSize = settings.get<double>(kViewportGridSizeKey);
    options.gridStep = settings.get<double>(kViewportGridStepKey);
    const QColor background = usdcc::ui::settingColor(kViewportBackgroundKey);
    if (background.isValid()) options.background = background;
    if (const auto style = selectionStyleFromName(QString::fromStdString(settings.get<std::string>(kViewportSelectionStyleKey)))) {
        options.selectionStyle = *style;
    }
    const QColor selection = usdcc::ui::settingColor(kViewportSelectionColorKey);
    if (selection.isValid()) options.selectionColor = selection;
    options.selectionWidth = std::clamp(settings.get<int>(kViewportSelectionWidthKey), kMinSelectionWidth, kMaxSelectionWidth);
    if (const auto complexity = complexityFromName(QString::fromStdString(settings.get<std::string>(kViewportComplexityKey)))) {
        options.complexity = *complexity;
    }
    for (const Switch& toggle : kSwitches) {
        if (toggle.key) options.*toggle.option = settings.get<bool>(toggle.key);
    }
    return options;
}

HydraViewportWindow::DisplayOptions ViewportDisplayOverrides::applyTo(HydraViewportWindow::DisplayOptions defaults) const {
    if (showGrid) defaults.showGrid = *showGrid;
    if (gridSize) defaults.gridSize = *gridSize;
    if (gridStep) defaults.gridStep = *gridStep;
    if (background) defaults.background = *background;
    if (selectionStyle) defaults.selectionStyle = *selectionStyle;
    if (selectionColor) defaults.selectionColor = *selectionColor;
    if (selectionWidth) defaults.selectionWidth = *selectionWidth;
    if (shading) defaults.shading = *shading;
    if (complexity) defaults.complexity = *complexity;
    for (const Switch& toggle : kSwitches) {
        if (const std::optional<bool>& value = this->*toggle.override) defaults.*toggle.option = *value;
    }
    return defaults;
}

QJsonObject ViewportDisplayOverrides::toJson() const {
    QJsonObject json;
    if (showGrid) json.insert(QStringLiteral("showGrid"), *showGrid);
    if (gridSize) json.insert(QStringLiteral("gridSize"), *gridSize);
    if (gridStep) json.insert(QStringLiteral("gridStep"), *gridStep);
    if (background) json.insert(QStringLiteral("background"), background->name());
    if (selectionStyle) json.insert(QStringLiteral("selectionStyle"), QLatin1String(selectionStyleName(*selectionStyle)));
    if (selectionColor) json.insert(QStringLiteral("selectionColor"), selectionColor->name());
    if (selectionWidth) json.insert(QStringLiteral("selectionWidth"), *selectionWidth);
    if (shading) json.insert(QStringLiteral("shading"), QLatin1String(shadingModeName(*shading)));
    if (complexity) json.insert(QStringLiteral("complexity"), QLatin1String(complexityName(*complexity)));
    for (const Switch& toggle : kSwitches) {
        if (const std::optional<bool>& value = this->*toggle.override) json.insert(QLatin1String(toggle.name), *value);
    }
    return json;
}

ViewportDisplayOverrides ViewportDisplayOverrides::fromJson(const QJsonObject& json) {
    ViewportDisplayOverrides overrides;
    if (const QJsonValue v = json.value(QStringLiteral("showGrid")); v.isBool()) overrides.showGrid = v.toBool();
    if (const QJsonValue v = json.value(QStringLiteral("gridSize")); v.isDouble() && positive(v.toDouble(), kMinGridSize, kMaxGridSize)) {
        overrides.gridSize = v.toDouble();
    }
    if (const QJsonValue v = json.value(QStringLiteral("gridStep")); v.isDouble() && positive(v.toDouble(), kMinGridStep, kMaxGridStep)) {
        overrides.gridStep = v.toDouble();
    }
    if (const QJsonValue v = json.value(QStringLiteral("background")); v.isString()) {
        const QColor color = QColor::fromString(v.toString());
        if (color.isValid()) overrides.background = color;
    }
    if (const QJsonValue v = json.value(QStringLiteral("selectionStyle")); v.isString()) {
        overrides.selectionStyle = selectionStyleFromName(v.toString());
    }
    if (const QJsonValue v = json.value(QStringLiteral("selectionColor")); v.isString()) {
        const QColor color = QColor::fromString(v.toString());
        if (color.isValid()) overrides.selectionColor = color;
    }
    if (const QJsonValue v = json.value(QStringLiteral("selectionWidth")); v.isDouble()) {
        const double width = v.toDouble();
        if (width == std::floor(width) && width >= kMinSelectionWidth && width <= kMaxSelectionWidth) {
            overrides.selectionWidth = static_cast<int>(width);
        }
    }
    if (const QJsonValue v = json.value(QStringLiteral("shading")); v.isString()) {
        overrides.shading = shadingModeFromName(v.toString());
    }
    if (const QJsonValue v = json.value(QStringLiteral("complexity")); v.isString()) {
        overrides.complexity = complexityFromName(v.toString());
    }
    for (const Switch& toggle : kSwitches) {
        if (const QJsonValue v = json.value(QLatin1String(toggle.name)); v.isBool()) overrides.*toggle.override = v.toBool();
    }
    return overrides;
}

}  // namespace usdcc::usd
